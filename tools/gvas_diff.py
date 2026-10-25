#!/usr/bin/env python3
"""Structural diff of two decrypted UE4 GVAS save files (see gvas.py).

Path syntax:
  A.B          struct member B of A
  A[i]         static-array element i (repeated property, ArrayIndex i);
               only shown when the property is repeated or i != 0
  A#i          element i of a dynamic ArrayProperty / SetProperty
  A{key}       MapProperty entry with the given key
  A:meta       property-tag metadata (type, struct name, enum name, ...)

Usage:
  gvas_diff.py A.sav B.sav [-o out.txt] [--blob-inline 64]
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gvas  # noqa: E402

BLOB_INLINE = 64


def _blob(b: bytes) -> str:
    if len(b) <= BLOB_INLINE:
        return f"hex:{b.hex()}"
    return f"blob len={len(b)} sha256={hashlib.sha256(b).hexdigest()[:16]}"


def _fmt(v) -> str:
    if isinstance(v, (bytes, bytearray)):
        return _blob(bytes(v))
    if isinstance(v, gvas.NStr):
        return f"{str(v)!r}<{v.enc}>"
    if isinstance(v, str):
        return repr(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, dict):
        return "{" + ", ".join(f"{k}={_fmt(x)}" for k, x in v.items()) + "}"
    return str(v)


class Rec:
    __slots__ = ("kind", "type", "value", "offset", "length", "info")

    def __init__(self, kind, typ, value, offset, length, info=None):
        self.kind, self.type, self.value = kind, typ, value
        self.offset, self.length, self.info = offset, length, info


def _meta(p: dict) -> str:
    parts = [p["type"]]
    for k in ("struct_type", "enum", "inner_type", "key_type", "value_type"):
        if k in p:
            parts.append(f"{k}={p[k]}")
    if p.get("struct_guid", "0" * 32) != "0" * 32:
        parts.append(f"struct_guid={p['struct_guid']}")
    if p.get("has_guid"):
        parts.append(f"guid={p.get('guid')}")
    if "inner_tag" in p:
        t = p["inner_tag"]
        parts.append(f"inner_tag={t['name']}/{t['struct_type']}")
    return " ".join(parts)


def _key_str(v) -> str:
    if isinstance(v, dict) and "kind" in v:  # struct key
        return _blob(_canon(v).encode())
    return _fmt(v)


def _canon(v) -> str:
    """Stable textual form of a value for hashing (ignores offsets)."""
    if isinstance(v, dict):
        return "{" + ",".join(f"{k}:{_canon(x)}" for k, x in v.items()
                              if k not in ("offset", "length", "value_offset")) + "}"
    if isinstance(v, list):
        return "[" + ",".join(_canon(x) for x in v) + "]"
    if isinstance(v, (bytes, bytearray)):
        return bytes(v).hex()
    return repr(v)


def flatten(tree: dict) -> dict[str, Rec]:
    out: dict[str, Rec] = {}
    h = tree["header"]
    ho = h["offset"]
    ev = h["engine_version"]
    out["<header>.save_game_version"] = Rec("leaf", "int32", h["save_game_version"], ho + 4, 4)
    out["<header>.package_version"] = Rec("leaf", "int32", h["package_version"], ho + 8, 4)
    out["<header>.engine_version"] = Rec(
        "leaf", "FEngineVersion",
        f"{ev['major']}.{ev['minor']}.{ev['patch']}-{ev['changelist']}+{ev['branch']}", ho + 12, None)
    out["<header>.custom_version_format"] = Rec("leaf", "int32", h["custom_version_format"], None, 4)
    for cv in h["custom_versions"]:
        out[f"<header>.custom_versions{{{cv['guid']}}}"] = Rec("leaf", "int32", cv["version"], cv.get("offset"), 20)
    out["<header>.save_game_class"] = Rec("leaf", "FString", h["save_game_class"],
                                         h.get("save_game_class_offset"), None)
    out["<header>.length"] = Rec("leaf", "int", h["length"], ho, h["length"])
    _props(out, "", tree["props"])
    out["<trailer>"] = Rec("leaf", "bytes", tree["trailer"], tree["none_end"], len(tree["trailer"]))
    return out


def _props(out, prefix, props):
    names = Counter(p["name"] for p in props)
    seen = Counter()
    for p in props:
        name, idx = p["name"], p["index"]
        key = f"{name}[{idx}]" if (names[name] > 1 or idx != 0) else name
        seen[key] += 1
        if seen[key] > 1:
            key += f"#dup{seen[key]}"
        path = f"{prefix}.{key}" if prefix else key
        _prop(out, path, p)


def _prop(out, path, p):
    typ = p["type"]
    out[path] = Rec("node", typ, None, p["offset"], p["length"], _meta(p))
    if "opaque" in p and "raw" in p:
        out[path + "=raw"] = Rec("leaf", typ, p["raw"], p["value_offset"], len(p["raw"]),
                                 f"opaque: {p['opaque']}")
        if typ == "BoolProperty":
            out[path + "=raw"].value = (p["value"], p["raw"])
        return
    if typ == "StructProperty":
        _struct(out, path, p["value"])
    elif typ == "ArrayProperty":
        out[path + "#count"] = Rec("leaf", "int32", p["count"], p["value_offset"], 4)
        if "elems" in p:
            for i, e in enumerate(p["elems"]):
                _value(out, f"{path}#{i}", p["inner_type"], e)
        else:
            out[path + "#raw"] = Rec("leaf", "bytes", p["elems_raw"], None, len(p["elems_raw"]),
                                     f"opaque: {p['opaque']}")
    elif typ == "SetProperty":
        out[path + "#count"] = Rec("leaf", "int32", len(p["elems"]), p["value_offset"] + 4, 4)
        for e in p["elems"]:
            _value(out, f"{path}{{{_key_str(e['value'])}}}", p["inner_type"], e)
        for e in p["removed"]:
            _value(out, f"{path}{{removed:{_key_str(e['value'])}}}", p["inner_type"], e)
    elif typ == "MapProperty":
        if "raw" in p:
            out[path + "=raw"] = Rec("leaf", "bytes", p["raw"], p["value_offset"], len(p["raw"]))
            return
        out[path + "#count"] = Rec("leaf", "int32", len(p["entries"]), None, 4)
        for e in p["entries"]:
            _value(out, f"{path}{{{_key_str(e['key']['value'])}}}", p["value_type"], e["value"])
        for e in p["removed"]:
            _value(out, f"{path}{{removed:{_key_str(e['value'])}}}", p["key_type"], e)
    else:
        out[path] = Rec("leaf", typ, p["value"], p["value_offset"], p["size"], _meta(p))


def _value(out, path, typ, e):
    v = e["value"]
    if typ == "StructProperty":
        out[path] = Rec("node", f"struct {v['struct']}", None, e["offset"], e["length"])
        _struct(out, path, v)
    else:
        out[path] = Rec("leaf", typ, v, e["offset"], e["length"])


def _struct(out, path, s):
    kind = s["kind"]
    if kind == "props":
        _props(out, path, s["props"])
    elif kind == "binary":
        # One leaf for the whole binary struct keeps the diff compact.
        out[path + "=" + s["struct"]] = Rec("leaf", s["struct"],
                                            {k: v for k, v in s["fields"].items()},
                                            s["offset"], s["length"])
    elif kind == "tags":
        out[path + "=tags"] = Rec("leaf", s["struct"], s["tags"], s["offset"], s["length"])
    else:
        out[path + "=raw"] = Rec("leaf", s["struct"], s["raw"], s["offset"], s["length"],
                                 f"opaque: {s['opaque']}")


def _loc(r: Rec | None) -> str:
    if r is None or r.offset is None:
        return "-"
    return f"0x{r.offset:x}+{r.length}" if r.length is not None else f"0x{r.offset:x}"


def _is_under(path: str, parent: str) -> bool:
    return len(path) > len(parent) and path.startswith(parent) and path[len(parent)] in ".[#{=:"


def diff(fa: dict[str, Rec], fb: dict[str, Rec]):
    """Yield (op, path, ra, rb) with op in '-', '+', '~'."""
    results = []
    covered: list[str] = []  # roots of added/removed subtrees
    # place b-only keys right after the preceding key in b that also exists in a
    merged = []
    pending = []
    last_anchor = None
    for k in fb:
        if k in fa:
            last_anchor = k
        else:
            pending.append((last_anchor, k))
    after: dict[str | None, list[str]] = {}
    for anchor, k in pending:
        after.setdefault(anchor, []).append(k)
    merged.extend(after.get(None, []))
    for k in fa:
        merged.append(k)
        merged.extend(after.get(k, []))
    for k in merged:
        if covered and _is_under(k, covered[-1]):
            continue
        ra, rb = fa.get(k), fb.get(k)
        if ra is None:
            results.append(("+", k, None, rb)); covered.append(k)
        elif rb is None:
            results.append(("-", k, ra, None)); covered.append(k)
        elif ra.kind == "node" or rb.kind == "node":
            if ra.kind != rb.kind or ra.info != rb.info or ra.type != rb.type:
                results.append(("~", k + ":meta", ra, rb))
        elif _canon(ra.value) != _canon(rb.value) or ra.type != rb.type:
            results.append(("~", k, ra, rb))
    return results


def _subtree_summary(flat: dict[str, Rec], root: str) -> str:
    n = sum(1 for k in flat if _is_under(k, root))
    return f"{n} descendant entries" if n else ""


def _val(r: Rec) -> str:
    if r.kind == "node":
        return f"<{r.info or r.type}>"
    return _fmt(r.value)


def render(a_path, b_path, ta, tb, results, fa, fb) -> str:
    lines = [f"--- {a_path}", f"+++ {b_path}",
             f"# header: {ta['header']['length']} vs {tb['header']['length']} bytes; "
             f"body {ta['none_end'] - ta['body_offset']} vs {tb['none_end'] - tb['body_offset']} bytes",
             f"# entries: {len(fa)} vs {len(fb)}; differences: "
             + ", ".join(f"{op}{c}" for op, c in Counter(r[0] for r in results).items()), ""]
    groups = Counter()
    for op, k, ra, rb in results:
        top = k.split(".")[0].split("#")[0].split("{")[0].split("=")[0].split(":")[0]
        groups[(top, op)] += 1
    if groups:
        lines.append("# by top-level property:")
        for (top, op), c in sorted(groups.items()):
            lines.append(f"#   {op} {top}: {c}")
        lines.append("")
    for op, k, ra, rb in results:
        if op == "~":
            typ = ra.type if ra.type == rb.type else f"{ra.type} -> {rb.type}"
            lines.append(f"~ {k}  ({typ})  {_val(ra)}  ->  {_val(rb)}   [a@{_loc(ra)} b@{_loc(rb)}]")
        elif op == "-":
            extra = _subtree_summary(fa, k)
            lines.append(f"- {k}  ({ra.info or ra.type})  {'' if ra.kind == 'node' else _val(ra)}"
                         f"{'  ' + extra if extra else ''}   [a@{_loc(ra)}]")
        else:
            extra = _subtree_summary(fb, k)
            lines.append(f"+ {k}  ({rb.info or rb.type})  {'' if rb.kind == 'node' else _val(rb)}"
                         f"{'  ' + extra if extra else ''}   [b@{_loc(rb)}]")
    return "\n".join(lines) + "\n"


def main(argv=None):
    global BLOB_INLINE
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("-o", "--output")
    ap.add_argument("--blob-inline", type=int, default=BLOB_INLINE,
                    help="show blobs up to this many bytes as hex (default 64)")
    args = ap.parse_args(argv)
    BLOB_INLINE = args.blob_inline
    ta, tb = gvas.load(args.a), gvas.load(args.b)
    fa, fb = flatten(ta), flatten(tb)
    res = diff(fa, fb)
    text = render(args.a, args.b, ta, tb, res, fa, fb)
    if args.output:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output).write_text(text, encoding="utf-8")
        print(f"wrote {args.output}: {len(res)} differences")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
