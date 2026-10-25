#!/usr/bin/env python3
"""Decrypt / encrypt DBZ Kakarot save files and dump the UE4 GVAS header.

Save files are UE4 `GVAS` SaveGame blobs, zero-padded to a 16-byte boundary and
AES-256-ECB encrypted in place with a fixed key that is embedded in the game
executable as an ASCII string (see docs/reverse-engineering-notes.md).

The key is never stored in this repository. Supply it either with
--key-file (a file whose contents are the key string) or with
--exe <AT-Win64-Shipping.exe> to locate it inside either build's game binary
(add --key-rva <hex> to read a known RVA instead of scanning; HD build: 5912980).
Only the first 32 bytes of the string are used.

Usage:
  kakarot_save.py decrypt --key-file K  --out DIR  FILE [FILE ...]
  kakarot_save.py encrypt --key-file K  --out DIR  FILE [FILE ...]
  kakarot_save.py header  --key-file K  FILE [FILE ...]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

GVAS_MAGIC = b"GVAS"


# --------------------------------------------------------------------------- key

def read_key_from_exe(exe: Path, rva: int) -> bytes:
    """Read a NUL-terminated ASCII key string from a PE file at a given RVA."""
    data = exe.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe_off:pe_off + 4] == b"PE\0\0", "not a PE file"
    num_sections = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec_off = pe_off + 24 + opt_size
    for i in range(num_sections):
        s = sec_off + i * 40
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, s + 8)
        if vaddr <= rva < vaddr + max(vsize, rsize):
            foff = rptr + (rva - vaddr)
            end = data.index(b"\0", foff)
            return data[foff:end]
    raise SystemExit(f"RVA {rva:#x} not inside any section of {exe}")


def scan_key_in_exe(exe: Path) -> bytes:
    """Find the key by its shape: the exe contains exactly one NUL-terminated run
    of 128 lowercase alphanumeric characters (true for both the standard and the
    HD build)."""
    import re
    data = exe.read_bytes()
    runs = [m.group(1) for m in re.finditer(rb"(?<![a-z0-9])([a-z0-9]{128})\x00", data)]
    if len(runs) != 1:
        raise SystemExit(f"expected exactly one 128-char key candidate in {exe}, found {len(runs)}")
    return runs[0]


def load_key(args) -> bytes:
    if getattr(args, "key_file", None):
        key = Path(args.key_file).read_bytes().strip(b"\r\n")
    elif getattr(args, "exe", None) and getattr(args, "key_rva", None):
        key = read_key_from_exe(Path(args.exe), int(args.key_rva, 16))
    elif getattr(args, "exe", None):
        key = scan_key_in_exe(Path(args.exe))
    else:
        raise SystemExit("need --key-file, or --exe (optionally with --key-rva)")
    # The embedded key string is 128 chars long, but FAES's 256-bit key schedule
    # only consumes the first 32 bytes (confirmed in rijndaelKeySetupEnc).
    if len(key) < 32:
        raise SystemExit(f"key is {len(key)} bytes, need at least 32 for AES-256")
    return key[:32]


# --------------------------------------------------------------------------- aes

def aes_ecb(key: bytes, data: bytes, encrypt: bool) -> bytes:
    if len(data) % 16:
        raise ValueError(f"data length {len(data)} is not a multiple of 16")
    c = Cipher(algorithms.AES(key), modes.ECB())
    op = c.encryptor() if encrypt else c.decryptor()
    return op.update(data) + op.finalize()


def pad16(data: bytes) -> bytes:
    rem = len(data) % 16
    return data + b"\0" * (16 - rem) if rem else data


# --------------------------------------------------------------------------- gvas

class Reader:
    def __init__(self, buf: bytes):
        self.buf = buf
        self.pos = 0

    def u8(self):  return self._unpack("<B")
    def u16(self): return self._unpack("<H")
    def i32(self): return self._unpack("<i")
    def u32(self): return self._unpack("<I")
    def i64(self): return self._unpack("<q")

    def _unpack(self, fmt):
        v = struct.unpack_from(fmt, self.buf, self.pos)[0]
        self.pos += struct.calcsize(fmt)
        return v

    def raw(self, n):
        v = self.buf[self.pos:self.pos + n]
        self.pos += n
        return v

    def fstring(self) -> str:
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:  # UTF-16
            n = -n
            s = self.raw(n * 2).decode("utf-16-le")
        else:
            s = self.raw(n).decode("utf-8", errors="replace")
        return s.rstrip("\0")

    def guid(self) -> str:
        a, b, c, d = struct.unpack_from("<IIII", self.buf, self.pos)
        self.pos += 16
        return f"{a:08X}-{b:08X}-{c:08X}-{d:08X}"


def parse_header(buf: bytes) -> dict:
    r = Reader(buf)
    magic = r.raw(4)
    if magic != GVAS_MAGIC:
        return {"magic": magic.hex(), "ok": False}
    h = {"magic": "GVAS", "ok": True}
    h["SaveGameFileVersion"] = r.i32()
    h["PackageFileUE4Version"] = r.i32()
    h["EngineVersion"] = {
        "Major": r.u16(), "Minor": r.u16(), "Patch": r.u16(),
        "Changelist": r.u32(), "Branch": r.fstring(),
    }
    if h["SaveGameFileVersion"] >= 2:
        h["CustomVersionFormat"] = r.i32()
        n = r.i32()
        h["CustomVersions"] = [(r.guid(), r.i32()) for _ in range(n)]
    h["SaveGameClassName"] = r.fstring()
    h["HeaderEnd"] = r.pos
    return h


# --------------------------------------------------------------------------- cli

def cmd_decrypt(args):
    key = load_key(args)
    out = Path(args.out); out.mkdir(parents=True, exist_ok=True)
    for f in map(Path, args.files):
        plain = aes_ecb(key, f.read_bytes(), encrypt=False)
        hdr = parse_header(plain)
        dst = out / (f.name + ".dec")
        dst.write_bytes(plain)
        status = "GVAS OK" if hdr["ok"] else f"NOT GVAS (magic {hdr['magic']})"
        print(f"{f}: {len(plain)} bytes -> {dst}  [{status}]")


def cmd_encrypt(args):
    key = load_key(args)
    out = Path(args.out); out.mkdir(parents=True, exist_ok=True)
    for f in map(Path, args.files):
        plain = pad16(f.read_bytes())
        enc = aes_ecb(key, plain, encrypt=True)
        name = f.name[:-4] if f.name.endswith(".dec") else f.name + ".enc"
        dst = out / name
        dst.write_bytes(enc)
        print(f"{f}: {len(enc)} bytes -> {dst}")


def cmd_header(args):
    key = load_key(args)
    for f in map(Path, args.files):
        raw = f.read_bytes()
        plain = raw if raw[:4] == GVAS_MAGIC else aes_ecb(key, raw, encrypt=False)
        hdr = parse_header(plain)
        print(f"== {f}  ({len(plain)} bytes)")
        for k, v in hdr.items():
            if k == "CustomVersions":
                print(f"  {k}: {len(v)} entries")
                for g, ver in v:
                    print(f"      {g} = {ver}")
            else:
                print(f"  {k}: {v}")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--key-file")
    p.add_argument("--exe")
    p.add_argument("--key-rva", help="hex RVA of the key string inside the exe")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, fn, needs_out in (("decrypt", cmd_decrypt, True), ("encrypt", cmd_encrypt, True), ("header", cmd_header, False)):
        sp = sub.add_parser(name)
        if needs_out:
            sp.add_argument("--out", required=True)
        sp.add_argument("files", nargs="+")
        sp.set_defaults(fn=fn)
    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main()
