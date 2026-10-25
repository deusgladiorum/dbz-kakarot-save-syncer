#!/usr/bin/env python3
"""Dependency-free reader/writer for decrypted UE4 ``GVAS`` save files.

Written for DBZ Kakarot saves (UE 4.21 standard build, UE 4.27 HD build) but
handles the generic UE4 tagged-property format.

The file is parsed into an ordered tree of plain dicts/lists so that it can be
dumped to JSON, diffed, edited and written back.  Every node records the byte
``offset`` and ``length`` it occupied in the source file.

Notable behaviour:

* C-style static arrays (``int Foo[N]``) are serialized by UE as N properties
  with the same name and ``ArrayIndex`` 0..N-1.  Properties are therefore kept
  as an ordered *list*, never a name-keyed dict, and the array index is kept.
* Property-tag ``Size`` fields are *recomputed* when writing, so a byte-exact
  round trip proves the value encodings were understood.
* A value that cannot be decoded is kept as an opaque byte blob (``"opaque"``
  key set to the reason) so the file can still be round-tripped.

CLI:
  gvas.py dump FILE [-o out.json] [--full]
  gvas.py roundtrip FILE [FILE ...]
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import struct
import sys
from pathlib import Path

__all__ = ["parse", "serialize", "load", "to_jsonable", "GvasError", "NStr"]


class GvasError(Exception):
    pass


class NStr(str):
    """A string whose on-disk FString encoding is not the canonical one.

    ``enc`` is one of ``"empty1"`` (length 1, just a NUL), ``"latin1"``
    (8-bit chars stored as ANSI), ``"utf16"`` (pure-ASCII stored as UTF-16),
    or ``"nonul"`` (no terminating NUL; raw bytes in ``raw``).
    """

    enc: str
    raw: bytes

    def __new__(cls, s: str, enc: str, raw: bytes = b""):
        o = super().__new__(cls, s)
        o.enc = enc
        o.raw = raw
        return o


# --------------------------------------------------------------------------- io

class Reader:
    def __init__(self, data: bytes, pos: int = 0):
        self.data = data
        self.pos = pos

    def need(self, n: int, end: int | None = None):
        lim = len(self.data) if end is None else end
        if n < 0 or self.pos + n > lim:
            raise GvasError(f"read of {n} bytes at {self.pos} overruns limit {lim}")

    def bytes(self, n: int) -> bytes:
        self.need(n)
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def unpack(self, fmt: str):
        s = struct.calcsize(fmt)
        self.need(s)
        v = struct.unpack_from("<" + fmt, self.data, self.pos)
        self.pos += s
        return v

    def i8(self): return self.unpack("b")[0]
    def u8(self): return self.unpack("B")[0]
    def i16(self): return self.unpack("h")[0]
    def u16(self): return self.unpack("H")[0]
    def i32(self): return self.unpack("i")[0]
    def u32(self): return self.unpack("I")[0]
    def i64(self): return self.unpack("q")[0]
    def u64(self): return self.unpack("Q")[0]

    def f32(self):
        raw = self.bytes(4)
        v = struct.unpack("<f", raw)[0]
        if struct.pack("<f", v) != raw:  # NaN payload not preserved by double
            return {"f32bits": struct.unpack("<I", raw)[0]}
        return v

    def f64(self):
        raw = self.bytes(8)
        v = struct.unpack("<d", raw)[0]
        if struct.pack("<d", v) != raw:
            return {"f64bits": struct.unpack("<Q", raw)[0]}
        return v

    def guid(self) -> str:
        return self.bytes(16).hex()

    def fstring(self) -> str:
        start = self.pos
        n = self.i32()
        if n == 0:
            return ""
        if n > 0:
            if n > 0x1000000:
                raise GvasError(f"implausible FString length {n} at {start}")
            raw = self.bytes(n)
            if raw[-1] != 0:
                return NStr(raw.decode("latin1"), "nonul", raw)
            s = raw[:-1].decode("latin1")
            if n == 1:
                return NStr("", "empty1")
            if any(ord(c) > 0x7F for c in s):
                return NStr(s, "latin1")
            return s
        n = -n
        if n > 0x1000000:
            raise GvasError(f"implausible FString length {-n} at {start}")
        raw = self.bytes(2 * n)
        if raw[-2:] != b"\0\0":
            return NStr(raw.decode("utf-16-le", "surrogatepass"), "nonul16", raw)
        s = raw[:-2].decode("utf-16-le", "surrogatepass")
        if s == "" or all(ord(c) <= 0x7F for c in s):
            return NStr(s, "utf16")
        return s


class Writer:
    def __init__(self):
        self.buf = bytearray()

    def bytes(self, b: bytes): self.buf += b
    def pack(self, fmt: str, *v): self.buf += struct.pack("<" + fmt, *v)
    def i8(self, v): self.pack("b", v)
    def u8(self, v): self.pack("B", v)
    def i16(self, v): self.pack("h", v)
    def u16(self, v): self.pack("H", v)
    def i32(self, v): self.pack("i", v)
    def u32(self, v): self.pack("I", v)
    def i64(self, v): self.pack("q", v)
    def u64(self, v): self.pack("Q", v)

    def f32(self, v):
        if isinstance(v, dict):
            self.pack("I", v["f32bits"])
        else:
            self.pack("f", v)

    def f64(self, v):
        if isinstance(v, dict):
            self.pack("Q", v["f64bits"])
        else:
            self.pack("d", v)

    def guid(self, g: str): self.buf += bytes.fromhex(g)

    def fstring(self, s: str):
        enc = getattr(s, "enc", None)
        if enc == "nonul":
            self.i32(len(s.raw)); self.buf += s.raw; return
        if enc == "nonul16":
            self.i32(-(len(s.raw) // 2)); self.buf += s.raw; return
        if enc == "empty1":
            self.i32(1); self.buf += b"\0"; return
        if s == "" and enc is None:
            self.i32(0); return
        if enc == "latin1" or (enc is None and all(ord(c) <= 0x7F for c in s)):
            b = s.encode("latin1") + b"\0"
            self.i32(len(b)); self.buf += b
        else:
            b = s.encode("utf-16-le", "surrogatepass") + b"\0\0"
            self.i32(-(len(b) // 2)); self.buf += b


# --------------------------------------------------------------------- structs

# Structs UE serializes natively (not as a tagged property list) in UE4.
# Field codes: f=float, d=double, i=int32, I=uint32, q=int64, B=uint8,
# g=guid, s=FString, plus nested struct names.
BINARY_STRUCTS: dict[str, list[tuple[str, str]]] = {
    "Vector": [("X", "f"), ("Y", "f"), ("Z", "f")],
    "Vector2D": [("X", "f"), ("Y", "f")],
    "Vector4": [("X", "f"), ("Y", "f"), ("Z", "f"), ("W", "f")],
    "Rotator": [("Pitch", "f"), ("Yaw", "f"), ("Roll", "f")],
    "Quat": [("X", "f"), ("Y", "f"), ("Z", "f"), ("W", "f")],
    "Plane": [("X", "f"), ("Y", "f"), ("Z", "f"), ("W", "f")],
    "LinearColor": [("R", "f"), ("G", "f"), ("B", "f"), ("A", "f")],
    "Color": [("B", "B"), ("G", "B"), ("R", "B"), ("A", "B")],
    "IntPoint": [("X", "i"), ("Y", "i")],
    "IntVector": [("X", "i"), ("Y", "i"), ("Z", "i")],
    "Box": [("Min", "Vector"), ("Max", "Vector"), ("IsValid", "B")],
    "Box2D": [("Min", "Vector2D"), ("Max", "Vector2D"), ("IsValid", "B")],
    "DateTime": [("Ticks", "q")],
    "Timespan": [("Ticks", "q")],
    "Guid": [("Guid", "g")],
    "FrameNumber": [("Value", "i")],
    "SoftObjectPath": [("AssetPathName", "s"), ("SubPathString", "s")],
    "SoftClassPath": [("AssetPathName", "s"), ("SubPathString", "s")],
    "Matrix": [(f"M{r}{c}", "f") for r in range(4) for c in range(4)],
}
# Tried only if parsing as a property list fails.
BINARY_FALLBACK_STRUCTS: dict[str, list[tuple[str, str]]] = {
    "Transform": [("Rotation", "Quat"), ("Translation", "Vector"), ("Scale3D", "Vector")],
}
SPECIAL_STRUCTS = {"GameplayTagContainer"}

_TICKS_EPOCH = _dt.datetime(1, 1, 1)


def _read_fields(r: Reader, spec) -> dict:
    out = {}
    for name, code in spec:
        if code == "f": out[name] = r.f32()
        elif code == "d": out[name] = r.f64()
        elif code == "i": out[name] = r.i32()
        elif code == "I": out[name] = r.u32()
        elif code == "q": out[name] = r.i64()
        elif code == "B": out[name] = r.u8()
        elif code == "g": out[name] = r.guid()
        elif code == "s": out[name] = r.fstring()
        else: out[name] = _read_fields(r, BINARY_STRUCTS[code])
    return out


def _write_fields(w: Writer, spec, vals: dict):
    for name, code in spec:
        v = vals[name]
        if code == "f": w.f32(v)
        elif code == "d": w.f64(v)
        elif code == "i": w.i32(v)
        elif code == "I": w.u32(v)
        elif code == "q": w.i64(v)
        elif code == "B": w.u8(v)
        elif code == "g": w.guid(v)
        elif code == "s": w.fstring(v)
        else: _write_fields(w, BINARY_STRUCTS[code], v)


def _annotate(struct_name: str, fields: dict):
    """Add human-readable, writer-ignored helper fields (prefixed with _)."""
    if struct_name == "DateTime":
        t = fields["Ticks"]
        try:
            fields["_iso"] = (_TICKS_EPOCH + _dt.timedelta(microseconds=t // 10)).isoformat()
        except (OverflowError, ValueError):
            pass
    elif struct_name == "Timespan":
        fields["_seconds"] = fields["Ticks"] / 1e7


def read_struct(r: Reader, struct_name: str, end: int | None) -> dict:
    """Read a struct body.  ``end`` is the exact end if known (top-level)."""
    start = r.pos
    node = {"struct": struct_name, "offset": start}

    def finish(kind, **kw):
        node["kind"] = kind
        node.update(kw)
        node["length"] = r.pos - start
        return node

    if struct_name in BINARY_STRUCTS:
        fields = _read_fields(r, BINARY_STRUCTS[struct_name])
        if end is None or r.pos == end:
            _annotate(struct_name, fields)
            return finish("binary", fields=fields)
        r.pos = start
    elif struct_name in SPECIAL_STRUCTS:  # GameplayTagContainer
        try:
            n = r.i32()
            tags = [r.fstring() for _ in range(n)]
            if end is None or r.pos == end:
                return finish("tags", tags=tags)
        except GvasError:
            pass
        r.pos = start
    try:
        props = read_props(r, end)
        if end is None or r.pos == end:
            return finish("props", props=props)
        err = f"property list ended at {r.pos}, expected {end}"
    except (GvasError, UnicodeDecodeError, KeyError, struct.error) as e:
        err = str(e)
    r.pos = start
    if struct_name in BINARY_FALLBACK_STRUCTS:
        try:
            fields = _read_fields(r, BINARY_FALLBACK_STRUCTS[struct_name])
            if end is None or r.pos == end:
                return finish("binary", fields=fields, binary_spec="fallback")
        except GvasError:
            pass
        r.pos = start
    if end is None:
        raise GvasError(f"cannot parse struct {struct_name} at {start}: {err}")
    raw = r.bytes(end - start)
    return finish("opaque", raw=raw, opaque=err)


def write_struct(w: Writer, node: dict):
    kind = node["kind"]
    name = node["struct"]
    if kind == "binary":
        spec = (BINARY_FALLBACK_STRUCTS if node.get("binary_spec") == "fallback"
                else BINARY_STRUCTS)[name]
        _write_fields(w, spec, node["fields"])
    elif kind == "tags":
        w.i32(len(node["tags"]))
        for t in node["tags"]:
            w.fstring(t)
    elif kind == "props":
        write_props(w, node["props"])
    elif kind == "opaque":
        w.bytes(node["raw"])
    else:
        raise GvasError(f"unknown struct kind {kind}")


# ------------------------------------------------------------------------ text

def read_text(r: Reader, end: int | None) -> dict:
    """FText.  History types None(-1) and Base(0) are decoded; others opaque."""
    start = r.pos
    flags = r.u32()
    htype = r.i8()
    t = {"flags": flags, "history": htype}
    if htype == -1:
        has = r.i32()
        t["has_invariant"] = has
        if has:
            t["invariant"] = r.fstring()
    elif htype == 0:
        t["namespace"] = r.fstring()
        t["key"] = r.fstring()
        t["source"] = r.fstring()
    else:
        if end is None:
            raise GvasError(f"unsupported FText history {htype} at {start}")
        t["raw"] = r.bytes(end - r.pos)
        t["opaque"] = f"FText history type {htype} not decoded"
    return t


def write_text(w: Writer, t: dict):
    w.u32(t["flags"]); w.i8(t["history"])
    if "raw" in t:
        w.bytes(t["raw"])
    elif t["history"] == -1:
        w.i32(t["has_invariant"])
        if t["has_invariant"]:
            w.fstring(t["invariant"])
    elif t["history"] == 0:
        w.fstring(t["namespace"]); w.fstring(t["key"]); w.fstring(t["source"])


# ---------------------------------------------------------- untagged elements

NUMERIC = {
    "IntProperty": "i32", "Int8Property": "i8", "Int16Property": "i16",
    "Int64Property": "i64", "UInt16Property": "u16", "UInt32Property": "u32",
    "UInt64Property": "u64", "FloatProperty": "f32", "DoubleProperty": "f64",
}
STRINGISH = {"StrProperty", "NameProperty", "EnumProperty", "ObjectProperty",
             "ClassProperty", "InterfaceProperty"}


def read_elem(r: Reader, typ: str, ctx: dict, end: int | None):
    """Read one untagged value (array/set/map element).

    ``ctx`` carries per-container decisions: ``struct`` (struct name, if
    known), ``byte_mode`` ("u8" or "name").
    """
    if typ in NUMERIC:
        return getattr(r, NUMERIC[typ])()
    if typ in STRINGISH:
        return r.fstring()
    if typ == "BoolProperty":
        return r.u8()
    if typ == "ByteProperty":
        return r.u8() if ctx.get("byte_mode", "u8") == "u8" else r.fstring()
    if typ in ("SoftObjectProperty", "SoftClassProperty"):
        return {"AssetPathName": r.fstring(), "SubPathString": r.fstring()}
    if typ == "TextProperty":
        return read_text(r, end)
    if typ == "StructProperty":
        return read_struct(r, ctx.get("struct") or "?", end)
    raise GvasError(f"unsupported element type {typ} at {r.pos}")


def write_elem(w: Writer, typ: str, v, ctx: dict):
    if typ in NUMERIC:
        getattr(w, NUMERIC[typ])(v)
    elif typ in STRINGISH:
        w.fstring(v)
    elif typ == "BoolProperty":
        w.u8(v)
    elif typ == "ByteProperty":
        if isinstance(v, int):
            w.u8(v)
        else:
            w.fstring(v)
    elif typ in ("SoftObjectProperty", "SoftClassProperty"):
        w.fstring(v["AssetPathName"]); w.fstring(v["SubPathString"])
    elif typ == "TextProperty":
        write_text(w, v)
    elif typ == "StructProperty":
        write_struct(w, v)
    else:
        raise GvasError(f"unsupported element type {typ}")


def _elem_node(r: Reader, typ, ctx, end):
    start = r.pos
    v = read_elem(r, typ, ctx, end)
    return {"offset": start, "length": r.pos - start, "value": v}


# ----------------------------------------------------------------- containers

def read_array(r: Reader, prop: dict, end: int):
    inner = prop["inner_type"]
    count = r.i32()
    prop["count"] = count
    if inner == "StructProperty":
        tstart = r.pos
        tag = {"name": r.fstring(), "type": r.fstring(), "size": r.i32(),
               "index": r.i32(), "struct_type": r.fstring(), "struct_guid": r.guid()}
        hg = r.u8()
        tag["has_guid"] = hg
        if hg:
            tag["guid"] = r.guid()
        tag["offset"] = tstart
        tag["length"] = r.pos - tstart
        if tag["type"] != "StructProperty":
            raise GvasError(f"array inner tag type {tag['type']} at {tstart}")
        prop["inner_tag"] = tag
        ctx = {"struct": tag["struct_type"]}
        estart = r.pos
        try:
            elems = [_elem_node(r, inner, ctx, None) for _ in range(count)]
            if r.pos != end:
                raise GvasError(f"struct array ended at {r.pos}, expected {end}")
        except (GvasError, UnicodeDecodeError, struct.error) as e:
            r.pos = estart
            prop["elems_raw"] = r.bytes(end - estart)
            prop["opaque"] = f"struct array elements: {e}"
            return
        prop["elems"] = elems
        return
    modes = [{}]
    if inner == "ByteProperty":
        modes = [{"byte_mode": "u8"}, {"byte_mode": "name"}]
    last = None
    estart = r.pos
    for ctx in modes:
        r.pos = estart
        try:
            elems = [_elem_node(r, inner, ctx, end if count == 1 else None)
                     for _ in range(count)]
            if r.pos == end:
                prop["elems"] = elems
                return
            last = f"array ended at {r.pos}, expected {end}"
        except (GvasError, UnicodeDecodeError, struct.error) as e:
            last = str(e)
    r.pos = estart
    prop["elems_raw"] = r.bytes(end - estart)
    prop["opaque"] = f"array elements: {last}"


def write_array(w: Writer, prop: dict):
    inner = prop["inner_type"]
    if "elems" in prop:
        w.i32(len(prop["elems"]))
    else:
        w.i32(prop["count"])
    if inner == "StructProperty":
        tag = prop["inner_tag"]
        body = Writer()
        if "elems" in prop:
            for e in prop["elems"]:
                write_elem(body, inner, e["value"], {})
        else:
            body.bytes(prop["elems_raw"])
        w.fstring(tag["name"]); w.fstring(tag["type"])
        w.i32(len(body.buf)); w.i32(tag["index"])
        w.fstring(tag["struct_type"]); w.guid(tag["struct_guid"])
        w.u8(tag["has_guid"])
        if tag["has_guid"]:
            w.guid(tag["guid"])
        w.bytes(body.buf)
        return
    if "elems" in prop:
        for e in prop["elems"]:
            write_elem(w, inner, e["value"], {})
    else:
        w.bytes(prop["elems_raw"])


def _container_modes(*types):
    """Candidate decoding contexts for untagged map/set elements."""
    opts = []
    for t in types:
        if t == "ByteProperty":
            opts.append([{"byte_mode": "u8"}, {"byte_mode": "name"}])
        elif t == "StructProperty":
            # The struct type is not recorded in the file; try a tagged
            # property list first, then 16-byte Guid, then other known
            # binary structs by name.
            opts.append([{"struct": "?"}, {"struct": "Guid"}, {"struct": "Vector"},
                         {"struct": "IntPoint"}, {"struct": "DateTime"}])
        else:
            opts.append([{}])
    return opts


def read_map(r: Reader, prop: dict, end: int):
    kt, vt = prop["key_type"], prop["value_type"]
    start = r.pos
    last = None
    kmodes, vmodes = _container_modes(kt, vt)
    for kctx in kmodes:
        for vctx in vmodes:
            r.pos = start
            try:
                nrem = r.i32()
                removed = [_elem_node(r, kt, kctx, None) for _ in range(nrem)]
                n = r.i32()
                entries = []
                for _ in range(n):
                    k = _elem_node(r, kt, kctx, None)
                    v = _elem_node(r, vt, vctx, None)
                    entries.append({"key": k, "value": v})
                if r.pos != end:
                    raise GvasError(f"map ended at {r.pos}, expected {end}")
                prop["removed"] = removed
                prop["entries"] = entries
                if kctx: prop["key_ctx"] = kctx
                if vctx: prop["value_ctx"] = vctx
                return
            except (GvasError, UnicodeDecodeError, struct.error) as e:
                last = str(e)
    r.pos = start
    prop["raw"] = r.bytes(end - start)
    prop["opaque"] = f"map: {last}"


def write_map(w: Writer, prop: dict):
    if "raw" in prop:
        w.bytes(prop["raw"]); return
    kt, vt = prop["key_type"], prop["value_type"]
    w.i32(len(prop["removed"]))
    for k in prop["removed"]:
        write_elem(w, kt, k["value"], {})
    w.i32(len(prop["entries"]))
    for e in prop["entries"]:
        write_elem(w, kt, e["key"]["value"], {})
        write_elem(w, vt, e["value"]["value"], {})


def read_set(r: Reader, prop: dict, end: int):
    it = prop["inner_type"]
    start = r.pos
    last = None
    for ctx in _container_modes(it)[0]:
        r.pos = start
        try:
            nrem = r.i32()
            removed = [_elem_node(r, it, ctx, None) for _ in range(nrem)]
            n = r.i32()
            elems = [_elem_node(r, it, ctx, None) for _ in range(n)]
            if r.pos != end:
                raise GvasError(f"set ended at {r.pos}, expected {end}")
            prop["removed"] = removed
            prop["elems"] = elems
            if ctx: prop["elem_ctx"] = ctx
            return
        except (GvasError, UnicodeDecodeError, struct.error) as e:
            last = str(e)
    r.pos = start
    prop["raw"] = r.bytes(end - start)
    prop["opaque"] = f"set: {last}"


def write_set(w: Writer, prop: dict):
    if "raw" in prop:
        w.bytes(prop["raw"]); return
    it = prop["inner_type"]
    w.i32(len(prop["removed"]))
    for e in prop["removed"]:
        write_elem(w, it, e["value"], {})
    w.i32(len(prop["elems"]))
    for e in prop["elems"]:
        write_elem(w, it, e["value"], {})


# ------------------------------------------------------------------ properties

def read_prop(r: Reader, limit: int | None):
    """Read one tagged property.  Returns None at the ``None`` terminator."""
    start = r.pos
    name = r.fstring()
    if name == "None":
        return None
    typ = r.fstring()
    size = r.i32()
    index = r.i32()
    p = {"name": name, "type": typ, "index": index, "offset": start}
    if typ == "StructProperty":
        p["struct_type"] = r.fstring()
        p["struct_guid"] = r.guid()
    elif typ == "BoolProperty":
        p["value"] = r.u8()
    elif typ in ("ByteProperty", "EnumProperty"):
        p["enum"] = r.fstring()
    elif typ in ("ArrayProperty", "SetProperty"):
        p["inner_type"] = r.fstring()
    elif typ == "MapProperty":
        p["key_type"] = r.fstring()
        p["value_type"] = r.fstring()
    hg = r.u8()
    p["has_guid"] = hg
    if hg:
        p["guid"] = r.guid()
    vstart = r.pos
    end = vstart + size
    if size < 0 or (limit is not None and end > limit) or end > len(r.data):
        raise GvasError(f"property {name}:{typ} at {start} size {size} overruns")
    p["value_offset"] = vstart
    p["size"] = size
    try:
        _read_value(r, p, typ, size, end)
        if r.pos != end:
            raise GvasError(f"value ended at {r.pos}, expected {end}")
    except (GvasError, UnicodeDecodeError, struct.error) as e:
        r.pos = vstart
        for k in ("value", "elems", "elems_raw", "entries", "removed", "raw"):
            if k in p and not (typ == "BoolProperty" and k == "value"):
                del p[k]
        p["raw"] = r.bytes(size)
        p["opaque"] = str(e)
    p["length"] = r.pos - start
    return p


def _read_value(r: Reader, p: dict, typ: str, size: int, end: int):
    if typ in NUMERIC:
        p["value"] = getattr(r, NUMERIC[typ])()
    elif typ == "BoolProperty":
        pass
    elif typ == "ByteProperty":
        p["value"] = r.u8() if size == 1 else r.fstring()
    elif typ in STRINGISH:
        p["value"] = r.fstring()
    elif typ in ("SoftObjectProperty", "SoftClassProperty"):
        p["value"] = {"AssetPathName": r.fstring(), "SubPathString": r.fstring()}
    elif typ == "TextProperty":
        p["value"] = read_text(r, end)
    elif typ == "StructProperty":
        p["value"] = read_struct(r, p["struct_type"], end)
    elif typ == "ArrayProperty":
        read_array(r, p, end)
    elif typ == "MapProperty":
        read_map(r, p, end)
    elif typ == "SetProperty":
        read_set(r, p, end)
    else:
        raise GvasError(f"unknown property type {typ}")


def read_props(r: Reader, limit: int | None) -> list:
    props = []
    while True:
        p = read_prop(r, limit)
        if p is None:
            return props
        props.append(p)


def write_prop(w: Writer, p: dict):
    typ = p["type"]
    body = Writer()
    if "raw" in p and "opaque" in p and typ not in ("MapProperty", "SetProperty"):
        body.bytes(p["raw"])
    elif typ in NUMERIC:
        getattr(body, NUMERIC[typ])(p["value"])
    elif typ == "BoolProperty":
        pass
    elif typ == "ByteProperty":
        write_elem(body, typ, p["value"], {})
    elif typ in STRINGISH:
        body.fstring(p["value"])
    elif typ in ("SoftObjectProperty", "SoftClassProperty", "TextProperty"):
        write_elem(body, typ, p["value"], {})
    elif typ == "StructProperty":
        write_struct(body, p["value"])
    elif typ == "ArrayProperty":
        write_array(body, p)
    elif typ == "MapProperty":
        write_map(body, p)
    elif typ == "SetProperty":
        write_set(body, p)
    else:
        raise GvasError(f"cannot write property type {typ}")
    w.fstring(p["name"]); w.fstring(typ)
    w.i32(len(body.buf)); w.i32(p["index"])
    if typ == "StructProperty":
        w.fstring(p["struct_type"]); w.guid(p["struct_guid"])
    elif typ == "BoolProperty":
        w.u8(p["value"])
    elif typ in ("ByteProperty", "EnumProperty"):
        w.fstring(p["enum"])
    elif typ in ("ArrayProperty", "SetProperty"):
        w.fstring(p["inner_type"])
    elif typ == "MapProperty":
        w.fstring(p["key_type"]); w.fstring(p["value_type"])
    w.u8(p["has_guid"])
    if p["has_guid"]:
        w.guid(p["guid"])
    w.bytes(body.buf)


def write_props(w: Writer, props: list):
    for p in props:
        write_prop(w, p)
    w.fstring("None")


# ---------------------------------------------------------------------- header

def read_header(r: Reader) -> dict:
    h = {"offset": r.pos}
    magic = r.bytes(4)
    if magic != b"GVAS":
        raise GvasError(f"bad magic {magic!r}")
    h["save_game_version"] = r.i32()
    h["package_version"] = r.i32()
    if h["save_game_version"] >= 3:
        h["package_version_ue5"] = r.i32()
    h["engine_version"] = {"major": r.u16(), "minor": r.u16(), "patch": r.u16(),
                           "changelist": r.u32(), "branch": r.fstring()}
    h["custom_version_format"] = r.i32()
    n = r.i32()
    cvs = []
    for _ in range(n):
        o = r.pos
        cvs.append({"guid": r.guid(), "version": r.i32(), "offset": o})
    h["custom_versions"] = cvs
    h["save_game_class_offset"] = r.pos
    h["save_game_class"] = r.fstring()
    h["length"] = r.pos - h["offset"]
    return h


def write_header(w: Writer, h: dict):
    w.bytes(b"GVAS")
    w.i32(h["save_game_version"]); w.i32(h["package_version"])
    if h["save_game_version"] >= 3:
        w.i32(h["package_version_ue5"])
    ev = h["engine_version"]
    w.u16(ev["major"]); w.u16(ev["minor"]); w.u16(ev["patch"])
    w.u32(ev["changelist"]); w.fstring(ev["branch"])
    w.i32(h["custom_version_format"])
    w.i32(len(h["custom_versions"]))
    for cv in h["custom_versions"]:
        w.guid(cv["guid"]); w.i32(cv["version"])
    w.fstring(h["save_game_class"])


# ------------------------------------------------------------------------ file

def parse(data: bytes) -> dict:
    r = Reader(data)
    header = read_header(r)
    body_start = r.pos
    props = read_props(r, len(data))
    none_end = r.pos
    trailer = r.bytes(min(4, len(data) - r.pos))
    padding = data[r.pos:]
    return {
        "header": header,
        "body_offset": body_start,
        "props": props,
        "none_end": none_end,
        "trailer": trailer,
        "padding_len": len(padding),
        "padding_all_zero": not any(padding),
        "file_size": len(data),
    }


def serialize(tree: dict, pad: bool = False) -> bytes:
    """Serialize back to bytes (up to and including the 4-byte trailer).

    With ``pad=True`` the output is zero-padded to a 16-byte multiple, as the
    game does before encryption.
    """
    w = Writer()
    write_header(w, tree["header"])
    write_props(w, tree["props"])
    w.bytes(tree["trailer"])
    out = bytes(w.buf)
    if pad and len(out) % 16:
        out += b"\0" * (16 - len(out) % 16)
    return out


def load(path) -> dict:
    return parse(Path(path).read_bytes())


# ------------------------------------------------------------------------ json

def _blob(b: bytes, full: bool):
    if full or len(b) <= 256:
        return {"hex": b.hex(), "len": len(b)}
    return {"hex": b[:256].hex(), "len": len(b), "truncated": True}


def to_jsonable(o, full: bool = False):
    if isinstance(o, (bytes, bytearray)):
        return _blob(bytes(o), full)
    if isinstance(o, NStr):
        return {"str": str(o), "enc": o.enc}
    if isinstance(o, dict):
        return {k: to_jsonable(v, full) for k, v in o.items()}
    if isinstance(o, list):
        return [to_jsonable(v, full) for v in o]
    if isinstance(o, float) and not math.isfinite(o):
        return repr(o)
    return o


def iter_opaque(o, path="$"):
    """Yield (path, reason) for every node that was kept as an opaque blob."""
    if isinstance(o, dict):
        if "opaque" in o:
            yield path, o["opaque"]
        for k, v in o.items():
            yield from iter_opaque(v, f"{path}.{k}")
    elif isinstance(o, list):
        for i, v in enumerate(o):
            yield from iter_opaque(v, f"{path}[{i}]")


# ------------------------------------------------------------------------- cli

def cmd_dump(args):
    tree = load(args.file)
    js = json.dumps(to_jsonable(tree, args.full), indent=1, ensure_ascii=False)
    if args.output:
        Path(args.output).write_text(js, encoding="utf-8")
        print(f"wrote {args.output}")
    else:
        sys.stdout.write(js + "\n")


def cmd_roundtrip(args):
    ok_all = True
    for f in args.files:
        data = Path(f).read_bytes()
        tree = parse(data)
        out = serialize(tree)
        cmp_end = tree["none_end"] + len(tree["trailer"])
        ref = data[:cmp_end]
        ok = out == ref
        ok_all &= ok
        pad = data[cmp_end:]
        opaque = list(iter_opaque(tree))
        msg = (f"{f}: {'OK' if ok else 'MISMATCH'}  compared={cmp_end} bytes, "
               f"trailer={tree['trailer'].hex()}, padding={len(pad)} bytes "
               f"({'all zero' if not any(pad) else 'NON-ZERO'}), opaque nodes={len(opaque)}")
        print(msg)
        if not ok:
            n = min(len(out), len(ref))
            diff_at = next((i for i in range(n) if out[i] != ref[i]), n)
            print(f"  first difference at byte {diff_at} (out len {len(out)}, ref len {len(ref)})")
        for p, why in opaque[:20]:
            print(f"  opaque: {p}: {why}")
    return 0 if ok_all else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump", help="dump parsed tree as JSON")
    d.add_argument("file")
    d.add_argument("-o", "--output")
    d.add_argument("--full", action="store_true", help="do not truncate blobs")
    d.set_defaults(fn=cmd_dump)
    rt = sub.add_parser("roundtrip", help="parse+serialize and compare")
    rt.add_argument("files", nargs="+")
    rt.set_defaults(fn=cmd_roundtrip)
    args = ap.parse_args(argv)
    return args.fn(args) or 0


if __name__ == "__main__":
    sys.exit(main())
