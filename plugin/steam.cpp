#include "steam.h"

#include <cstdint>

#include "plugin/hooks.h"
#include "plugin/log.h"
#include "plugin/storage.h"
#include "plugin/vtable.h"

namespace plugin {
namespace steam {
namespace {

constexpr DWORD kPollMs = 15;
constexpr ULONGLONG kWaitTimeoutMs = 120000;
constexpr DWORD kHeartbeatMs = 5 * 60 * 1000;
constexpr wchar_t kSteamApiDll[] = L"steam_api64.dll";
constexpr wchar_t kSteamClientDll[] = L"steamclient64.dll";
constexpr char kRemoteStorageVersion[] = "STEAMREMOTESTORAGE_INTERFACE_VERSION014";

// Flat steam_api64 exports (S_CALLTYPE is __cdecl; x64 has one convention).
using SteamClientFn = void*(__cdecl*)();
using SteamHandleFn = int32_t(__cdecl*)();
using GetRemoteStorageFn = void*(__cdecl*)(void* client, int32_t user, int32_t pipe, const char* version);

struct Exports {
    SteamClientFn client = nullptr;
    SteamHandleFn user = nullptr;
    SteamHandleFn pipe = nullptr;
    GetRemoteStorageFn remote_storage = nullptr;
};

HMODULE g_module = nullptr;
HANDLE g_stop = nullptr;
PrepareCallback g_prepare = nullptr;
ActiveCallback g_on_active = nullptr;
ULONGLONG g_start_tick = 0;

ULONGLONG since_start() { return GetTickCount64() - g_start_tick; }

// Waits `ms`; false if a stop was requested.
bool pause(DWORD ms) { return WaitForSingleObject(g_stop, ms) == WAIT_TIMEOUT; }

// 1. steam_api64.dll is delay-loaded by the game and not present when DllMain runs.
HMODULE wait_for_steam_api() {
    ULONGLONG deadline = GetTickCount64() + kWaitTimeoutMs;
    HMODULE api = nullptr;
    while (!(api = GetModuleHandleW(kSteamApiDll))) {
        if (GetTickCount64() > deadline) {
            log::write("steam_api64.dll did not load within %llu s; no hooks installed", kWaitTimeoutMs / 1000);
            return nullptr;
        }
        if (!pause(kPollMs)) return nullptr;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(api, path, MAX_PATH);
    char path_utf8[MAX_PATH * 3];
    log::write("steam_api64.dll loaded, %llu ms after attach: %s", since_start(),
               log::utf8(path, path_utf8, sizeof(path_utf8)));
    return api;
}

bool resolve(HMODULE api, Exports& e) {
    e.client = reinterpret_cast<SteamClientFn>(GetProcAddress(api, "SteamClient"));
    e.user = reinterpret_cast<SteamHandleFn>(GetProcAddress(api, "SteamAPI_GetHSteamUser"));
    e.pipe = reinterpret_cast<SteamHandleFn>(GetProcAddress(api, "SteamAPI_GetHSteamPipe"));
    e.remote_storage =
        reinterpret_cast<GetRemoteStorageFn>(GetProcAddress(api, "SteamAPI_ISteamClient_GetISteamRemoteStorage"));
    if (e.client && e.user && e.pipe && e.remote_storage) return true;
    log::write("missing steam_api64 export(s); no hooks installed");
    return false;
}

// 2. Waits for SteamAPI_Init (non-zero pipe and user, a client) and gets the v014 interface.
void* wait_for_remote_storage(const Exports& e) {
    ULONGLONG deadline = GetTickCount64() + kWaitTimeoutMs;
    int32_t pipe = 0, user = 0;
    void* client = nullptr;
    while (!((pipe = e.pipe()) && (user = e.user()) && (client = e.client()))) {
        if (GetTickCount64() > deadline) {
            log::write("Steam not initialised within %llu s; no hooks installed", kWaitTimeoutMs / 1000);
            return nullptr;
        }
        if (!pause(kPollMs)) return nullptr;
    }
    void* remote_storage = e.remote_storage(client, user, pipe, kRemoteStorageVersion);
    if (!remote_storage) {
        log::write("GetISteamRemoteStorage(\"%s\") returned null; no hooks installed", kRemoteStorageVersion);
        return nullptr;
    }
    void* table = *static_cast<void**>(remote_storage);
    char where[MAX_PATH + 32];
    bool in_steamclient = false;
    vtable::describe_address(table, where, sizeof(where), kSteamClientDll, &in_steamclient);
    log::write("Steam initialised, %llu ms after attach: ISteamRemoteStorage* = %p, vtable %p (%s)%s", since_start(),
               remote_storage, table, where, in_steamclient ? "" : "  WARNING: vtable is not inside steamclient64.dll");
    return remote_storage;
}

void heartbeat() {
    int64_t counts[hooks::kFunctionCount];
    hooks::counts(counts);
    log::write("heartbeat: FileWrite=%lld FileRead=%lld FileDelete=%lld FileExists=%lld, hooked slots intact: %s",
               counts[hooks::kFileWrite], counts[hooks::kFileRead], counts[hooks::kFileDelete],
               counts[hooks::kFileExists], vtable::intact() ? "yes" : "NO");
}

void run() {
    HMODULE api = wait_for_steam_api();
    Exports exports;
    if (!api || !resolve(api, exports)) return;
    void* remote_storage = wait_for_remote_storage(exports);
    if (!remote_storage) return;
    if (g_prepare && !g_prepare()) {
        log::write("plugin not ready; no hooks installed, the game runs untouched");
        return;
    }
    storage::bind(remote_storage);
    if (vtable::install(remote_storage) == 0) return;
    log::write("active, %llu ms after attach", since_start());
    if (g_on_active) g_on_active();
    while (pause(kHeartbeatMs)) heartbeat();
}

DWORD WINAPI thread_proc(void*) {
    __try {
        run();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::write("wait thread: exception 0x%08lx, thread stopped (patched slots stay patched)", GetExceptionCode());
    }
    FreeLibraryAndExitThread(g_module, 0);
}

}  // namespace

bool start(PrepareCallback prepare, ActiveCallback on_active) {
    g_prepare = prepare;
    g_on_active = on_active;
    g_start_tick = GetTickCount64();
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) return false;
    // The thread keeps the DLL loaded while it runs and releases it with FreeLibraryAndExitThread.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&start), &self))
        return false;
    g_module = self;
    HANDLE thread = CreateThread(nullptr, 0, thread_proc, nullptr, 0, nullptr);
    if (!thread) {
        FreeLibrary(self);
        return false;
    }
    CloseHandle(thread);
    return true;
}

void stop() {
    if (g_stop) SetEvent(g_stop);
}

}  // namespace steam
}  // namespace plugin
