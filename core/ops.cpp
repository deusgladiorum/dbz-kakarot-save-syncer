#include "ops.h"

#include <algorithm>
#include <map>
#include <set>

#include "gvas.h"

namespace kakarot {
namespace ops {
namespace {

// Fields the standard build's ATSaveSystemUser has and the HD build does not.
const std::set<std::string> kStandardOnlyUserFields = {
    "IsPrivacyPolicyW180",
    "IsKpiNoticeW180",
    "IsAlreadyUpdatedLinkedBonusesForPatchOneDotSeven",
};

// Top-level field the HD build adds after a native transfer.
const char kIsConversion[] = "IsConversion";

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

// Decrypts and walks a file; `what` names it in error messages.
bool open(const Bytes& encrypted, const Key& key, const char* what, Bytes& plain, gvas::Layout& layout,
          std::string* error) {
    std::string e;
    if (!decrypt(encrypted, key, plain, &e) || !gvas::parse_layout(plain, layout, &e)) {
        set_error(error, std::string(what) + ": " + e);
        return false;
    }
    return true;
}

// The target's engine header followed by the source's class name and body (padding dropped).
Bytes rebase(const Bytes& src_plain, const gvas::Layout& src, const Bytes& ref_plain, const gvas::Layout& ref) {
    Bytes out(ref_plain.begin(), ref_plain.begin() + static_cast<ptrdiff_t>(ref.header.class_name_offset));
    out.insert(out.end(), src_plain.begin() + static_cast<ptrdiff_t>(src.header.class_name_offset),
               src_plain.begin() + static_cast<ptrdiff_t>(src.body_end));
    return out;
}

// Header swap plus Version restamp, shared by both conversions.
bool rebase_and_restamp(const Bytes& src, const Bytes& ref, const Key& key, Bytes& out, std::string* error) {
    Bytes src_plain, ref_plain;
    gvas::Layout src_layout, ref_layout;
    if (!open(src, key, "source", src_plain, src_layout, error)) return false;
    if (!open(ref, key, "reference", ref_plain, ref_layout, error)) return false;
    uint32_t version = 0;
    std::string e;
    if (!gvas::read_leading_version(ref_plain, version, &e)) {
        set_error(error, "reference: " + e);
        return false;
    }
    out = rebase(src_plain, src_layout, ref_plain, ref_layout);
    if (!gvas::write_leading_version(out, version, &e)) {
        set_error(error, "source: " + e);
        return false;
    }
    return true;
}

}  // namespace

bool decrypt(const Bytes& encrypted, const Key& key, Bytes& plain, std::string* error) {
    plain = encrypted;
    if (!aes::decrypt_ecb(key, plain.data(), plain.size())) {
        set_error(error, "file size is not a multiple of 16");
        return false;
    }
    return true;
}

Bytes encrypt(Bytes plain, const Key& key) {
    aes::pad16(plain);
    aes::encrypt_ecb(key, plain.data(), plain.size());
    return plain;
}

bool convert_game_save(const Bytes& src, const Bytes& ref, const Key& key, Bytes& out, std::string* error) {
    Bytes plain;
    if (!rebase_and_restamp(src, ref, key, plain, error)) return false;
    out = encrypt(std::move(plain), key);
    return true;
}

bool merge_headers(const Bytes& src, const Bytes& dst, const Key& key, const std::vector<int>& slots,
                   Bytes& out, std::string* error) {
    Bytes src_plain, plain;
    gvas::Layout src_layout, layout;
    if (!open(src, key, "source", src_plain, src_layout, error)) return false;
    if (!open(dst, key, "destination", plain, layout, error)) return false;
    plain.resize(layout.body_end);  // drop the padding; it is recomputed on encryption

    // Source entries by slot; a later duplicate wins, as in the Python tool.
    std::map<int, gvas::Property> src_headers;
    for (const gvas::Property& p : src_layout.top.properties)
        if (p.name == "Headers") src_headers[p.array_index] = p;

    for (int slot : slots) {
        auto it = src_headers.find(slot);
        if (it == src_headers.end()) {
            set_error(error, "source system save has no Headers[" + std::to_string(slot) + "]");
            return false;
        }
        const Bytes entry = gvas::property_bytes(src_plain, it->second);
        if (!gvas::parse_layout(plain, layout, error)) return false;
        const auto& props = layout.top.properties;

        if (gvas::find_property(layout.top, "Headers", slot) >= 0) {
            if (!gvas::replace_property(plain, "Headers", slot, entry, error)) return false;
            continue;
        }
        // Insert keeping Headers entries contiguous and ordered by slot: after the
        // last entry with a lower slot, else before the first entry, else after Option.
        size_t position = 0;
        bool have_headers = false;
        for (size_t i = 0; i < props.size(); ++i) {
            if (props[i].name != "Headers") continue;
            if (!have_headers) position = i;
            have_headers = true;
            if (props[i].array_index < slot) position = i + 1;
        }
        if (!have_headers) {
            auto option = std::find_if(props.begin(), props.end(),
                                       [](const gvas::Property& p) { return p.name == "Option"; });
            if (option == props.end()) {
                set_error(error, "destination system save has no Headers and no Option");
                return false;
            }
            position = static_cast<size_t>(option - props.begin()) + 1;
        }
        if (!gvas::insert_property(plain, position, entry, error)) return false;
    }
    out = encrypt(std::move(plain), key);
    return true;
}

bool convert_system_save(const Bytes& src, const Bytes& ref, const Key& key, bool to_hd, Bytes& out,
                         std::string* error) {
    Bytes plain;
    if (!rebase_and_restamp(src, ref, key, plain, error)) return false;
    if (to_hd) {
        if (!gvas::remove_struct_fields(plain, "User", kStandardOnlyUserFields, error)) return false;
        gvas::Layout layout;
        if (!gvas::parse_layout(plain, layout, error)) return false;
        if (gvas::find_property(layout.top, kIsConversion, 0) < 0) {
            const auto& props = layout.top.properties;
            auto dlc = std::find_if(props.begin(), props.end(),
                                    [](const gvas::Property& p) { return p.name == "DLC"; });
            if (dlc == props.end()) {
                set_error(error, "system save has no DLC property");
                return false;
            }
            size_t position = static_cast<size_t>(dlc - props.begin()) + 1;
            if (!gvas::insert_property(plain, position, gvas::make_bool_property(kIsConversion, true), error))
                return false;
        }
    } else {
        if (!gvas::remove_properties(plain, kIsConversion, error)) return false;
    }
    out = encrypt(std::move(plain), key);
    return true;
}

bool header_slots(const Bytes& encrypted, const Key& key, std::vector<int>& slots, std::string* error) {
    Bytes plain;
    gvas::Layout layout;
    if (!open(encrypted, key, "system save", plain, layout, error)) return false;
    slots.clear();
    for (const gvas::Property& p : layout.top.properties)
        if (p.name == "Headers") slots.push_back(p.array_index);
    std::sort(slots.begin(), slots.end());
    return true;
}

}  // namespace ops
}  // namespace kakarot
