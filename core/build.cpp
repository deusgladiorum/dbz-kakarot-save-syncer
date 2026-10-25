#include "build.h"

#include <cstring>
#include <vector>

#pragma comment(lib, "version.lib")

namespace kakarot {
namespace build {
namespace {

// Finds VS_FIXEDFILEINFO inside a raw VS_VERSIONINFO block by its signature.
const VS_FIXEDFILEINFO* find_fixed_info(const uint8_t* data, size_t size) {
    for (size_t off = 0; off + sizeof(VS_FIXEDFILEINFO) <= size; off += 4) {
        uint32_t sig;
        std::memcpy(&sig, data + off, sizeof(sig));
        if (sig == VS_FFI_SIGNATURE) return reinterpret_cast<const VS_FIXEDFILEINFO*>(data + off);
    }
    return nullptr;
}

Build from_fixed_info(const VS_FIXEDFILEINFO* info) {
    if (!info) return Build::Unknown;
    return from_engine_version(HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS));
}

struct SlotKind {
    const char* stem;
    int first_slot;
    int last_slot;
};

// Name stems; the HD build inserts "R" between the stem and the digit.
const SlotKind kSlotKinds[] = {
    {"ManualSave", kFirstManualSlot, kLastManualSlot},
    {"AutoSave", kAutoSlot, kAutoSlot},
    {"SystemSave", kSystemSlot, kSystemSlot},
};

}  // namespace

Build from_engine_version(int major, int minor) {
    if (major != 4) return Build::Unknown;
    if (minor >= 27) return Build::HD;
    if (minor == 21) return Build::Standard;
    return Build::Unknown;
}

Build from_save_header(const gvas::Header& header) {
    return from_engine_version(header.engine.major, header.engine.minor);
}

Build from_exe_file(const std::wstring& exe_path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(exe_path.c_str(), &handle);
    if (size == 0) return Build::Unknown;
    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(exe_path.c_str(), 0, size, block.data())) return Build::Unknown;
    void* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(block.data(), L"\\", &info, &info_size) || info_size < sizeof(VS_FIXEDFILEINFO))
        return Build::Unknown;
    return from_fixed_info(static_cast<const VS_FIXEDFILEINFO*>(info));
}

Build from_module(HMODULE module) {
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    if (!res) return Build::Unknown;
    HGLOBAL loaded = LoadResource(module, res);
    if (!loaded) return Build::Unknown;
    const uint8_t* data = static_cast<const uint8_t*>(LockResource(loaded));
    if (!data) return Build::Unknown;
    return from_fixed_info(find_fixed_info(data, SizeofResource(module, res)));
}

const char* name(Build build) {
    switch (build) {
        case Build::Standard: return "standard";
        case Build::HD: return "HD";
        default: return "unknown";
    }
}

std::string slot_file_name(int slot, Build build) {
    if (build == Build::Unknown) return std::string();
    for (const SlotKind& kind : kSlotKinds) {
        if (slot < kind.first_slot || slot > kind.last_slot) continue;
        std::string out = kind.stem;
        if (build == Build::HD) out += 'R';
        out += static_cast<char>('0' + (slot - kind.first_slot));
        return out;
    }
    return std::string();
}

bool parse_slot_file_name(const std::string& file_name, int& slot, Build& build) {
    for (const Build b : {Build::Standard, Build::HD}) {
        for (int s = kFirstManualSlot; s <= kSystemSlot; ++s) {
            if (slot_file_name(s, b) == file_name) {
                slot = s;
                build = b;
                return true;
            }
        }
    }
    return false;
}

std::string counterpart_file_name(const std::string& file_name) {
    int slot = 0;
    Build b = Build::Unknown;
    if (!parse_slot_file_name(file_name, slot, b)) return std::string();
    return slot_file_name(slot, b == Build::HD ? Build::Standard : Build::HD);
}

}  // namespace build
}  // namespace kakarot
