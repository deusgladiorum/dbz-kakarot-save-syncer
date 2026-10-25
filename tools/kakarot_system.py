#!/usr/bin/env python3
"""SystemSave (ATSaveSystem) tools for DBZ Kakarot standard <-> HD sync.

The SystemSave file holds, among other things, the per-slot `Headers[slot]`
structs that the in-game save list displays (level, chapter, play time,
timestamp, character, progress id and the thumbnail). ManualSave/AutoSave files
do not carry that metadata themselves, which is why copying only those files
leaves the save list showing stale thumbnails and play times.

Slot indices follow the game's ESaveGameSlot enum: ManualSave0..9 = 0..9,
AutoSave0 = 10, SystemSave0 = 11.

Two operations:

  convert        Full conversion of a SystemSave to the other build, matching
                 what the HD build's native "Transfer Save Data" produces:
                 header swap, `User.Version` restamp, drop fields the target
                 build does not have, add/remove `IsConversion`.
                 (Changes the EULA / notice flags' container, so use only when
                 the target build has no SystemSave of its own yet.)

  merge-headers  Copy `Headers[slot]` entries from a source SystemSave into a
                 destination SystemSave of the other build, leaving the
                 destination's User/Option/DLC/etc. untouched. This is what the
                 sync plugin should do after mirroring a ManualSave/AutoSave.

Both read the AES key from --key-file or --exe/--key-rva (see kakarot_save.py).
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gvas  # noqa: E402
from kakarot_save import aes_ecb, load_key  # noqa: E402

# Fields present in the standard build's ATSaveSystemUser that the HD build does
# not have (it drops them when it re-saves). Observed from the native transfer.
STD_ONLY_USER_FIELDS = {
    "IsPrivacyPolicyW180",
    "IsKpiNoticeW180",
    "IsAlreadyUpdatedLinkedBonusesForPatchOneDotSeven",
}
HD_ONLY_TOP_FIELDS = {"IsConversion"}


def is_hd(tree: dict) -> bool:
    return tree["header"]["engine_version"]["minor"] >= 27


def find(props: list, name: str, index: int = 0):
    for p in props:
        if p["name"] == name and p["index"] == index:
            return p
    return None


def user_field(tree: dict, name: str):
    return find(find(tree["props"], "User")["value"]["props"], name)


def load_tree(key: bytes, path: Path) -> dict:
    return gvas.parse(aes_ecb(key, path.read_bytes(), encrypt=False))


def save_tree(key: bytes, tree: dict, path: Path) -> bytes:
    out = aes_ecb(key, gvas.serialize(tree, pad=True), encrypt=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)
    return out


# ------------------------------------------------------------------- convert

def convert(src: dict, ref: dict, tcg_notice: int | None = None) -> dict:
    out = copy.deepcopy(src)
    # 1. engine header from the reference, keep our own class name
    hdr = copy.deepcopy(ref["header"])
    hdr["save_game_class"] = src["header"]["save_game_class"]
    out["header"] = hdr
    # 2. User.Version restamp
    user_field(out, "Version")["value"] = user_field(ref, "Version")["value"]
    to_hd = is_hd(ref)
    user_props = find(out["props"], "User")["value"]["props"]
    if to_hd:
        # 3. drop standard-only fields
        user_props[:] = [p for p in user_props if p["name"] not in STD_ONLY_USER_FIELDS]
        # 4. IsConversion = true, placed after DLC as the HD build serialises it
        if find(out["props"], "IsConversion") is None:
            node = {"name": "IsConversion", "type": "BoolProperty", "index": 0,
                    "value": 1, "has_guid": 0, "size": 0}
            pos = next(i for i, p in enumerate(out["props"]) if p["name"] == "DLC") + 1
            out["props"].insert(pos, node)
    else:
        out["props"][:] = [p for p in out["props"] if p["name"] not in HD_ONLY_TOP_FIELDS]
    if tcg_notice is not None:
        user_field(out, "NewTCGNoticesFlag")["value"] = tcg_notice
    return out


# ------------------------------------------------------------- merge-headers

def merge_headers(src: dict, dst: dict, slots: list[int]) -> dict:
    out = copy.deepcopy(dst)
    props = out["props"]
    src_headers = {p["index"]: p for p in src["props"] if p["name"] == "Headers"}
    for slot in slots:
        if slot not in src_headers:
            raise SystemExit(f"source SystemSave has no Headers[{slot}]")
        new = copy.deepcopy(src_headers[slot])
        existing = find(props, "Headers", slot)
        if existing is not None:
            props[props.index(existing)] = new
        else:
            # keep Headers entries contiguous and ordered by slot index
            idxs = [i for i, p in enumerate(props) if p["name"] == "Headers"]
            if idxs:
                after = [i for i in idxs if props[i]["index"] < slot]
                pos = (after[-1] + 1) if after else idxs[0]
            else:
                pos = next(i for i, p in enumerate(props) if p["name"] == "Option") + 1
            props.insert(pos, new)
    return out


def remove_headers(dst: dict, slots: list[int]) -> dict:
    out = copy.deepcopy(dst)
    out["props"][:] = [p for p in out["props"] if not (p["name"] == "Headers" and p["index"] in slots)]
    return out


# ----------------------------------------------------------------------- cli

def describe(tree: dict) -> str:
    ev = tree["header"]["engine_version"]
    hs = sorted(p["index"] for p in tree["props"] if p["name"] == "Headers")
    return (f"UE {ev['major']}.{ev['minor']}, User.Version={user_field(tree, 'Version')['value']}, "
            f"Headers slots={hs}, IsConversion={'yes' if find(tree['props'], 'IsConversion') else 'no'}")


def check_oracle(out: bytes, expect: Path) -> int:
    exp = expect.read_bytes()
    same = exp == out
    print(f"  ORACLE: {'MATCH' if same else 'MISMATCH'}  (out {len(out)} B {hashlib.sha256(out).hexdigest()[:16]}, "
          f"expected {len(exp)} B {hashlib.sha256(exp).hexdigest()[:16]})")
    if not same:
        n = min(len(out), len(exp))
        i = next((k for k in range(n) if out[k] != exp[k]), n)
        print(f"  first differing byte at {i:#x}")
        return 1
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--key-file"); p.add_argument("--exe"); p.add_argument("--key-rva")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("convert")
    c.add_argument("--ref", required=True, help="SystemSave natively written by the TARGET build")
    c.add_argument("--out", required=True)
    c.add_argument("--tcg-notice", type=int, help="override User.NewTCGNoticesFlag (oracle testing)")
    c.add_argument("--expect")
    c.add_argument("src")

    m = sub.add_parser("merge-headers")
    m.add_argument("--src", required=True, help="SystemSave to take Headers[slot] from")
    m.add_argument("--dst", required=True, help="SystemSave to merge into (other build)")
    m.add_argument("--slots", required=True, help="comma-separated slot indices, or 'all'")
    m.add_argument("--out", required=True)
    m.add_argument("--expect")

    i = sub.add_parser("info")
    i.add_argument("files", nargs="+")

    a = p.parse_args(argv)
    key = load_key(a)

    if a.cmd == "info":
        for f in a.files:
            print(f"{f}: {describe(load_tree(key, Path(f)))}")
        return 0

    if a.cmd == "convert":
        src, ref = load_tree(key, Path(a.src)), load_tree(key, Path(a.ref))
        print(f"src: {describe(src)}\nref: {describe(ref)}")
        out_tree = convert(src, ref, a.tcg_notice)
        out = save_tree(key, out_tree, Path(a.out))
        print(f"out: {describe(out_tree)} -> {a.out} ({len(out)} B)")
        return check_oracle(out, Path(a.expect)) if a.expect else 0

    if a.cmd == "merge-headers":
        src, dst = load_tree(key, Path(a.src)), load_tree(key, Path(a.dst))
        print(f"src: {describe(src)}\ndst: {describe(dst)}")
        if a.slots == "all":
            slots = sorted(p["index"] for p in src["props"] if p["name"] == "Headers")
        else:
            slots = [int(s) for s in a.slots.split(",")]
        out_tree = merge_headers(src, dst, slots)
        out = save_tree(key, out_tree, Path(a.out))
        print(f"out: {describe(out_tree)} (merged slots {slots}) -> {a.out} ({len(out)} B)")
        return check_oracle(out, Path(a.expect)) if a.expect else 0


if __name__ == "__main__":
    sys.exit(main())
