# Plugin, phase 3: save sync

The plugin keeps the standard and HD builds' saves in step, as specified in
`docs/design.html`. It hooks `ISteamRemoteStorage` `FileWrite`, `FileRead`, `FileDelete` and
`FileExists` (v014 vtable slots 0, 1, 6, 13) in whichever build it is loaded in. That build is
"own" and the other build is "foreign".

- **At save time.** When the game writes one of its own slots (`ManualSaveRx`/`AutoSaveR0` in
  HD, `ManualSaveX`/`AutoSave0` in standard), the plugin converts the bytes to the foreign format
  once the write has succeeded and writes them under the foreign slot name. When the game next
  writes its own system save (about 50 ms later), the plugin merges those slots' `Headers`
  entries into the foreign system save. If the foreign system save does not exist, the own one is
  converted whole instead. Each produced slot is then recorded in the manifest.
- **At startup.** One reconcile pass runs on the plugin's thread as soon as the hooks are in.
  A game call naming a save file or the manifest waits while it runs (each wait is logged), and
  if the pass has not started by the game's first call naming its own system save, that call
  runs it first. For every slot 0 to 10, the pass compares the save timestamps in the two
  system saves' `Headers` entries. The newer side is converted to the other side and merged.
  Equal timestamps mean nothing to do. If a slot exists on one side only, it is converted unless
  the manifest links it to the missing file, which means the missing file was deleted on
  purpose.
- **On delete.** `FileDelete` of an own slot records a deletion entry in the manifest. The
  foreign copy is never deleted.

The target build's engine header and version stamp come from a reference: the newest file of
that build (by Steam file time) whose decrypted header really is that build's. If there is
none, the output keeps the source header and stamp.

All reads and writes go through Remote Storage, so Steam's index and Cloud see them. Save-time
and delete work runs on the game's thread inside the hooked call. A failure, or a write that
Steam refuses, is logged and skipped, never retried, and never changes what the game's own call
returns. The AES key is located in the running exe and kept only in memory. Encryption uses
AES-NI when the CPU has it and the software implementation otherwise, with identical output
(`kakarot_cli bench` times both).

## Build

```
MSBuild.exe kakarot-hd-save-syncer.slnx /p:Configuration=Release /p:Platform=x64
```

Projects: `core/` (conversions), `plugin/kakarot_plugin.vcxproj` (this folder, a static
library), `kakarot-hd-save-syncer.vcxproj` (the `.asi`, only `dllmain.cpp`), `cli/` and
`plugin_test/`. Output: `x64\Release\kakarot-hd-save-syncer.asi` (`x64\Debug\...` for Debug).

## Install

Back up `userdata\<steam id>\851850\remote` first.

1. Put the Ultimate ASI Loader in the exe folder as `xinput1_3.dll`, unless it is already there.
2. Copy `kakarot-hd-save-syncer.asi` into the same folder:
   - Standard: `<game>\AT\Binaries\Win64\`
   - HD: `<game>\dlc\Remaster\AT\Binaries\Win64\`

One build is enough. Installing into both is supported. To uninstall, delete the `.asi`.
Already-produced files stay.

## Files the plugin writes

| File | Where | What |
|---|---|---|
| `kakarot-hd-save-syncer.log` | next to the `.asi` | Appended, one `session start` banner per run |
| `kakarot-hd-save-syncer.manifest` | Remote Storage (the game's cloud folder) | The manifest |
| `kakarot-hd-save-syncer.manifest` | next to the `.asi` | Local copy, read when the cloud copy cannot be read |
| foreign slot files and system save | Remote Storage | The converted saves |

Every log line has the local time to the millisecond and the thread id. The log records each
game call (name, size, result, time), each plugin read and write (size, result, time, with
`REFUSED` when Steam refuses a write), each reconcile decision, and each conversion with its
reference and elapsed time. A heartbeat line every 5 minutes reports call counts and whether
the hooks are still in place. The key is never logged.

The manifest is small JSON. Each entry has `slot`, `direction` (`HD->standard`,
`standard->HD`, `deleted:HD` or `deleted:standard`), `source` and `output` (FNV-1a 64 hashes,
hex) and `time` (UTC). Only the newest 8 entries per slot are kept. Deleting the manifest is
safe for data: the only effect is that a slot deleted on one side may be produced again from
the other.

## Tests

```
x64\Release\plugin_test.exe [--exe STANDARD_EXE] [--fixtures BACKUPS_DIR] [--scratch DIR]
```

`plugin_test` patches the real hooks into a fake Remote Storage with the v014 vtable layout.
The fake keeps its files in memory and mirrors them to a scratch folder. Test code then makes
the game's calls through that vtable. Each case copies its fixtures from
`backups_outside_cloud` and opens nothing there for writing. Expected bytes come from
`kakarot_cli.exe`. Logs and outputs go to `out\plugin_test\<case>\`. `tools/oracle_test.py`
covers `core/` against the Python tools.
