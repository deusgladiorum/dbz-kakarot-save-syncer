// Kakarot HD Save Syncer, ASI plugin entry point.
//
// Detects the build, starts the wait thread that hooks ISteamRemoteStorage once the game's Steam
// session is up, hands the sync logic its configuration (build, AES key, manifest path), and
// has the wait thread run the reconcile pass as soon as the hooks are in.
// See plugin/README.md and docs/design.html.
#include <windows.h>

#include <string>

#include "core/build.h"
#include "core/key.h"
#include "plugin/log.h"
#include "plugin/manifest.h"
#include "plugin/steam.h"
#include "plugin/sync.h"
#include "plugin/vtable.h"

namespace {

using plugin::log::write;

kakarot::Build g_build = kakarot::Build::Unknown;
std::wstring g_local_manifest;

// Path of `file_name` in the folder that holds `module`.
std::wstring next_to(HMODULE module, const wchar_t* file_name) {
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1) + file_name;
}

// Runs on the wait thread once Steam is up and the exe is unpacked: locates the key in memory
// (never stored, never logged) and starts the sync session. False leaves the game untouched.
bool prepare() {
    plugin::Config config;
    config.own = g_build;
    config.local_manifest_path = g_local_manifest;
    ULONGLONG start = GetTickCount64();
    std::string error;
    if (!kakarot::key::find_in_module(GetModuleHandleW(nullptr), config.key, &error)) {
        write("AES key: not found (%s; scan %llu ms)", error.c_str(), GetTickCount64() - start);
        return false;
    }
    write("AES key: found, %zu bytes (scan %llu ms)", config.key.size(), GetTickCount64() - start);
    plugin::sync::init(config);
    SecureZeroMemory(config.key.data(), config.key.size());
    return true;
}

void on_attach(HMODULE module) {
    plugin::log::open(module);
    wchar_t path[MAX_PATH * 2] = {};
    char utf8[MAX_PATH * 6];
    GetModuleFileNameW(module, path, MAX_PATH * 2);
    write("plugin: %s", plugin::log::utf8(path, utf8, sizeof(utf8)));
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    write("host exe: %s", plugin::log::utf8(path, utf8, sizeof(utf8)));

    g_build = kakarot::build::from_module(GetModuleHandleW(nullptr));
    if (g_build == kakarot::Build::Unknown) {
        write("build: unknown; plugin inactive");
        return;
    }
    const kakarot::Build foreign = g_build == kakarot::Build::HD ? kakarot::Build::Standard : kakarot::Build::HD;
    write("build: %s (own system save %s, foreign %s)", kakarot::build::name(g_build),
          kakarot::build::slot_file_name(kakarot::build::kSystemSlot, g_build).c_str(),
          kakarot::build::slot_file_name(kakarot::build::kSystemSlot, foreign).c_str());
    g_local_manifest = next_to(module, plugin::manifest::kLocalName);
    write("manifest: cloud name %s, local copy %s", plugin::manifest::kCloudName,
          plugin::log::utf8(g_local_manifest.c_str(), utf8, sizeof(utf8)));

    if (!plugin::steam::start(&prepare, &plugin::sync::reconcile_early))
        write("could not start the wait thread (%lu); plugin inactive", GetLastError());
}

void on_detach(bool process_terminating) {
    write("detach (%s)", process_terminating ? "process terminating" : "FreeLibrary");
    plugin::steam::stop();
    plugin::vtable::restore();
    // At process exit other threads are gone and may have left the sync lock held.
    if (!process_terminating) plugin::sync::shutdown();
    write("detach done");
    plugin::log::close();
}

void on_attach_safe(HMODULE module) {
    __try {
        on_attach(module);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        write("attach: exception 0x%08lx; plugin may be inactive", GetExceptionCode());
    }
}

void on_detach_safe(bool process_terminating) {
    __try {
        on_detach(process_terminating);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            on_attach_safe(module);
            break;
        case DLL_PROCESS_DETACH:
            on_detach_safe(reserved != nullptr);
            break;
    }
    return TRUE;
}
