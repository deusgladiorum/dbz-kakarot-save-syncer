# Reverse-engineering notes

Findings from static analysis of the **HD (Remaster)** executable in Ghidra,
`dlc/Remaster/AT/Binaries/Win64/AT-Win64-Shipping.exe` (UE4 branch `Remaster_dlc8`,
image base `0x140000000`, ~250k stripped functions). Addresses below are for that
binary only; the standard-edition exe has not been analysed yet.

## Save file format

- Saves are written through **ISteamRemoteStorage** (interface version 014), which is
  why they live in `userdata/<id>/851850/remote` and have no extension.
- Every save file is a multiple of 16 bytes. Bytes 16..31 are identical across *all*
  files (standard, HD, system, autosave, TCG `C_Save1`); bytes 0..15 occur in only two
  variants. That is the signature of **AES in ECB mode with a fixed key**.
- Confirmed in code: the game's `UGameplayStatics::SaveGameToMemory`
  (`FUN_14406c8a0`) was modified to pad the serialized buffer with zero bytes to a
  16-byte boundary and then call `FUN_1426f10e0`, which is UE4's
  `FAES::EncryptData(Contents, NumBytes, const ANSICHAR* KeyString)`:
  - `FUN_1426f10e0` — strlen of the key string at `PTR_DAT_1468fc080`, then calls
  - `FUN_1426f1100` — `FAES::EncryptData(uint8*, uint32, const uint8* Key)`:
    256-bit key schedule (`FUN_1426ffe60(ctx, key, 0x100)`) then in-place ECB over
    16-byte blocks via `FUN_1426feac0` (rijndael block encrypt; uses the Te0 table at
    `0x145912a10`).
  - The key is a **static ANSI string** referenced via `PTR_DAT_1468fc080`
    (`g_SaveAESKeyString_Ptr`), string at `0x145912980` = RVA `0x5912980`, file
    offset `0x5911B80` in the HD exe. The string is 128 lowercase alphanumeric
    characters but the 256-bit key schedule only consumes the first 32 bytes.
    **Do not commit the key.** `tools/kakarot_save.py` reads it from the exe at
    runtime (`--exe ... --key-rva 5912980`) or from a local key file.
  - Decrypt side: `FAES_DecryptData_KeyString` `0x1426f07b0` (same key pointer),
    `FAES_DecryptData` `0x1426f07e0`, `rijndaelKeySetupDec` `0x1426ffc20`,
    `rijndaelDecrypt` `0x1426fd990` (Td tables `0x145913e10..0x145914a10`). Called
    from `UGameplayStatics_LoadGameFromSlot` `0x144061bd0`, which loads through
    `ISaveGameSystem` (vtable +0x30), decrypts, then calls `LoadGameFromMemory`.
  - A second AES table at `0x146023d80` (used by `FUN_1449c8970` / `FUN_1449c91c0`)
    belongs to a different library (OpenSSL/Steam/CriWare), not the save path.
- The payload under the encryption is expected to be a stock UE4 `GVAS` save:
  `FSaveGameHeader::Read` is at `FUN_14406a080` (checks tag `0x53415647`), and the
  writer in `FUN_14406c8a0` emits `GVAS`, `SaveGameFileVersion = 2`, package version
  and `FEngineVersion`, then the SaveGame UClass path, then the object's properties.
  `LoadGameFromMemory` is `FUN_1440619c0` (callers `FUN_14404c630`, `FUN_144061bd0`).
- The two first-block variants are therefore two different 16-byte plaintext headers,
  most likely differing in the engine version / changelist written by each build.

## Phase 1 results: what the native transfer actually does

Verified by decrypting the user's `_matrix_1_std_pre_transfer` (input) and
`_matrix_2_hd_post_transfer` (native transfer output) sets with the key above.

- All files decrypt to valid `GVAS`. Standard build writes a **UE 4.21.2** header
  (`PackageFileUE4Version` 517, 48 custom versions, header = 1039 bytes for
  `ATSaveGame`, 1044 for `ATSaveSystem`). HD build writes a **UE 4.27.2** header
  (522, 63 custom versions, 1339 / 1344 bytes). That is the 300-byte difference.
- **ManualSave / AutoSave:** after the header, the transferred body is byte-identical
  to the standard body except for 4 bytes: the `System.Version` UInt32Property
  (first property in the file) is rewritten from the standard build's value to the
  HD build's value. Nothing else changes. So native transfer == header swap +
  version restamp.
- **SystemSave (`ATSaveSystem`):** header swap + `User.Version` restamp, plus
  (from the structural diff with `tools/gvas_diff.py`):
  - three standard-only bools dropped from `User`: `IsPrivacyPolicyW180`,
    `IsKpiNoticeW180`, `IsAlreadyUpdatedLinkedBonusesForPatchOneDotSeven`
    (the HD class simply does not have them);
  - `User.NewTCGNoticesFlag` 6 -> 2 (runtime notice bitmask; bit 4 cleared, likely
    `ETCGNoticeFlag::SaveTransferNotice` being consumed);
  - a new top-level `IsConversion` BoolProperty = true inserted after `DLC`.
    Traced (HD build): field at `ATSaveSystem+0x9af34`, no Blueprint flags, no getter.
    Written only by `ATSaveTransfer_TransferSlot` (`0x141d229fb`) after a successful
    system-save transfer; read only by `ATSaveManager_StartSaveSlot_SendKpi`
    (`0x141d22670`, read at `0x141d22752`) to send a "savedata_conversion" 1/0
    telemetry value via `ATKpi_SendSaveDataConversion` (`0x141d213f0`). Nothing in
    gameplay, loading, the transfer scene or `MergeSystemSave` checks it, and a
    second transfer does not consult it. For a converter it is optional;
  - **everything else identical**, including `Option`, `DLC`, quest lists and both
    `Headers[slot]` entries.
  `tools/kakarot_system.py convert` reproduces the native `SystemSaveR0`
  byte-exactly (with `--tcg-notice 2`).
- **Where the save-list metadata lives:** `ATSaveSystem.Headers[slot]`
  (struct `ATSaveSystemHeader`, sparse static array indexed by `ESaveGameSlot`:
  ManualSave0..9 = 0..9, AutoSave0 = 10). Fields: `LevelNum`, `EpisodeNum`,
  `ChapterNum`, `PartNum`, `AreaID`, `PlayTime` (Timespan), `Timestamp` (DateTime),
  `CharacterName[]`, `MainCurrent[]`, `ProgressID[]` (byte arrays), and for manual
  saves `ThumbnailBin[]` (28672 x UInt16, sparse). This is why copying only the
  ManualSave file leaves stale thumbnails/play time: the list reads SystemSave.
  `tools/kakarot_system.py merge-headers` splices `Headers[slot]` from one build's
  SystemSave into the other's without touching `User`/`Option` (no EULA reset).
- Static arrays (`Foo[i]`) are serialised sparsely: elements equal to the class
  default are omitted, and non-zero `ArrayIndex` values occur. `pygvas` cannot parse
  this; `tools/gvas.py` is a stdlib-only parser that round-trips all six reference
  files byte-exactly (it recomputes every size field).
- `System.Version` / `User.Version` is a per-build save-data version constant with a
  separate number range per edition, observed across all backups on this machine:

  | Build | older saves (Sep 2025) | current saves (Mar 2026) |
  |---|---|---|
  | Standard | 544 (0x220) | 560 (0x230) |
  | HD | 304 (0x130) | 320 (0x140) |

  The TCG save `C_Save1` (`ATSaveTcg`) has its own `Version` = 256 and is not part of
  the transfer.
- **Oracle passed.** `tools/kakarot_transfer.py` (decrypt, splice the target
  build's engine header, restamp `Version` from a reference save, re-pad,
  re-encrypt) applied to `_matrix_1` AutoSave0 / ManualSave0 produces files that
  are **byte-identical, including the encrypted bytes**, to the game's own
  `_matrix_2` AutoSaveR0 / ManualSaveR0. The transform for gameplay saves is
  therefore fully characterised.
- Both games load files carrying the other build's engine header and version
  (the current plugin's raw copies prove this), so the version field is what
  separates "loads" from "transferred correctly". Load-time logic gated on it is the
  prime suspect for the camera / HP drift.

## Phase 2 results: what `Version` controls (HD build)

- Current version constant 320 (0x140) is a plain immediate, stamped right before
  every save write by `ATSaveGameSystem_StampCurrentVersion` `0x1422a3b40`
  (`ATSaveGame+0x28` = `System.Version`) and `ATSaveSystem_StampCurrentVersion`
  `0x1422a3b50` (`User.Version`); default set in `FATSaveGameSystem_Init`
  `0x1422921a0`. Callers: `ATSaveManager_SaveSlotTask` `0x141d09360`,
  `ATSaveManager_AutoSaveTask` `0x141d09200`, `ATSaveTransfer_TransferSlot`
  `0x141d22900`. So the native transfer of a game save is literally load, restamp,
  save.
- Load side, `ATSaveManager_LoadSlotTask` `0x141d098d0`:
  `ATSaveGameSystem_IsVersionSupported` `0x142288640` accepts `Version >= 400`
  (standard range) **or** `Version <= 320` (HD range); 321..399 is rejected with
  error 5 (`SaveError_Version_Game`). Same rule for `User.Version`
  (`ATSaveSystem_IsVersionSupported` `0x142288660`). Card-game slots 0xC..0xE use
  `<= 256`. **No migration, clamp or other branch reads `Version`.** Remaining
  post-load steps are a DLC ownership check and `ATSaveGame_PostLoadQuestFixups`
  `0x141d16160` (story/DLC quest flags only).
- Consequence: a standard save loaded raw by the HD build takes the same code path
  as a transferred one. Since our transform output is byte-identical to the native
  transfer, any residual camera / HP difference between builds is inherent to the HD
  build (different tables / defaults), not a defect of the sync.
- `ATSaveTransfer_MergeSystemSave` `0x141d1f920` merges the original SystemSave into
  the existing HD one, preserving a few current HD values (system +0x3d, +0x3e,
  +0x98, +0x9c, +0x710; mostly `ATSaveSystemUser` flags).

## HP: why the two builds show different health for the same save

Observed in the user's files: each time a save is loaded by the *other* build and
re-saved, every character's stored `Status.HitPoint` (and `MagicPoint`, rounded)
grows by about 1% of its non-boost portion, in both directions, and nothing else
in the save changes. Consecutive saves within one build show no drift. Lineage for
Gohan (Lv 15): 38021 (std) -> native transfer keeps 38021 -> HD play+save 38380 ->
std load+save 38742 -> std load shows 39108 (stored in a later std save). Native
transfer + play therefore drifts exactly like the conversion does.

HD build mechanism (Ghidra, confirmed unless marked):
- Saved fields: `ATSaveGameIndividualCharacter` (0x300 B) -> `Status`
  (`ATSaveCharacterStatus`: `Level`, `Exp`, `HitPoint` int, `MagicPoint`,
  `SparkingPoint`) at +0x2C0 and `Boost` (`ATSaveCharacterBoost`: flat permanent
  additions) at +0x2E0. **No max HP or ratio is stored; `HitPoint` is the absolute
  current HP and is read back as-is** (`UATPlayerStatusInstance_GetHitPointF`
  `0x1418afd30`, `_SetHitPoint` `0x1418b83e0`; `_Initialize` `0x1418b3ab0` keeps a
  pointer to the save entry at +0x388 and does not clamp).
- Max HP is recomputed on every load by `UATStatusInstanceBase_RecalcMaxParams`
  `0x14189ec40` = `ATCharacterStatusTable` row (per character/variation/level,
  `ATCharacterStatusTable_FindRow` `0x1418af020`, 0x78 B per level) + active-effect
  contributions x constant coefficients + `Boost.HitPoint`, capped at 999,999,999.
- `UATStatusInstanceBase_RecalcParamsKeepHpRatio` `0x14189e6a0`: whenever max HP
  changes, HP = min(newMax, HP x newMax / oldMax) (optional round-up); same for MP.
  Runs on every effect add/remove and on `_SetLevel` `0x1418b8500`.
- After load, `ATScene_ReapplySavedPlayerEffects` `0x1422a6940` ->
  `UPlayerManager_ApplySavedEffectsToAllStatus` `0x141bc4950` ->
  `UATPlayerStatusInstance_ApplySavedEffects` `0x1418b63a0` re-adds the 26
  `OtherBuffList` buffs, 8 equipped slots and the global saved buffs, each through
  the ratio rescale. (Inferred: the saved absolute HP is first measured against the
  bare max, then scaled up as effects are re-applied, capped at the full max.)
- **In-game result (2026-10-12): the drift is per load, in both builds, regardless
  of which build wrote the file.** Gohan Lv 15: file storing 38742 displays 39108 in
  standard *and* in HD; file storing 39108 (written by standard) displays 39477 in
  standard *and* in HD after the native transfer. Every load shows stored x ~1.00945
  and a save writes that back, so HP compounds ~1% per load/save cycle even without
  ever changing builds. It is a property of the game's effect re-application on
  load, not of the sync or of the native transfer, and the conversion tools produce
  the same in-game numbers as the native transfer.

## Steam Remote Storage index (why raw copies report "no save data")

The game checks slot existence and reads/writes saves through `ISteamRemoteStorage`
(`FileExists` / `FileRead` / `FileWrite`), which consults Steam's per-app index
`userdata/<id>/851850/remotecache.vdf`, not the raw folder. Observed on this
machine: every file a game wrote natively is listed; files created from outside
(the current plugin's `ManualSave1` copy, and a hand-converted `ManualSave2`) are
**not**, so the save list (driven by `SystemSave.Headers[slot]`) shows them but the
load fails with `SaveError_NotExist` (error code 1 in `ATSave_GetErrorText`
`0x141d18160`; 2 = `SaveError_Corrupt`, 3 = `OutOfStorage`, 5 =
`SaveError_Version_Game`, 6 = `MSG_DLC_NotDlcData`). Overwriting an already-indexed
file in place does get picked up (entry goes to `syncstate 3`, pending upload).

Consequence for the plugin: mirrored files must be written with
`ISteamRemoteStorage::FileWrite` (and removed with `FileDelete`), never with
`CopyFileW`. Inside the game process the plugin can use the game's own Steam
context through the flat API exported by `steam_api64.dll`. This also makes the
mirrored files sync through Steam Cloud, which is what cross-device use needs.

## Key location for a runtime scan

The same 128-char key string is present in **both** exes and is the only
NUL-terminated run of 128 `[a-z0-9]` characters in either file (HD: file offset
`0x5911B80`; standard: `0x470BBC0`). In the HD build it is immediately followed by
the AES Te0 table; in the standard build it is not. A plugin can therefore locate
the key by scanning its host module's image for that pattern and confirming the
candidate by decrypting the first 16 bytes of any existing save to `GVAS`.

## Slot naming

- Two reflected enums: `ESaveGameSlot` (`ManualSave0..9`, `AutoSave0`, `SystemSave0`,
  `C_SaveTransfer0`) and `ESteamRemasterSaveGameSlot` (same with an `R` suffix, plus
  `C_SaveTransferR0`). The HD build picks a name by
  `UEnum::GetNameStringByIndex` on whichever enum applies.
- `ATSave_GetCurrentBuildSlotName` `0x141d19af0(FString* out, uint8 slot)` — returns
  this build's (R-suffixed, `ESteamRemasterSaveGameSlot`) slot name for an index.
  `ATSave_DoesCurrentBuildSaveGameExist` `0x141d147d0` uses it.
- `ATSave_DoesOriginalSaveGameExist_Slot` `0x141d145a0(this, uint8 slot)` — builds the
  non-R name from `ESaveGameSlot` and calls `ISteamRemoteStorage::FileExists` (vtable slot 13, offset `0x68`) on
  the non-R name, falling back to `ISaveGameSystem::DoesSaveGameExist`
  (`FUN_14404fc80`) when Steam is unavailable. Exposed to Blueprint as
  `DoesOriginalSaveGameExist` (string at `0x145656fa8`); class exposes
  `OriginalSaveSystem` (`0x145657270`).
- `FUN_141d144f0` — any original slot 0..10 exists.
- `FUN_1421e2a20` — enumerates `ESaveGameSlot` 0..11 and returns the names of the
  original saves that exist (used to populate the transfer UI list).
- `FUN_141d18a90` — picks the newest existing original slot (compares a timestamp at
  `+0x20` of per-slot info from `FUN_14228e760`).

## Transfer flow (Steam remaster)

- Strings: `UATSaveDataTransferManager` (`0x1456547c0`), `UATSceneSaveDataTransferSteam`,
  `UATSceneSaveDataTransferRoot`, `/Game/Maps/SaveDataTransfer/SaveDataTransferRoot`,
  UI text keys `Remaster_Save_Check`, `Remaster_Save_Taking_Over`,
  `Remaster_Save_Take_Over_Compleate`, `SteamRemaster_Save_*`.
- `FUN_141d119b0(this)` — the transfer scene's state machine (state int at `+0x2bc`):
  state 0 shows `Save_GamePlayCheck`, state 5 probes all 11 original slots with
  `FileExists` and resets per-slot info (`FUN_14229eea0` sets `+4 = -1`), states 6/7
  show `MSG_Find_Data`. The state that performs read -> convert -> write has not been
  isolated yet; it is reached from the same object (`FUN_141afe6d0()` returns the
  save manager, slot info array at `+0x108`, user index at `+0x134`).
- TCG (card game) has its own transfer path: `UGCGTransferSaveFile`,
  `GCGTransferOfflineInfoSave`, slot `C_SaveTransfer0`.

## Generic UE4 save plumbing (for hooking)

- `FUN_14404ac80` — `UGameplayStatics::AsyncSaveGameToSlot`; callers `FUN_1412ed250`,
  `FUN_1412ed1c0`, `FUN_14401c5e0` (game-side save requests).
- `FUN_14404ab20` — async load counterpart.
- Steam contexts: `SteamInternal_ContextInit(&PTR_FUN_1466471a8)` = RemoteStorage,
  `(&PTR_FUN_146647118)` = another Steam interface whose vtable slot 1 is used as a
  "logged on / available" check.
- `FUN_14267ae90` = `FMemory::Free`; `FUN_140c8db90` / `FUN_140c8d690` = `TArray`
  reserve / grow; `FUN_142659ff0` = wide-string copy.
