#!/usr/bin/env python3
"""Convert a DBZ Kakarot save between the standard and HD (Remaster) builds.

What the HD build's native "Transfer Save Data" does to a ManualSave/AutoSave
(verified byte-for-byte against the user's matrix_1 -> matrix_2 sets):

  1. decrypt (AES-256-ECB, fixed key)
  2. replace the engine part of the GVAS header (UE 4.21 -> UE 4.27, 48 -> 63
     custom versions) with the target build's header
  3. restamp the first property, `System.Version` (UInt32), with the target
     build's save-data version (HD currently 320, standard currently 560)
  4. zero-pad to 16 bytes and re-encrypt

Everything after the header is otherwise left untouched. The same transform in
the other direction (HD -> standard) is the obvious inverse; the game has no
native path for it so it is unverified against ground truth.

The target header and version are taken from a *reference* file: any save that
was natively written by the target build (the newest one in the save directory
is a good choice). This avoids hardcoding engine headers or version numbers that
change with game patches.

Usage:
  kakarot_transfer.py --key-file K --ref REF_SAVE --out OUT_FILE SRC_SAVE
  kakarot_transfer.py --key-file K --ref REF_SAVE --out OUT_FILE SRC_SAVE --expect NATIVE_OUTPUT
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kakarot_save import aes_ecb, load_key, pad16, parse_header, GVAS_MAGIC  # noqa: E402

TERMINATOR = b"\x05\x00\x00\x00None\x00"  # last top-level property name
VERSION_TAG = b"Version\x00\x0f\x00\x00\x00UInt32Property\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00"


def split_gvas(plain: bytes):
    """Return (engine_header, class_name_bytes, body, padding_len)."""
    h = parse_header(plain)
    if not h["ok"]:
        raise SystemExit("not a GVAS file after decryption (wrong key?)")
    # The engine header is everything before the SaveGameClassName FString.
    class_len = struct.unpack_from("<i", plain, h["HeaderEnd"] - 4 - len(h["SaveGameClassName"]) - 1)[0]
    class_start = h["HeaderEnd"] - 4 - class_len
    engine_header = plain[:class_start]
    class_bytes = plain[class_start:h["HeaderEnd"]]
    term = plain.rfind(TERMINATOR)
    if term < 0:
        raise SystemExit("could not find the 'None' terminator")
    body_end = term + len(TERMINATOR) + 4  # trailing uint32 0
    body = plain[h["HeaderEnd"]:body_end]
    return engine_header, class_bytes, body, len(plain) - body_end


def read_version(body: bytes) -> int:
    i = body.find(VERSION_TAG)
    if i < 0 or i > 256:
        raise SystemExit("could not find leading System/User.Version property")
    return struct.unpack_from("<I", body, i + len(VERSION_TAG))[0]


def write_version(body: bytes, version: int) -> bytes:
    i = body.find(VERSION_TAG)
    if i < 0 or i > 256:
        raise SystemExit("could not find leading System/User.Version property")
    off = i + len(VERSION_TAG)
    return body[:off] + struct.pack("<I", version) + body[off + 4:]


def transfer(key: bytes, src: bytes, ref: bytes) -> tuple[bytes, dict]:
    src_plain = aes_ecb(key, src, encrypt=False)
    ref_plain = aes_ecb(key, ref, encrypt=False)
    src_hdr, src_cls, src_body, src_pad = split_gvas(src_plain)
    ref_hdr, _, ref_body, _ = split_gvas(ref_plain)
    src_ver, ref_ver = read_version(src_body), read_version(ref_body)
    new_body = write_version(src_body, ref_ver)
    out_plain = pad16(ref_hdr + src_cls + new_body)
    info = {
        "src_engine": parse_header(src_plain)["EngineVersion"]["Branch"],
        "ref_engine": parse_header(ref_plain)["EngineVersion"]["Branch"],
        "src_version": src_ver, "ref_version": ref_ver,
        "class": src_cls[4:-1].decode(),
        "body_len": len(src_body), "src_pad": src_pad, "out_pad": len(out_plain) - len(ref_hdr + src_cls + new_body),
    }
    return aes_ecb(key, out_plain, encrypt=True), info


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--key-file")
    p.add_argument("--exe")
    p.add_argument("--key-rva")
    p.add_argument("--ref", required=True, help="a save natively written by the TARGET build")
    p.add_argument("--out", required=True)
    p.add_argument("--expect", help="native-transfer output to compare against (oracle)")
    p.add_argument("src")
    a = p.parse_args(argv)
    key = load_key(a)
    out, info = transfer(key, Path(a.src).read_bytes(), Path(a.ref).read_bytes())
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(out)
    print(f"{a.src} -> {a.out}")
    for k, v in info.items():
        print(f"  {k}: {v}")
    if a.expect:
        exp = Path(a.expect).read_bytes()
        same = exp == out
        print(f"  ORACLE: {'MATCH' if same else 'MISMATCH'}  (out {len(out)} B sha256 {hashlib.sha256(out).hexdigest()[:16]}, "
              f"expected {len(exp)} B sha256 {hashlib.sha256(exp).hexdigest()[:16]})")
        if not same:
            n = min(len(out), len(exp))
            i = next((k for k in range(n) if out[k] != exp[k]), n)
            print(f"  first differing byte at {i:#x}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
