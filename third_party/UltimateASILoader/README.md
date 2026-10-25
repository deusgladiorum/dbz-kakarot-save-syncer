# Ultimate ASI Loader

`xinput1_3.dll` in this folder is Ultimate ASI Loader, the DLL that loads `.asi` plugins into a
game process. The release zip ships it next to `kakarot-hd-save-syncer.asi` so that the plugin
is loaded when the game starts.

| | |
|---|---|
| Upstream | ThirteenAG/Ultimate-ASI-Loader on GitHub |
| Licence | MIT |
| Variant | x64 (`InternalName` `Ultimate-ASI-Loader-x64`, `OriginalFilename` `Ultimate-ASI-Loader-x64.dll`) |
| Product version | 9.7.0 (file version 9.7.0) |
| Size | 1,178,624 bytes |
| SHA-256 | `6203C5A0BA1F8C5C77C7C12C33BE178A5057063EF1DA265EB0A546495C6E6A4E` |

The file is redistributed unmodified, renamed to `xinput1_3.dll`, one of the DLL names the
loader supports. `LICENSE.txt` here is the upstream project's MIT licence, fetched from its
repository, and ships in the release zip as `UltimateASILoader-LICENSE.txt`. It is not built from this repository. `tools/package.ps1` checks its version
resource before packaging it.
