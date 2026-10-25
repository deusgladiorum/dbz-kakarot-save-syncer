# dbz-kakarot-save-syncer

An ASI plugin for DRAGON BALL Z: KAKAROT on Steam that keeps the standard build's and the HD
build's saves in step, so a game saved in either build can be continued in the other, on the
same PC or on another device through Steam Cloud, with no manual transfer.

## How it works

The plugin is loaded into the game by Ultimate ASI Loader and hooks the game's Steam Remote
Storage calls. When the game writes a save slot, the plugin converts it to the other build's
format and writes it, and its save-list entry, through the same Remote Storage session, so
Steam's index and Steam Cloud see the result. At startup it compares each slot's save time in
the two builds and brings the older side up to date. It never deletes a save. The design is
in [`docs/design.html`](docs/design.html); the save mechanism reference (file format,
encryption, how the game saves, loads and transfers) is in
[`docs/save-mechanism.html`](docs/save-mechanism.html).

## Install

Download `dbz-kakarot-save-syncer-<version>.zip`, extract it, and follow `INSTALL.txt` inside.
In short: copy `dbz-kakarot-save-syncer.asi` and `xinput1_3.dll` into the exe folder of the
build you play on Windows. Nothing else is needed: the `.asi` is linked against the static C++
runtime, so no Visual C++ redistributable is required.

## Build

Requirements: Visual Studio 2026 (version 18) with the Desktop development with C++ workload
(toolset v145, a Windows 10 SDK or later). Only x64 is supported.

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" dbz-kakarot-save-syncer.slnx /p:Configuration=Release /p:Platform=x64
```

Outputs go to `x64\Release\` (`x64\Debug\` for Debug): `dbz-kakarot-save-syncer.asi`,
`kakarot_cli.exe` and `plugin_test.exe`.

## Test

Both test programs need a copy of the game's standard-build exe (the AES key is read from it)
and a folder of save backups. Neither writes to the game folder or the backups folder.

### Plugin tests

```
x64\Release\plugin_test.exe --exe STANDARD_EXE --fixtures BACKUPS_DIR [--scratch DIR] [--cli KAKAROT_CLI]
```

- `--exe`: the standard build's `AT\Binaries\Win64\AT-Win64-Shipping.exe` (required; falls back to `KAKAROT_EXE`).
- `--fixtures`: the folder holding the backup sets the cases copy their saves from (required;
  falls back to `KAKAROT_FIXTURES`).
- `--scratch`: where logs and outputs go (default `out\plugin_test\`).
- `--cli`: the `kakarot_cli.exe` that produces the expected bytes (default: next to the test).

It prints one `PASS`/`FAIL` line per case and exits non-zero if any case fails.
`plugin/README.md` describes what the cases cover.

### Oracle test

The oracle test checks that `kakarot_cli.exe` produces byte-identical output to the Python
tools in `tools/`. Set up the Python environment once:

```
python -m venv .venv
.venv\Scripts\python.exe -m pip install cryptography
```

Then run:

```
.venv\Scripts\python.exe tools\oracle_test.py --cli x64\Release\kakarot_cli.exe --exe STANDARD_EXE --hd-exe HD_EXE --saves BACKUPS_DIR
```

- `--cli`: the built `kakarot_cli.exe` under test.
- `--exe`: the standard build's `AT-Win64-Shipping.exe` (key source).
- `--hd-exe`: the HD build's `dlc\Remaster\AT\Binaries\Win64\AT-Win64-Shipping.exe`; optional,
  adds the key-scan case.
- `--saves`: the folder holding the backup sets (read only).
- `--out`: where outputs go (default `out\oracle\`).

The backup sets each test expects are named at the top of `tools/oracle_test.py` and
`plugin_test/main.cpp`.

## Package

```
powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-TestExe STANDARD_EXE] [-TestFixtures BACKUPS_DIR]
```

The script rebuilds Release x64, checks that the `.asi` does not depend on the C++ runtime DLLs,
runs `plugin_test.exe` (and stops if it fails; skipped with a warning unless `KAKAROT_EXE` and
`KAKAROT_FIXTURES` or the two parameters are set), and writes `dist\dbz-kakarot-save-syncer-<version>.zip`
with the `.asi`, `xinput1_3.dll`, `INSTALL.txt` and `LICENSE.txt`. The version is read from
`VERSION`. `INSTALL.txt` is generated from `tools/INSTALL.txt` with the version and the
loader's version and SHA-256 filled in.

## Repository layout

| Folder | Contents |
|---|---|
| `core/` | Static library: AES-256, save-file parsing, build detection, the conversions, key lookup |
| `plugin/` | Static library: the hooks, reconcile pass, manifest, log and Remote Storage access |
| `cli/` | `kakarot_cli.exe`, command-line front end to `core/` |
| `plugin_test/` | `plugin_test.exe`, the plugin tests against a fake Remote Storage |
| `tools/` | Python save tools (the oracle), `oracle_test.py`, `package.ps1`, `INSTALL.txt` template |
| `docs/` | The design and the save mechanism reference |
| `third_party/` | Ultimate ASI Loader, redistributed unmodified (see its README) |

The `.asi` project itself (`dbz-kakarot-save-syncer.vcxproj`, `dllmain.cpp`) is at the root.
`x64/`, `obj/`, `out/` and `dist/` are build, test and packaging output and are not tracked.

## The save key

The AES key that encrypts the saves is not stored in this repository. The plugin, the CLI and
the tests read it from the game's exe at runtime (the Python tools do the same with `--exe`)
and keep it only in memory; it is never logged or printed.

## Licence

MIT, see `LICENSE.txt`. Ultimate ASI Loader is MIT-licensed by its own authors.
