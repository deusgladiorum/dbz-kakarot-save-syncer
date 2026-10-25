// Which game build (standard = Unreal 4.21, HD = Unreal 4.27) an executable or
// a save belongs to, and the save slot names each build uses.
#pragma once

#include <string>

#include <windows.h>

#include "gvas.h"

namespace kakarot {

enum class Build { Unknown, Standard, HD };

namespace build {

// Slot indices follow the game's ESaveGameSlot enum.
constexpr int kFirstManualSlot = 0;
constexpr int kLastManualSlot = 9;
constexpr int kAutoSlot = 10;
constexpr int kSystemSlot = 11;

// Maps an Unreal engine version to a build (4.21 standard, 4.27 or later HD).
Build from_engine_version(int major, int minor);

// Detects the build from a decrypted save's GVAS header.
Build from_save_header(const gvas::Header& header);

// Detects the build from an executable's version resource on disk.
Build from_exe_file(const std::wstring& exe_path);

// Detects the build from the version resource of a module loaded in this process.
Build from_module(HMODULE module);

// Returns "standard", "HD" or "unknown".
const char* name(Build build);

// Returns the file name of a slot in a build (e.g. 3, HD -> "ManualSaveR3"), or "" if invalid.
std::string slot_file_name(int slot, Build build);

// Parses a slot file name into its slot index and build; false for any other name.
bool parse_slot_file_name(const std::string& file_name, int& slot, Build& build);

// Returns the other build's name for the same slot (ManualSave2 <-> ManualSaveR2), or "".
std::string counterpart_file_name(const std::string& file_name);

}  // namespace build
}  // namespace kakarot
