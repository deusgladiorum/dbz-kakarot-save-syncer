# Steam Remote Storage interface notes (investigation notes)

Notes for hooking `ISteamRemoteStorage` from inside either build. HD addresses are for the
Remaster exe as loaded in Ghidra (image base `0x140000000`). Standard-build facts come from
the on-disk bytes only (see "Standard exe is SteamStub-wrapped").

Files:

- Standard: `AT\Binaries\Win64\AT-Win64-Shipping.exe`, `Engine\Binaries\ThirdParty\Steamworks\Steamv139\Win64\steam_api64.dll`
- HD: `dlc\Remaster\AT\Binaries\Win64\AT-Win64-Shipping.exe`, `dlc\Remaster\Engine\Binaries\ThirdParty\Steamworks\Steamv151\Win64\steam_api64.dll`

## 1. Interface version and vtable layout

Both builds use **`STEAMREMOTESTORAGE_INTERFACE_VERSION014`**:

- Standard exe: ASCII string at file offset `0x39377d0` (VA `0x143938bd0`), in a block with
  `SteamClient017`, `SteamUser019`, `SteamUtils008`, `STEAMUSERSTATS_INTERFACE_VERSION011`,
  `STEAMAPPS_INTERFACE_VERSION008`, `STEAMSCREENSHOTS...003`, `STEAMHTTP...002`, `STEAMUGC...009`,
  and so on. That is the SDK 1.39 `CSteamAPIContext::Init` string set.
- HD exe: string at `0x145028c08`, next to `SteamUser021`, `SteamUtils010`,
  `STEAMUSERSTATS_INTERFACE_VERSION012`, `STEAMAPPS_INTERFACE_VERSION008`.

Same version means one layout. The layout below comes from the v014 header, and I checked it
against the flat-API thunks in **both** DLLs. Each `SteamAPI_ISteamRemoteStorage_X` export is
`mov rax,[rcx]; jmp [rax+off]`, and the offsets match in the 1.39 and 1.51 DLLs.

| Method | Index | Offset |
|---|---|---|
| FileWrite | 0 | 0x00 |
| FileRead | 1 | 0x08 |
| FileForget | 5 | 0x28 |
| FileDelete | 6 | 0x30 |
| FileExists | 13 | 0x68 |
| FilePersisted | 14 | 0x70 |
| GetFileSize | 15 | 0x78 |
| GetFileTimestamp | 16 | 0x80 |
| GetFileCount | 18 | 0x90 |
| GetFileNameAndSize | 19 | 0x98 |
| (GetQuota 20 / IsCloudEnabledForApp 22) | | 0xa0 / 0xb0 |

Neither DLL exports `BeginFileWriteBatch` or `EndFileWriteBatch` (those arrived in a later version).

Cross-check against the HD game code. All of these load the pointer from the RemoteStorage
context `0x1466471a8`:

- `ATSave_SteamLoadCallback` `0x141d217d0` (the game's ISaveGameSystem load hook, see 4) calls
  `ISteamUser::BLoggedOn` (+0x08 on context `0x146647118`), then `FileExists` **+0x68**, then
  `GetFileSize` **+0x78**, then `FileRead` **+0x08** (size cap `0x6400000`). If Steam is not
  available it falls back to `%sSaveGames/%s.sav`. It returns 0 = ok, 1 = not exist,
  2 = short read, 5 = fallback failed.
- `ATSave_SteamSaveCallback` `0x141d2a140`: `FileWrite` **+0x00**.
- UE4 OnlineSubsystemSteam user-cloud tasks use the same table: write `0x140cda280` (FileWrite +0x00,
  GetFileSize +0x78), read `0x140ccdc00` (GetFileSize +0x78, FileRead +0x08), enumerate
  `0x140cccb40` (GetFileCount +0x90, GetFileNameAndSize +0x98), delete/forget `0x140ccc810`
  (+0x28/+0x30).
- `ATSave_DoesOriginalSaveGameExist_Slot` `0x141d145a0`: FileExists +0x68 (already known).

`ISteamClient::GetISteamRemoteStorage` is index **17 (0x88)** in both DLLs (flat thunk
`SteamAPI_ISteamClient_GetISteamRemoteStorage` = `jmp [rax+0x88]`).
`GetISteamGenericInterface` is index 12 (0x60).

On Steam, file names are bare slot names with no extension (`SystemSaveR0`, `ManualSave1`, ...),
matching `userdata/<id>/851850/remote/`.

## 2. How each build obtains the pointer

Both exes **delay-load** `steam_api64.dll`. Neither exe imports it statically.

- **HD** (SDK 1.51, per-interface accessor macro): the context `PTR_FUN_1466471a8` is a 3-slot
  `{callback, counter, value}` block. Callback `0x140ccb240` does
  `*p = SteamInternal_FindOrCreateUserInterface(SteamAPI_GetHSteamUser(), "STEAMREMOTESTORAGE_INTERFACE_VERSION014")`
  (delay IAT `0x146c6c690` = GetHSteamUser, `0x146c6c678` = FindOrCreateUserInterface).
  Every use calls `SteamInternal_ContextInit` (IAT `0x146c6c680`) and reads the cached pointer.
  The cache is refilled only when steam_api's init counter changes. The HD exe imports neither
  `SteamInternal_CreateInterface` nor `SteamAPI_GetHSteamPipe`.
  Context `0x146647118` is ISteamUser (`SteamUser021`, callback `0x140ccb270`).
- **Standard** (SDK 1.39, single `CSteamAPIContext`): its steam_api64 delay imports include
  `SteamInternal_ContextInit`, `SteamInternal_CreateInterface`, `SteamAPI_GetHSteamUser` and
  `SteamAPI_GetHSteamPipe` (IAT `0x1459177c8..0x1459177e0`). The string block above is
  `CSteamAPIContext::Init`'s. So the pointer comes from
  `ISteamClient017 = SteamInternal_CreateInterface("SteamClient017")`, then
  `->GetISteamRemoteStorage(hUser, hPipe, "...014")` (vtable 0x88). It is cached in the
  module context in `.data`. This was inferred from imports and strings; code is not
  readable on disk.

Export check:

| Export | 1.39 DLL | 1.51 DLL |
|---|---|---|
| `SteamClient`, `SteamAPI_GetHSteamUser`, `SteamAPI_GetHSteamPipe` | yes | yes |
| `SteamAPI_ISteamClient_GetISteamRemoteStorage` | yes | yes |
| `SteamAPI_ISteamRemoteStorage_*` (FileWrite etc.) | yes | yes |
| `SteamInternal_FindOrCreateUserInterface` | no | yes |
| `SteamAPI_SteamRemoteStorage_v014` | no | yes |

The 1.51 DLL contains both `SteamClient017` and `SteamClient020`. GetISteamRemoteStorage is
0x88 in both client versions.

**Simplest common path:**
`SteamAPI_ISteamClient_GetISteamRemoteStorage(SteamClient(), SteamAPI_GetHSteamUser(), SteamAPI_GetHSteamPipe(), "STEAMREMOTESTORAGE_INTERFACE_VERSION014")`.
Resolve each export with `GetProcAddress` on `GetModuleHandleW(L"steam_api64.dll")`. This is
valid only after the game's `SteamAPI_Init`; before that `GetHSteamPipe()`/`GetHSteamUser()`
return 0. It may not return the very same object as the game's cached pointer (that has not
been verified), but it is the same interface version from the same steamclient. So it has the
same adapter class and the same vtable, which is all a vtable patch needs.

## 3. Hooking

- The object comes from `steamclient64.dll` (loaded by `SteamAPI_Init` from the Steam install,
  not the game folder). Its vtable is in that DLL's read-only data. This is not checked
  statically: at runtime, confirm `*(void**)obj` lies inside `steamclient64.dll` with
  `GetModuleHandleExW(FROM_ADDRESS)`.
- Patching the slot in place is viable: `VirtualProtect(slot, 8, PAGE_READWRITE)`, an aligned
  8-byte store (`InterlockedExchangePointer`, which is atomic against concurrent callers),
  then restore the protection. Save the original pointers and call them directly from the
  plugin's own FileWrite/FileDelete so the hook does not recurse.
- This hooks every caller of that interface version in the process: the game's save code, UE4
  OnlineSubsystemSteam user cloud, and the plugin itself. That is desired.
- Both builds cache the **object pointer** (HD per-interface context `0x1466471b8`, standard
  CSteamAPIContext). Every call still goes through `mov rax,[obj]; call [rax+off]` (seen at all
  call sites above), so caching does not matter for a vtable patch. It would only matter for an
  object-swap approach.
- steamclient64 changes with Steam client updates, so never hardcode its addresses. Derive
  them from the live object.

## 4. Startup timing (HD)

Boot path, from `AAT_Title` (UClass `/Script/AT.AT_Title`, getter `0x142013c40`, ctor
`0x141d044d0`, vtable `0x1454072c8`). Its vtable slot 143 (`0x141d26b40`, most likely `Tick`)
runs a state machine at `this+0x2b8`:

1. State 1, `0x141d13530`: shows `System_Check_Attention`. After a minimum display time it
   calls `ATSave_DoesCurrentBuildSaveGameExist(mgr, 0xB)`, which is **`FileExists("SystemSaveR0")`
   on the game thread**. The result goes to `this+0x2c6`.
2. State 2 (only if it exists), `0x141d13d10`: shows `System_Load_Attention`, binds
   completion to `mgr+0xa0`, and calls `ATSaveManager_RequestLoad` `0x141d1f550(mgr, 0xB, 0)`.
   That function sets `mgr+0x140 = 3` and queues a task-graph task (create `0x140c826e0`,
   dispatch `0x141d24b70`, ThreadToExecuteOn = `0xff` = `ENamedThreads::AnyThread`). The task
   runs the thunk `0x141d0cdf0`, which jumps to `ATSaveManager_LoadSlotTask` `0x141d098d0`,
   then `UGameplayStatics_LoadGameFromSlot`, then the ISaveGameSystem load delegate (bound at
   `saveSystem+0x18` by `ATSaveManager_Init` `0x141d20e40`), then `0x141d217d0`. That last
   function does **FileExists, GetFileSize and FileRead on a worker thread**. LoadSlotTask
   broadcasts `mgr+0xa0` and resets `+0x140` from that same thread.
3. If the save does not exist, state 3 (`0x141d13770`) runs, and state 4 is
   `ATSceneSaveDataTransferSteam_Tick` (FileExists on the 11 original slots).

So the first Remote Storage touch of the own system save is a game-thread `FileExists`, shown
while the attention notice is up. A few hundred ms there is a hitch on a static notice screen.
The actual read follows on an AnyThread worker. No save-code RS call precedes step 1 on this
path. I did not prove that nothing else (e.g. OnlineSubsystemSteam) calls RS earlier, so the
trigger should be name-based, not "first call". Other code that requests slot 0xB, all guarded
by `mgr+0x130` (system save loaded): `UTitleHud` vtable `0x145408d48` slot 105 (`0x141d0b2c0`),
`0x141d26880` (TitleHud exec native), `0x1421e1a40`, `0x1421d85e0`.

The standard build presumably runs the same logic with `SystemSave0`. It cannot be checked on
disk (see below).

## 5. When steam_api64.dll is loaded

- No static import, so it is **not loaded when an ASI/proxy `DllMain` runs**. Both exes
  delay-load it. The engine's SteamShared module loads it explicitly first. In HD,
  `0x140d40eb0` builds `<Engine>/Binaries/ThirdParty/Steamworks/Steamv151/Win64/` +
  `steam_api` + `64` + `.dll`, then calls PushDllDirectory, GetDllHandle (`LoadLibraryW`) and
  PopDllDirectory. The standard exe has the same strings (`Binaries/ThirdParty/Steamworks/%s/Win64/`,
  `Steamv139`, `steam_api64.dll`). The delay-load stubs then bind to the already-loaded module.
- `SteamAPI_Init` (OnlineSubsystemSteam init) comes later. Only then are the user/pipe
  handles non-zero and `steamclient64.dll` loaded.
- Plugin wait: start a thread in `DllMain` (no work under the loader lock). Poll every ~10-20 ms
  for `GetModuleHandleW(L"steam_api64.dll")`, then for `SteamAPI_GetHSteamPipe() != 0`, then get
  the interface and patch. Engine init takes seconds before the title tick, so polling wins
  the race comfortably. Hooking `LoadLibraryW` would not help, because the interface is not
  ready at load time anyway.

## Standard exe is SteamStub-wrapped

The standard exe has a `.bind` section and its entry point is in `.bind` (`0x65d3310`). `.text`
has entropy 8.00 with no `CC` padding, so it is encrypted on disk and decrypted by the stub
before the OEP. Static code xrefs in the standard exe are therefore impossible without a
memory dump. Also, a proxy DLL's `DllMain` runs before decryption. The approach above needs
no code addresses in either exe, only the steam_api64 exports and the live vtable.

## Recommended approach

1. In `DllMain`, spawn a worker thread and return.
2. The thread polls for `steam_api64.dll` and then for `SteamAPI_GetHSteamPipe() != 0`.
3. Get `ISteamRemoteStorage*` with
   `SteamAPI_ISteamClient_GetISteamRemoteStorage(SteamClient(), GetHSteamUser(), GetHSteamPipe(), "...VERSION014")`
   (both DLLs). Check that the vtable is inside `steamclient64.dll`.
4. Patch vtable slots 13 (FileExists), 15 (GetFileSize) and 1 (FileRead), plus 0 (FileWrite)
   if write mirroring is wanted. Keep the original pointers.
5. In the hooks, when the file name equals this build's system save (`SystemSaveR0` for HD,
   `SystemSave0` for standard), run the startup sync once under a once-lock that blocks the
   other callers. Then forward to the original. In HD the first such call is the game-thread
   FileExists at the title check, so the sync completes before the game decides "exists /
   load". Do all plugin RS I/O through the saved originals.

## Unresolved

- I did not confirm that steamclient's returned object (or its vtable) is identical to the
  game's cached one. It is expected to share the vtable; verify at runtime by comparing
  `*(void**)` of the plugin's pointer with the game's. HD cache: `0x1466471b8`.
- Standard-build boot order and thread are inferred, not verified (encrypted `.text`).
- Exact identity of `AAT_Title` slot 143 (assumed Tick) and of `UTitleHud` slot 105.
- In Ghidra I disassembled two small undefined ranges (`0x141d0cdc0`, `0x140ccb270`). The
  program was not saved.
