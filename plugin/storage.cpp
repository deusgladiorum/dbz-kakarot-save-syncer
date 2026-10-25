#include "storage.h"

#include <windows.h>

#include "plugin/hooks.h"
#include "plugin/log.h"

namespace plugin {
namespace storage {
namespace {

// ISteamRemoteStorage v014 member functions as x64 free functions (`this` first, in rcx).
using FileWriteFn = bool(__fastcall*)(void* self, const char* file, const void* data, int32_t size);
using FileReadFn = int32_t(__fastcall*)(void* self, const char* file, void* data, int32_t size);
using FileExistsFn = bool(__fastcall*)(void* self, const char* file);
using GetFileSizeFn = int32_t(__fastcall*)(void* self, const char* file);
using GetFileTimestampFn = int64_t(__fastcall*)(void* self, const char* file);

struct Functions {
    void* object = nullptr;
    FileWriteFn write = nullptr;
    FileReadFn read = nullptr;
    FileExistsFn exists = nullptr;
    GetFileSizeFn size = nullptr;
    GetFileTimestampFn timestamp = nullptr;
};

Functions g_functions;
thread_local int t_depth = 0;

// The original behind a vtable slot: the slot itself, unless it already holds this plugin's hook.
void* original_at(void** vtable, int index) {
    for (int f = 0; f < hooks::kFunctionCount; ++f) {
        const hooks::Slot& s = hooks::slot(f);
        if (s.vtable_index == index && vtable[index] == s.replacement) return hooks::original(f);
    }
    return vtable[index];
}

// The raw calls run under SEH so a fault inside Steam never leaves the inside flag set.
bool call_exists(const char* name) {
    ++t_depth;
    bool result = false;
    __try {
        result = g_functions.exists(g_functions.object, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = false;
    }
    --t_depth;
    return result;
}

int64_t call_timestamp(const char* name) {
    ++t_depth;
    int64_t result = 0;
    __try {
        result = g_functions.timestamp(g_functions.object, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
    }
    --t_depth;
    return result;
}

int32_t call_size(const char* name) {
    ++t_depth;
    int32_t result = 0;
    __try {
        result = g_functions.size(g_functions.object, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    --t_depth;
    return result;
}

int32_t call_read(const char* name, void* data, int32_t size) {
    ++t_depth;
    int32_t result = 0;
    __try {
        result = g_functions.read(g_functions.object, name, data, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    --t_depth;
    return result;
}

bool call_write(const char* name, const void* data, int32_t size) {
    ++t_depth;
    bool result = false;
    __try {
        result = g_functions.write(g_functions.object, name, data, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = false;
    }
    --t_depth;
    return result;
}

}  // namespace

void bind(void* object) {
    void** vtable = *static_cast<void***>(object);
    Functions f;
    f.object = object;
    f.write = reinterpret_cast<FileWriteFn>(original_at(vtable, kFileWriteIndex));
    f.read = reinterpret_cast<FileReadFn>(original_at(vtable, kFileReadIndex));
    f.exists = reinterpret_cast<FileExistsFn>(original_at(vtable, kFileExistsIndex));
    f.size = reinterpret_cast<GetFileSizeFn>(original_at(vtable, kGetFileSizeIndex));
    f.timestamp = reinterpret_cast<GetFileTimestampFn>(original_at(vtable, kGetFileTimestampIndex));
    g_functions = f;
}

void unbind() { g_functions = Functions(); }

bool ready() { return g_functions.object != nullptr; }

bool inside_plugin() { return t_depth > 0; }

bool exists(const std::string& name) { return ready() && call_exists(name.c_str()); }

int64_t timestamp(const std::string& name) { return ready() ? call_timestamp(name.c_str()) : 0; }

bool read(const std::string& name, kakarot::Bytes& out) {
    if (!ready()) return false;
    log::Timer timer;
    const int32_t size = call_size(name.c_str());
    if (size <= 0) {
        log::write("  plugin read \"%s\": GetFileSize = %d, not read | %.3f ms", name.c_str(), size, timer.ms());
        return false;
    }
    out.assign(static_cast<size_t>(size), 0);
    const int32_t got = call_read(name.c_str(), out.data(), size);
    const bool ok = got == size;
    log::write("  plugin read \"%s\": %d of %d B -> %s | %.3f ms", name.c_str(), got, size, ok ? "ok" : "FAILED",
               timer.ms());
    if (!ok) out.clear();
    return ok;
}

bool write(const std::string& name, const kakarot::Bytes& data) {
    if (!ready()) return false;
    log::Timer timer;
    const bool ok = call_write(name.c_str(), data.data(), static_cast<int32_t>(data.size()));
    log::write("  plugin write \"%s\": %zu B -> %s | %.3f ms", name.c_str(), data.size(), ok ? "ok" : "REFUSED",
               timer.ms());
    return ok;
}

}  // namespace storage
}  // namespace plugin
