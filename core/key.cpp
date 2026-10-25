#include "key.h"

#include <cstring>

namespace kakarot {
namespace key {
namespace {

constexpr size_t kKeyStringLength = 128;

bool is_key_char(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// Counts maximal runs of exactly 128 key characters followed by a NUL.
// Data may arrive in several chunks; reset() marks a discontinuity.
class Scanner {
public:
    void reset() { run_ = 0; }

    // Chunks fed without a reset() in between must be contiguous in memory.
    void feed(const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            uint8_t c = data[i];
            if (is_key_char(c)) {
                if (run_ == 0) run_start_ = data + i;
                ++run_;
                continue;
            }
            if (c == 0 && run_ == kKeyStringLength) {
                if (count_ == 0) std::memcpy(first_.data(), run_start_, first_.size());
                ++count_;
            }
            run_ = 0;
        }
    }

    // Copies the first 32 bytes of the single candidate; false if there were zero or several.
    bool result(Key& key, std::string* error) const {
        if (count_ != 1) {
            if (error) *error = "expected exactly one 128-character key candidate, found " + std::to_string(count_);
            return false;
        }
        key = first_;
        return true;
    }

private:
    const uint8_t* run_start_ = nullptr;
    size_t run_ = 0;
    size_t count_ = 0;
    Key first_{};
};

}  // namespace

bool find_in_file(const std::wstring& exe_path, Key& key, std::string* error) {
    HANDLE file = CreateFileW(exe_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot open executable";
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) {
        if (error) *error = "cannot map executable";
        return false;
    }
    const uint8_t* view = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!view) {
        if (error) *error = "cannot map executable";
        return false;
    }
    Scanner scanner;
    scanner.feed(view, static_cast<size_t>(size.QuadPart));
    UnmapViewOfFile(view);
    return scanner.result(key, error);
}

bool find_in_module(HMODULE module, Key& key, std::string* error) {
    // LoadLibraryEx(..., LOAD_LIBRARY_AS_IMAGE_RESOURCE) handles carry flag bits in the low bits.
    const uint8_t* base = reinterpret_cast<const uint8_t*>(reinterpret_cast<ULONG_PTR>(module) & ~ULONG_PTR(3));
    if (!base) {
        if (error) *error = "null module";
        return false;
    }
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        if (error) *error = "module has no DOS header";
        return false;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        if (error) *error = "module has no PE header";
        return false;
    }
    Scanner scanner;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        size_t section_size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        const uint8_t* p = base + section->VirtualAddress;
        const uint8_t* end = p + section_size;
        scanner.reset();
        // Walk the section region by region, scanning only committed, readable pages.
        while (p < end) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(p, &info, sizeof(info))) break;
            const uint8_t* region_end = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
            if (region_end > end) region_end = end;
            bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (readable) {
                scanner.feed(p, static_cast<size_t>(region_end - p));
            } else {
                scanner.reset();
            }
            p = region_end;
        }
    }
    return scanner.result(key, error);
}

bool verify_key(const Key& key, const uint8_t* encrypted, size_t size) {
    if (size < aes::kBlockSize) return false;
    uint8_t block[aes::kBlockSize];
    std::memcpy(block, encrypted, sizeof(block));
    aes::decrypt_ecb(key, block, sizeof(block));
    return std::memcmp(block, "GVAS", 4) == 0;
}

}  // namespace key
}  // namespace kakarot
