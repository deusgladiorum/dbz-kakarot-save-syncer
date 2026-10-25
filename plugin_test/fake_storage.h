// A fake ISteamRemoteStorage for plugin_test: an object whose first word points to a v014-layout
// vtable (FileWrite 0, FileRead 1, FileDelete 6, FileExists 13, GetFileSize 15,
// GetFileTimestamp 16; every other slot traps), backed by an in-memory map that is mirrored to
// a scratch directory. The plugin's real hooks are patched into this vtable, so test code that
// calls through the vtable plays the game.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/aes.h"

namespace plugin_test {

class FakeStorage {
public:
    // Loads every file in `dir`, with its file time as the Steam timestamp.
    explicit FakeStorage(const std::filesystem::path& dir);

    void* object() { return &object_; }
    void** vtable() { return table_; }

    // Game-side calls, made through the (possibly hooked) vtable.
    bool game_write(const std::string& name, const kakarot::Bytes& data);
    bool game_exists(const std::string& name);
    bool game_delete(const std::string& name);

    bool has(const std::string& name) const { return files_.count(name) != 0; }
    kakarot::Bytes get(const std::string& name) const;
    void set_time(const std::string& name, int64_t time) { files_[name].time = time; }

    // Number of times the original FileWrite stored (or refused) this name.
    int writes(const std::string& name) const;

    // FileWrite of these names returns false and stores nothing.
    std::set<std::string> refused;
    // When set, each outermost original FileWrite calls back through the vtable once: FileWrite
    // with the same arguments and FileExists of `reenter_exists`.
    bool reenter = false;
    std::string reenter_exists;
    // When set, called with the name at the start of every original FileRead.
    std::function<void(const std::string&)> on_read;

private:
    struct File {
        kakarot::Bytes data;
        int64_t time = 0;
    };
    struct Object {
        void** vtable;
        FakeStorage* owner;
    };

    static FakeStorage& of(void* self) { return *static_cast<Object*>(self)->owner; }
    static bool __fastcall FileWrite(void* self, const char* name, const void* data, int32_t size);
    static int32_t __fastcall FileRead(void* self, const char* name, void* data, int32_t size);
    static bool __fastcall FileDelete(void* self, const char* name);
    static bool __fastcall FileExists(void* self, const char* name);
    static int32_t __fastcall GetFileSize(void* self, const char* name);
    static int64_t __fastcall GetFileTimestamp(void* self, const char* name);
    static void Trap();

    bool write(const std::string& name, const void* data, int32_t size);

    std::filesystem::path dir_;
    std::map<std::string, File> files_;
    std::vector<std::string> write_log_;
    int64_t clock_ = 0;
    int depth_ = 0;
    void* table_[32];
    Object object_;
};

}  // namespace plugin_test
