#include "fake_storage.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace plugin_test {
namespace {

using FileWriteFn = bool(__fastcall*)(void*, const char*, const void*, int32_t);
using FileDeleteFn = bool(__fastcall*)(void*, const char*);
using FileExistsFn = bool(__fastcall*)(void*, const char*);

int64_t unix_time_of(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return 0;
    ULARGE_INTEGER t;
    t.LowPart = info.ftLastWriteTime.dwLowDateTime;
    t.HighPart = info.ftLastWriteTime.dwHighDateTime;
    return static_cast<int64_t>(t.QuadPart / 10000000ULL) - 11644473600LL;
}

kakarot::Bytes read_all(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return kakarot::Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

FakeStorage::FakeStorage(const fs::path& dir) : dir_(dir) {
    for (void*& slot : table_) slot = reinterpret_cast<void*>(&Trap);
    table_[0] = reinterpret_cast<void*>(&FileWrite);
    table_[1] = reinterpret_cast<void*>(&FileRead);
    table_[6] = reinterpret_cast<void*>(&FileDelete);
    table_[13] = reinterpret_cast<void*>(&FileExists);
    table_[15] = reinterpret_cast<void*>(&GetFileSize);
    table_[16] = reinterpret_cast<void*>(&GetFileTimestamp);
    object_ = {table_, this};
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        File& f = files_[entry.path().filename().string()];
        f.data = read_all(entry.path());
        f.time = unix_time_of(entry.path());
        clock_ = std::max(clock_, f.time);
    }
}

bool FakeStorage::game_write(const std::string& name, const kakarot::Bytes& data) {
    return reinterpret_cast<FileWriteFn>(table_[0])(object(), name.c_str(), data.data(),
                                                    static_cast<int32_t>(data.size()));
}

bool FakeStorage::game_exists(const std::string& name) {
    return reinterpret_cast<FileExistsFn>(table_[13])(object(), name.c_str());
}

bool FakeStorage::game_delete(const std::string& name) {
    return reinterpret_cast<FileDeleteFn>(table_[6])(object(), name.c_str());
}

kakarot::Bytes FakeStorage::get(const std::string& name) const {
    auto it = files_.find(name);
    return it == files_.end() ? kakarot::Bytes() : it->second.data;
}

int FakeStorage::writes(const std::string& name) const {
    return static_cast<int>(std::count(write_log_.begin(), write_log_.end(), name));
}

bool FakeStorage::write(const std::string& name, const void* data, int32_t size) {
    write_log_.push_back(name);
    if (refused.count(name)) return false;
    File& f = files_[name];
    const uint8_t* p = static_cast<const uint8_t*>(data);
    f.data.assign(p, p + size);
    f.time = ++clock_;
    std::ofstream out(dir_ / name, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), size);
    if (reenter && depth_ == 0) {
        ++depth_;
        reinterpret_cast<FileWriteFn>(table_[0])(object(), name.c_str(), data, size);
        if (!reenter_exists.empty()) reinterpret_cast<FileExistsFn>(table_[13])(object(), reenter_exists.c_str());
        --depth_;
    }
    return true;
}

bool __fastcall FakeStorage::FileWrite(void* self, const char* name, const void* data, int32_t size) {
    return of(self).write(name, data, size);
}

int32_t __fastcall FakeStorage::FileRead(void* self, const char* name, void* data, int32_t size) {
    FakeStorage& s = of(self);
    if (s.on_read) s.on_read(name);
    auto it = s.files_.find(name);
    if (it == s.files_.end() || size <= 0) return 0;
    int32_t n = std::min(size, static_cast<int32_t>(it->second.data.size()));
    std::memcpy(data, it->second.data.data(), static_cast<size_t>(n));
    return n;
}

bool __fastcall FakeStorage::FileDelete(void* self, const char* name) {
    FakeStorage& s = of(self);
    if (!s.files_.erase(name)) return false;
    std::error_code ignored;
    fs::remove(s.dir_ / name, ignored);
    return true;
}

bool __fastcall FakeStorage::FileExists(void* self, const char* name) { return of(self).has(name); }

int32_t __fastcall FakeStorage::GetFileSize(void* self, const char* name) {
    FakeStorage& s = of(self);
    auto it = s.files_.find(name);
    return it == s.files_.end() ? 0 : static_cast<int32_t>(it->second.data.size());
}

int64_t __fastcall FakeStorage::GetFileTimestamp(void* self, const char* name) {
    FakeStorage& s = of(self);
    auto it = s.files_.find(name);
    return it == s.files_.end() ? 0 : it->second.time;
}

void FakeStorage::Trap() {
    std::fprintf(stderr, "FATAL: the plugin called an unexpected ISteamRemoteStorage vtable slot\n");
    ExitProcess(3);
}

}  // namespace plugin_test
