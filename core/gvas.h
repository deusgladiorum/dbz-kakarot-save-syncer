// Minimal walk of a decrypted UE4 GVAS save: the header, the top-level tagged
// properties, and a handful of raw-byte splice helpers.
//
// Values are never parsed; properties are located by their tags and moved as
// raw bytes. Static arrays appear as repeated properties with the same name
// and different array indices, so properties are kept in file order and are
// addressed by (name, array index).
#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "aes.h"

namespace kakarot {
namespace gvas {

struct EngineVersion {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    uint32_t changelist = 0;
    std::string branch;
};

struct CustomVersion {
    std::array<uint8_t, 16> guid{};
    int32_t version = 0;
};

struct Header {
    int32_t save_game_version = 0;
    int32_t package_version = 0;
    int32_t package_version_ue5 = 0;  // only present when save_game_version >= 3
    EngineVersion engine;
    int32_t custom_version_format = 0;
    std::vector<CustomVersion> custom_versions;
    std::string save_game_class;
    size_t class_name_offset = 0;  // the engine header is everything before this
    size_t body_offset = 0;        // first top-level property
};

// One tagged property, located but not decoded.
struct Property {
    std::string name;
    std::string type;
    int32_t array_index = 0;
    int32_t size = 0;           // value size as stored in the tag
    size_t offset = 0;          // start of the tag (the name FString)
    size_t size_offset = 0;     // position of the int32 size field
    size_t value_offset = 0;    // first byte of the value
    size_t end() const { return value_offset + static_cast<size_t>(size); }
};

// A property list terminated by a "None" name.
struct PropertyList {
    std::vector<Property> properties;  // in file order
    size_t none_offset = 0;            // start of the "None" FString
    size_t none_end = 0;               // just after it
};

// Header plus the top-level property list of a whole file.
struct Layout {
    Header header;
    PropertyList top;
    size_t body_end = 0;  // after the "None" terminator and its trailing uint32
};

// Parses the GVAS header of a decrypted save.
bool parse_header(const Bytes& plain, Header& header, std::string* error = nullptr);

// Walks tagged properties starting at `begin` until "None"; no tag or value may cross `limit`.
bool read_properties(const Bytes& plain, size_t begin, size_t limit, PropertyList& list,
                     std::string* error = nullptr);

// Parses the header and the top-level property list and finds the body end.
bool parse_layout(const Bytes& plain, Layout& layout, std::string* error = nullptr);

// Returns the position in `list` of the first property with this name and array index, or -1.
int find_property(const PropertyList& list, const std::string& name, int32_t array_index);

// Reads the leading `Version` UInt32Property inside the first top-level struct (System or User).
bool read_leading_version(const Bytes& plain, uint32_t& version, std::string* error = nullptr);

// Overwrites the leading `Version` UInt32Property inside the first top-level struct.
bool write_leading_version(Bytes& plain, uint32_t version, std::string* error = nullptr);

// Reads the ticks of DateTime field `field` in the tagged body of struct `owner` (e.g. Headers[slot].Timestamp).
bool read_struct_datetime(const Bytes& plain, const Property& owner, const std::string& field, int64_t& ticks,
                          std::string* error = nullptr);

// Returns the raw bytes (tag and value) of a property.
Bytes property_bytes(const Bytes& plain, const Property& property);

// Serializes a BoolProperty tag (size 0, value in the tag, no GUID).
Bytes make_bool_property(const std::string& name, bool value);

// Replaces the top-level property (name, array index) with `bytes`; fails if it is absent.
bool replace_property(Bytes& plain, const std::string& name, int32_t array_index, const Bytes& bytes,
                      std::string* error = nullptr);

// Inserts `bytes` before the top-level property at `position` (== count inserts before "None").
bool insert_property(Bytes& plain, size_t position, const Bytes& bytes, std::string* error = nullptr);

// Removes every top-level property with this name, whatever its array index.
bool remove_properties(Bytes& plain, const std::string& name, std::string* error = nullptr);

// Removes fields by name from a top-level struct's property list and fixes the struct's size field.
bool remove_struct_fields(Bytes& plain, const std::string& struct_name, const std::set<std::string>& fields,
                          std::string* error = nullptr);

}  // namespace gvas
}  // namespace kakarot
