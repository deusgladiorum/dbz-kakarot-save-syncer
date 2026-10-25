#!/usr/bin/env python3
"""Oracle test: the C++ CLI (kakarot_cli.exe) must produce byte-identical output
to the Python tools in this folder, and to the game's native transfer where a
native output exists.

The AES key is read from --exe by both sides at runtime and is never printed;
keys are compared through a 64-bit FNV-1a fingerprint only.

Usage:
  oracle_test.py --cli x64/Release/kakarot_cli.exe --exe STANDARD_EXE --saves SAVES_DIR
                 [--hd-exe HD_EXE] [--out out/oracle] [--python .venv/Scripts/python.exe]

SAVES_DIR is read only; every output goes under --out.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent

M1 = "_matrix_1_std_pre_transfer"
M2 = "_matrix_2_hd_post_transfer"
M3 = "_matrix_3_hd_post_transfer_new_save"
BACKPORT = "_20261012_backup_before_backport"


class Runner:
    def __init__(self, a):
        self.cli = str(Path(a.cli).resolve())
        self.py = str(Path(a.python).resolve())
        self.exe = a.exe
        self.saves = Path(a.saves)
        self.out = Path(a.out).resolve()
        self.results: list[tuple[str, bool, str]] = []

    def save(self, folder: str, name: str) -> Path:
        return self.saves / folder / name

    def run(self, argv: list[str]) -> None:
        p = subprocess.run(argv, capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError(f"{Path(argv[0]).name} {' '.join(argv[1:3])}... exited {p.returncode}: "
                               f"{(p.stderr or p.stdout).strip()[-300:]}")

    def python(self, script: str, *args) -> None:
        self.run([self.py, str(TOOLS / script), "--exe", self.exe, *map(str, args)])

    def cli_run(self, *args) -> None:
        self.run([self.cli, "--exe", self.exe, *map(str, args)])

    def case_dir(self, case: str, side: str) -> Path:
        d = self.out / side / re.sub(r"[^A-Za-z0-9_.-]+", "_", case)
        d.mkdir(parents=True, exist_ok=True)
        return d

    def record(self, case: str, ok: bool, detail: str = "") -> None:
        self.results.append((case, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'}  {case}{'  ' + detail if detail else ''}", flush=True)

    def guarded(self, case: str, fn) -> None:
        try:
            fn(case)
        except Exception as e:  # report and continue with the other cases
            self.record(case, False, f"error: {e}")


def first_diff(a: bytes, b: bytes) -> str:
    n = min(len(a), len(b))
    i = next((k for k in range(n) if a[k] != b[k]), n)
    return f"first difference at {i:#x} (sizes {len(a)} vs {len(b)})"


def compare(named: dict[str, bytes]) -> tuple[bool, str]:
    """All byte strings must be equal; describe the first mismatch otherwise."""
    items = list(named.items())
    base_name, base = items[0]
    for name, data in items[1:]:
        if data != base:
            return False, f"{name} != {base_name}: {first_diff(data, base)}"
    return True, f"{len(base)} bytes, {len(items)} outputs identical"


def fnv1a64(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


# ----------------------------------------------------------------------- cases

def transfer_case(r: Runner, src: Path, ref: Path, expected: Path | None):
    def fn(case):
        py_out = r.case_dir(case, "py") / src.name
        cli_out = r.case_dir(case, "cli") / src.name
        r.python("kakarot_transfer.py", "--ref", ref, "--out", py_out, src)
        r.cli_run("transfer", "--ref", ref, "--out", cli_out, src)
        named = {"cli": cli_out.read_bytes(), "python": py_out.read_bytes()}
        if expected is not None:
            named = {"expected": expected.read_bytes(), **named}
        r.record(case, *compare(named))
    return fn


def system_convert_case(r: Runner, src: Path, ref: Path):
    def fn(case):
        py_out = r.case_dir(case, "py") / src.name
        cli_out = r.case_dir(case, "cli") / src.name
        r.python("kakarot_system.py", "convert", "--ref", ref, "--out", py_out, src)
        r.cli_run("system-convert", "--ref", ref, "--out", cli_out, src)
        r.record(case, *compare({"python": py_out.read_bytes(), "cli": cli_out.read_bytes()}))
    return fn


def system_merge_case(r: Runner, src: Path, dst: Path, slots: str, expect_input: bool = False):
    def fn(case):
        py_out = r.case_dir(case, "py") / dst.name
        cli_out = r.case_dir(case, "cli") / dst.name
        r.python("kakarot_system.py", "merge-headers", "--src", src, "--dst", dst, "--slots", slots, "--out", py_out)
        r.cli_run("system-merge", "--src", src, "--dst", dst, "--slots", slots, "--out", cli_out)
        named = {"python": py_out.read_bytes(), "cli": cli_out.read_bytes()}
        if expect_input:
            named = {"input": dst.read_bytes(), **named}
        r.record(case, *compare(named))
    return fn


def roundtrip_case(r: Runner, src: Path):
    def fn(case):
        py_dir, cli_dir = r.case_dir(case, "py"), r.case_dir(case, "cli")
        r.python("kakarot_save.py", "decrypt", "--out", py_dir, src)
        r.cli_run("decrypt", "--out", cli_dir, src)
        py_dec, cli_dec = py_dir / (src.name + ".dec"), cli_dir / (src.name + ".dec")
        ok, detail = compare({"python decrypt": py_dec.read_bytes(), "cli decrypt": cli_dec.read_bytes()})
        if not ok:
            return r.record(case, False, detail)
        r.python("kakarot_save.py", "encrypt", "--out", py_dir / "enc", py_dec)
        r.cli_run("encrypt", "--out", cli_dir / "enc", cli_dec)
        r.record(case, *compare({"original": src.read_bytes(),
                                 "python encrypt": (py_dir / "enc" / src.name).read_bytes(),
                                 "cli encrypt": (cli_dir / "enc" / src.name).read_bytes()}))
    return fn


def keyscan_case(r: Runner, exes: list[str]):
    def fn(case):
        p = subprocess.run([r.cli, "keyscan", *exes], capture_output=True, text=True)
        prints = set(re.findall(r"fingerprint ([0-9a-f]{16})", p.stdout))
        # The Python scan runs in the venv interpreter and prints only fingerprints.
        code = ("import sys; sys.path.insert(0, sys.argv[1]); from pathlib import Path; "
                "from kakarot_save import scan_key_in_exe; from oracle_test import fnv1a64; "
                "[print(f'{fnv1a64(scan_key_in_exe(Path(e))[:32]):016x}') for e in sys.argv[2:]]")
        q = subprocess.run([r.py, "-c", code, str(TOOLS), *exes], capture_output=True, text=True)
        py_prints = set(q.stdout.split())
        ok = p.returncode == 0 and len(prints) == 1 and prints == py_prints
        r.record(case, ok, f"{len(exes)} exes, file and mapped-image scans agree with Python: {'yes' if ok else 'no'}")
    return fn


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", required=True, help="path to the built kakarot_cli.exe")
    ap.add_argument("--exe", required=True, help="standard-build AT-Win64-Shipping.exe (key source)")
    ap.add_argument("--hd-exe", help="HD-build exe; adds the keyscan case")
    ap.add_argument("--saves", required=True, help="folder holding the backup sets (read only)")
    ap.add_argument("--out", default=str(REPO / "out" / "oracle"))
    ap.add_argument("--python", default=str(REPO / ".venv" / "Scripts" / "python.exe"))
    a = ap.parse_args(argv)

    r = Runner(a)
    if r.out.exists():
        shutil.rmtree(r.out)
    s = r.save

    cases = [
        ("transfer std->HD ManualSave0",
         transfer_case(r, s(M1, "ManualSave0"), s(M3, "ManualSaveR0"), s(M2, "ManualSaveR0"))),
        ("transfer std->HD AutoSave0",
         transfer_case(r, s(M1, "AutoSave0"), s(M3, "ManualSaveR0"), s(M2, "AutoSaveR0"))),
        ("transfer HD->std ManualSaveR0 (reverse)",
         transfer_case(r, s(M2, "ManualSaveR0"), s(M1, "ManualSave0"), s(M1, "ManualSave0"))),
        ("system-convert std->HD SystemSave0",
         system_convert_case(r, s(M1, "SystemSave0"), s(M3, "SystemSaveR0"))),
        ("system-convert HD->std SystemSaveR0 (reverse)",
         system_convert_case(r, s(M2, "SystemSaveR0"), s(M1, "SystemSave0"))),
        ("system-merge backport R0->0 slots 0,10",
         system_merge_case(r, s(BACKPORT, "SystemSaveR0"), s(BACKPORT, "SystemSave0"), "0,10")),
        ("system-merge backport R0->0 slots all",
         system_merge_case(r, s(BACKPORT, "SystemSaveR0"), s(BACKPORT, "SystemSave0"), "all")),
        ("system-merge no-op SystemSave0 into itself",
         system_merge_case(r, s(BACKPORT, "SystemSave0"), s(BACKPORT, "SystemSave0"), "all", expect_input=True)),
        ("system-merge no-op SystemSaveR0 into itself",
         system_merge_case(r, s(BACKPORT, "SystemSaveR0"), s(BACKPORT, "SystemSaveR0"), "all", expect_input=True)),
    ]
    for folder in (M1, M2):
        for f in sorted((Path(a.saves) / folder).iterdir()):
            if f.is_file():
                cases.append((f"roundtrip {folder}/{f.name}", roundtrip_case(r, f)))
    if a.hd_exe:
        cases.append(("keyscan standard + HD", keyscan_case(r, [a.exe, a.hd_exe])))

    for name, fn in cases:
        r.guarded(name, fn)

    passed = sum(ok for _, ok, _ in r.results)
    print(f"\n{passed}/{len(r.results)} cases passed")
    return 0 if passed == len(r.results) else 1


if __name__ == "__main__":
    sys.exit(main())
