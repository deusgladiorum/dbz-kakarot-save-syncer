#include "gvas.h"

#include <cstring>
#include <stdexcept>

namespace kakarot {
namespace gvas {
namespace {

// Bounds-checked little-endian reader. Throws std::runtime_error on overrun;
// the public functions below catch it and report it through `error`.
class Reader {
public:
    Reader(const Bytes& data, size_t pos, size_t limit) : data_(data), pos_(pos), limit_(limit) {
        if (limit_ > data_.size()) limit_ = data_.size();
    }

    size_t pos() const { return pos_; }
    void seek(size_t pos) {
        if (pos > limit_) fail("seek past end");
        pos_ = pos;
    }

    const uint8_t* take(size_t n) {
        if (n > limit_ - pos_) fail("read past end");
        const uint8_t* p = data_.data() + pos_;
        pos_ += n;
        return p;
    }

    uint8_t u8() { return *take(1); }
    uint16_t u16() { return static_cast<uint16_t>(scalar(2)); }
    uint32_t u32() { return static_cast<uint32_t>(scalar(4)); }
    int32_t i32() { return static_cast<int32_t>(u32()); }

    // FString: int32 length; > 0 is that many 8-bit chars including the NUL,
    // < 0 is that many UTF-16 units including the NUL. Non-ASCII UTF-16 units
    // become '?', which is enough for comparing property names.
    std::string fstring() {
        int32_t n = i32();
        if (n == 0) return std::string();
        std::string s;
        if (n > 0) {
            if (n > 0x1000000) fail("implausible FString length");
            const uint8_t* p = take(static_cast<size_t>(n));
            s.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
        } else {
            if (n < -0x1000000) fail("implausible FString length");
            size_t units = static_cast<size_t>(-static_cast<int64_t>(n));
            const uint8_t* p = take(units * 2);
            for (size_t i = 0; i < units; ++i) {
                uint16_t ch = static_cast<uint16_t>(p[2 * i] | (p[2 * i + 1] << 8));
                s.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
            }
        }
        if (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string(what) + " at offset " + std::to_string(pos_));
    }

private:
    uint64_t scalar(size_t n) {
        const uint8_t* p = take(n);
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
        return v;
    }

    const Bytes& data_;
    size_t pos_;
    size_t limit_;
};

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

void put_i32(Bytes& out, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(u >> (8 * i)));
}

void put_fstring(Bytes& out, const std::string& s) {
    put_i32(out, static_cast<int32_t>(s.size() + 1));
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
}

void store_u32(Bytes& plain, size_t offset, uint32_t v) {
    for (int i = 0; i < 4; ++i) plain[offset + i] = static_cast<uint8_t>(v >> (8 * i));
}

void read_header_impl(const Bytes& plain, Header& h) {
    Reader r(plain, 0, plain.size());
    if (std::memcmp(r.take(4), "GVAS", 4) != 0) r.fail("bad GVAS magic");
    h.save_game_version = r.i32();
    h.package_version = r.i32();
    if (h.save_game_version >= 3) h.package_version_ue5 = r.i32();
    h.engine.major = r.u16();
    h.engine.minor = r.u16();
    h.engine.patch = r.u16();
    h.engine.changelist = r.u32();
    h.engine.branch = r.fstring();
    h.custom_version_format = r.i32();
    int32_t count = r.i32();
    if (count < 0 || count > 0x10000) r.fail("implausible custom version count");
    h.custom_versions.resize(static_cast<size_t>(count));
    for (CustomVersion& cv : h.custom_versions) {
        std::memcpy(cv.guid.data(), r.take(16), 16);
        cv.version = r.i32();
    }
    h.class_name_offset = r.pos();
    h.save_game_class = r.fstring();
    h.body_offset = r.pos();
}

void read_properties_impl(const Bytes& plain, size_t begin, size_t limit, PropertyList& list) {
    Reader r(plain, begin, limit);
    list.properties.clear();
    for (;;) {
        Property p;
        p.offset = r.pos();
        p.name = r.fstring();
        if (p.name == "None") {
            list.none_offset = p.offset;
            list.none_end = r.pos();
            return;
        }
        p.type = r.fstring();
        p.size_offset = r.pos();
        p.size = r.i32();
        p.array_index = r.i32();
        if (p.type == "StructProperty") {
            r.fstring();  // struct type name
            r.take(16);   // struct GUID
        } else if (p.type == "BoolProperty") {
            r.u8();       // the bool value lives in the tag
        } else if (p.type == "ByteProperty" || p.type == "EnumProperty") {
            r.fstring();  // enum name
        } else if (p.type == "ArrayProperty" || p.type == "SetProperty") {
            r.fstring();  // inner type
        } else if (p.type == "MapProperty") {
            r.fstring();  // key type
            r.fstring();  // value type
        }
        if (r.u8()) r.take(16);  // optional property GUID
        p.value_offset = r.pos();
        if (p.size < 0) r.fail("negative property size");
        r.seek(p.value_offset + static_cast<size_t>(p.size));
        list.properties.push_back(std::move(p));
    }
}

void parse_layout_impl(const Bytes& plain, Layout& layout) {
    read_header_impl(plain, layout.header);
    read_properties_impl(plain, layout.header.body_offset, plain.size(), layout.top);
    size_t end = layout.top.none_end + 4;  // trailing uint32 after the terminator
    layout.body_end = end < plain.size() ? end : plain.size();
}

// Value offset of the leading Version UInt32Property inside the first top-level struct.
size_t leading_version_offset(const Bytes& plain) {
    Layout layout;
    parse_layout_impl(plain, layout);
    if (layout.top.properties.empty() || layout.top.properties[0].type != "StructProperty")
        throw std::runtime_error("first top-level property is not a struct");
    const Property& first = layout.top.properties[0];
    PropertyList fields;
    read_properties_impl(plain, first.value_offset, first.end(), fields);
    int i = find_property(fields, "Version", 0);
    if (i < 0) throw std::runtime_error("no Version field in " + first.name);
    const Property& v = fields.properties[static_cast<size_t>(i)];
    if (v.type != "UInt32Property" || v.size != 4)
        throw std::runtime_error(first.name + ".Version is not a UInt32Property");
    return v.value_offset;
}

void splice(Bytes& plain, size_t begin, size_t end, const Bytes& with) {
    plain.erase(plain.begin() + static_cast<ptrdiff_t>(begin), plain.begin() + static_cast<ptrdiff_t>(end));
    plain.insert(plain.begin() + static_cast<ptrdiff_t>(begin), with.begin(), with.end());
}

}  // namespace

bool parse_header(const Bytes& plain, Header& header, std::string* error) {
    try {
        read_header_impl(plain, header);
        return true;
    } catch (const std::exception& e) {
        set_error(error, std::string("GVAS header: ") + e.what());
        return false;
    }
}

bool read_properties(const Bytes& plain, size_t begin, size_t limit, PropertyList& list, std::string* error) {
    try {
        read_properties_impl(plain, begin, limit, list);
        return true;
    } catch (const std::exception& e) {
        set_error(error, std::string("property list: ") + e.what());
        return false;
    }
}

bool parse_layout(const Bytes& plain, Layout& layout, std::string* error) {
    try {
        parse_layout_impl(plain, layout);
        return true;
    } catch (const std::exception& e) {
        set_error(error, std::string("GVAS layout: ") + e.what());
        return false;
    }
}

int find_property(const PropertyList& list, const std::string& name, int32_t array_index) {
    for (size_t i = 0; i < list.properties.size(); ++i) {
        const Property& p = list.properties[i];
        if (p.name == name && p.array_index == array_index) return static_cast<int>(i);
    }
    return -1;
}

bool read_leading_version(const Bytes& plain, uint32_t& version, std::string* error) {
    try {
        size_t off = leading_version_offset(plain);
        version = static_cast<uint32_t>(plain[off] | (plain[off + 1] << 8) | (plain[off + 2] << 16) |
                                        (static_cast<uint32_t>(plain[off + 3]) << 24));
        return true;
    } catch (const std::exception& e) {
        set_error(error, std::string("Version: ") + e.what());
        return false;
    }
}

bool write_leading_version(Bytes& plain, uint32_t version, std::string* error) {
    try {
        store_u32(plain, leading_version_offset(plain), version);
        return true;
    } catch (const std::exception& e) {
        set_error(error, std::string("Version: ") + e.what());
        return false;
    }
}

bool read_struct_datetime(const Bytes& plain, const Property& owner, const std::string& field, int64_t& ticks,
                          std::string* error) {
    if (owner.type != "StructProperty") {
        set_error(error, owner.name + " is not a struct");
        return false;
    }
    PropertyList fields;
    if (!read_properties(plain, owner.value_offset, owner.end(), fields, error)) return false;
    int i = find_property(fields, field, 0);
    if (i < 0 || fields.properties[static_cast<size_t>(i)].size != 8) {
        set_error(error, owner.name + " has no 8-byte " + field);
        return false;
    }
    const size_t off = fields.properties[static_cast<size_t>(i)].value_offset;
    uint64_t v = 0;
    for (int k = 0; k < 8; ++k) v |= static_cast<uint64_t>(plain[off + k]) << (8 * k);
    ticks = static_cast<int64_t>(v);
    return true;
}

Bytes property_bytes(const Bytes& plain, const Property& property) {
    return Bytes(plain.begin() + static_cast<ptrdiff_t>(property.offset),
                 plain.begin() + static_cast<ptrdiff_t>(property.end()));
}

Bytes make_bool_property(const std::string& name, bool value) {
    Bytes out;
    put_fstring(out, name);
    put_fstring(out, "BoolProperty");
    put_i32(out, 0);  // size
    put_i32(out, 0);  // array index
    out.push_back(value ? 1 : 0);
    out.push_back(0);  // no property GUID
    return out;
}

bool replace_property(Bytes& plain, const std::string& name, int32_t array_index, const Bytes& bytes,
                      std::string* error) {
    Layout layout;
    if (!parse_layout(plain, layout, error)) return false;
    int i = find_property(layout.top, name, array_index);
    if (i < 0) {
        set_error(error, "no top-level property " + name + "[" + std::to_string(array_index) + "]");
        return false;
    }
    const Property& p = layout.top.properties[static_cast<size_t>(i)];
    splice(plain, p.offset, p.end(), bytes);
    return true;
}

bool insert_property(Bytes& plain, size_t position, const Bytes& bytes, std::string* error) {
    Layout layout;
    if (!parse_layout(plain, layout, error)) return false;
    const auto& props = layout.top.properties;
    if (position > props.size()) {
        set_error(error, "insert position past the end of the property list");
        return false;
    }
    size_t at = position < props.size() ? props[position].offset : layout.top.none_offset;
    splice(plain, at, at, bytes);
    return true;
}

bool remove_properties(Bytes& plain, const std::string& name, std::string* error) {
    Layout layout;
    if (!parse_layout(plain, layout, error)) return false;
    const auto& props = layout.top.properties;
    for (size_t i = props.size(); i-- > 0;) {
        if (props[i].name == name) splice(plain, props[i].offset, props[i].end(), Bytes());
    }
    return true;
}

bool remove_struct_fields(Bytes& plain, const std::string& struct_name, const std::set<std::string>& fields,
                          std::string* error) {
    Layout layout;
    if (!parse_layout(plain, layout, error)) return false;
    int i = find_property(layout.top, struct_name, 0);
    if (i < 0 || layout.top.properties[static_cast<size_t>(i)].type != "StructProperty") {
        set_error(error, "no top-level struct " + struct_name);
        return false;
    }
    const Property s = layout.top.properties[static_cast<size_t>(i)];
    PropertyList inner;
    if (!read_properties(plain, s.value_offset, s.end(), inner, error)) return false;
    size_t removed = 0;
    for (size_t k = inner.properties.size(); k-- > 0;) {
        const Property& f = inner.properties[k];
        if (fields.count(f.name)) {
            removed += f.end() - f.offset;
            splice(plain, f.offset, f.end(), Bytes());
        }
    }
    // The struct's tag precedes all removed bytes, so its size field has not moved.
    store_u32(plain, s.size_offset, static_cast<uint32_t>(s.size - static_cast<int32_t>(removed)));
    return true;
}

}  // namespace gvas
}  // namespace kakarot
