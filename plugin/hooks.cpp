#include "hooks.h"

#include <windows.h>

#include <cstring>

#include "plugin/log.h"
#include "plugin/storage.h"
#include "plugin/sync.h"

namespace plugin {
namespace hooks {
namespace {

// ISteamRemoteStorage v014 member functions as x64 free functions: `this` arrives first, in rcx.
// (__fastcall is accepted and ignored on x64, where there is a single calling convention.)
using FileWriteFn = bool(__fastcall*)(void* self, const char* file, const void* data, int32_t size);
using FileReadFn = int32_t(__fastcall*)(void* self, const char* file, void* data, int32_t size);
using FileDeleteFn = bool(__fastcall*)(void* self, const char* file);
using FileExistsFn = bool(__fastcall*)(void* self, const char* file);

constexpr size_t kNameMax = 128;

void* volatile g_original[kFunctionCount] = {};
volatile LONG64 g_counts[kFunctionCount] = {};

// Copy of the game's file name argument; "?" if the pointer faults.
struct Name {
    char text[kNameMax];

    explicit Name(const char* file) { copy(file, text); }

    static void copy(const char* file, char* out) {
        __try {
            if (!file) {
                strcpy_s(out, kNameMax, "(null)");
                return;
            }
            size_t i = 0;
            for (; i + 1 < kNameMax && file[i]; ++i) out[i] = file[i];
            out[i] = '\0';
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            strcpy_s(out, kNameMax, "?");
        }
    }
};

const char* bool_text(bool value) { return value ? "true" : "false"; }

template <class Fn>
Fn forward(int function) {
    return reinterpret_cast<Fn>(g_original[function]);
}

bool __fastcall FileWrite(void* self, const char* file, const void* data, int32_t size) {
    if (storage::inside_plugin()) return forward<FileWriteFn>(kFileWrite)(self, file, data, size);
    InterlockedIncrement64(&g_counts[kFileWrite]);
    Name name(file);
    sync::before_call(name.text);
    log::Timer timer;
    bool result = forward<FileWriteFn>(kFileWrite)(self, file, data, size);
    log::write("FileWrite(\"%s\", %d B) -> %s | %.3f ms", name.text, size, bool_text(result), timer.ms());
    sync::after_write(name.text, data, size, result);
    return result;
}

int32_t __fastcall FileRead(void* self, const char* file, void* data, int32_t size) {
    if (storage::inside_plugin()) return forward<FileReadFn>(kFileRead)(self, file, data, size);
    InterlockedIncrement64(&g_counts[kFileRead]);
    Name name(file);
    sync::before_call(name.text);
    log::Timer timer;
    int32_t result = forward<FileReadFn>(kFileRead)(self, file, data, size);
    log::write("FileRead(\"%s\", %d B) -> %d | %.3f ms", name.text, size, result, timer.ms());
    return result;
}

bool __fastcall FileDelete(void* self, const char* file) {
    if (storage::inside_plugin()) return forward<FileDeleteFn>(kFileDelete)(self, file);
    InterlockedIncrement64(&g_counts[kFileDelete]);
    Name name(file);
    sync::before_call(name.text);
    const uint64_t deleted_hash = sync::before_delete(name.text);
    log::Timer timer;
    bool result = forward<FileDeleteFn>(kFileDelete)(self, file);
    log::write("FileDelete(\"%s\") -> %s | %.3f ms", name.text, bool_text(result), timer.ms());
    sync::after_delete(name.text, result, deleted_hash);
    return result;
}

bool __fastcall FileExists(void* self, const char* file) {
    if (storage::inside_plugin()) return forward<FileExistsFn>(kFileExists)(self, file);
    InterlockedIncrement64(&g_counts[kFileExists]);
    Name name(file);
    sync::before_call(name.text);
    log::Timer timer;
    bool result = forward<FileExistsFn>(kFileExists)(self, file);
    log::write("FileExists(\"%s\") -> %s | %.3f ms", name.text, bool_text(result), timer.ms());
    return result;
}

const Slot kSlots[kFunctionCount] = {
    {"FileWrite", 0, reinterpret_cast<void*>(&FileWrite)},
    {"FileRead", 1, reinterpret_cast<void*>(&FileRead)},
    {"FileDelete", 6, reinterpret_cast<void*>(&FileDelete)},
    {"FileExists", 13, reinterpret_cast<void*>(&FileExists)},
};

}  // namespace

const Slot& slot(int function) { return kSlots[function]; }

void set_original(int function, void* original) {
    InterlockedExchangePointer(const_cast<void**>(&g_original[function]), original);
}

void* original(int function) { return g_original[function]; }

void counts(int64_t out[kFunctionCount]) {
    for (int i = 0; i < kFunctionCount; ++i) out[i] = g_counts[i];
}

}  // namespace hooks
}  // namespace plugin
