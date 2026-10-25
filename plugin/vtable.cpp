#include "vtable.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

#include "plugin/hooks.h"
#include "plugin/log.h"

namespace plugin {
namespace vtable {
namespace {

void** volatile g_vtable = nullptr;
volatile LONG g_patched[hooks::kFunctionCount] = {};

bool read_pointer(const void* address, void*& out) {
    __try {
        out = *static_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool is_executable(DWORD protect) {
    return (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Makes one slot writable; restores the old protection through `old_protect` afterwards.
bool unprotect(void** slot, const char* what, DWORD& old_protect) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        log::write("%s: slot %p is not committed memory", what, slot);
        return false;
    }
    if (!VirtualProtect(slot, sizeof(void*), is_executable(mbi.Protect) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE,
                        &old_protect)) {
        log::write("%s: VirtualProtect failed (%lu)", what, GetLastError());
        return false;
    }
    return true;
}

void reprotect(void** slot, DWORD old_protect) {
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
}

// Replaces one vtable slot with its hook; the original is stored before the swap.
bool patch_slot(int function) {
    const hooks::Slot& info = hooks::slot(function);
    void** slot = g_vtable + info.vtable_index;
    if (reinterpret_cast<uintptr_t>(slot) % sizeof(void*) != 0) {
        log::write("patch %s: slot %p is not 8-byte aligned, not patched", info.name, slot);
        return false;
    }
    if (*slot == info.replacement) {
        log::write("patch %s: slot already holds this plugin's hook, left alone", info.name);
        return false;
    }
    DWORD old_protect = 0;
    if (!unprotect(slot, info.name, old_protect)) return false;
    void* current = nullptr;
    bool swapped = false;
    for (int attempt = 0; attempt < 3 && !swapped; ++attempt) {
        current = *slot;
        hooks::set_original(function, current);
        swapped = InterlockedCompareExchangePointer(slot, info.replacement, current) == current;
    }
    reprotect(slot, old_protect);
    if (!swapped) {
        log::write("patch %s: slot %p kept changing under the swap, not patched", info.name, slot);
        return false;
    }
    InterlockedExchange(&g_patched[function], 1);
    char where[MAX_PATH + 32];
    describe_address(current, where, sizeof(where));
    log::write("patched %s: vtable[%d] at %p, original %p (%s)", info.name, info.vtable_index, slot, current, where);
    return true;
}

void restore_slot(int function) {
    const hooks::Slot& info = hooks::slot(function);
    void** slot = g_vtable + info.vtable_index;
    DWORD old_protect = 0;
    if (!unprotect(slot, info.name, old_protect)) return;
    void* original = hooks::original(function);
    bool restored = InterlockedCompareExchangePointer(slot, original, info.replacement) == info.replacement;
    reprotect(slot, old_protect);
    InterlockedExchange(&g_patched[function], 0);
    log::write(restored ? "restored %s: vtable[%d] = %p" : "restore %s: vtable[%d] no longer holds the hook, left alone",
               info.name, info.vtable_index, original);
}

void restore_slot_safe(int function) {
    __try {
        restore_slot(function);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::write("restore %s: exception 0x%08lx", hooks::slot(function).name, GetExceptionCode());
    }
}

}  // namespace

int install(void* object) {
    void* table = nullptr;
    if (!read_pointer(object, table) || !table) {
        log::write("object %p has no readable vtable; no hooks installed", object);
        return 0;
    }
    g_vtable = static_cast<void**>(table);
    int patched = 0;
    for (int f = 0; f < hooks::kFunctionCount; ++f) {
        if (patch_slot(f)) ++patched;
    }
    log::write("hooks installed: %d of %d", patched, int(hooks::kFunctionCount));
    return patched;
}

void restore() {
    if (!g_vtable) return;
    for (int f = 0; f < hooks::kFunctionCount; ++f) {
        if (g_patched[f]) restore_slot_safe(f);
    }
}

bool intact() {
    void** table = g_vtable;
    if (!table) return false;
    for (int f = 0; f < hooks::kFunctionCount; ++f) {
        if (!g_patched[f]) continue;
        void* value = nullptr;
        if (!read_pointer(table + hooks::slot(f).vtable_index, value) || value != hooks::slot(f).replacement)
            return false;
    }
    return true;
}

void describe_address(const void* address, char* out, size_t size, const wchar_t* module_name, bool* in_module) {
    if (in_module) *in_module = false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module)) {
        _snprintf_s(out, size, _TRUNCATE, "not in a module");
        return;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* file = wcsrchr(path, L'\\');
    file = file ? file + 1 : path;
    if (in_module && module_name) *in_module = _wcsicmp(file, module_name) == 0;
    char name[MAX_PATH];
    _snprintf_s(out, size, _TRUNCATE, "%s+0x%llx", log::utf8(file, name, sizeof(name)),
                static_cast<unsigned long long>(static_cast<const uint8_t*>(address) -
                                                reinterpret_cast<const uint8_t*>(module)));
}

}  // namespace vtable
}  // namespace plugin
