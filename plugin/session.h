// The own/foreign model and the per-process sync state shared by the save-time hook and the
// reconcile pass.
//
// "Own" is the build the plugin is loaded in, "foreign" the other build (docs/design.html, 1).
#pragma once

#include <string>
#include <vector>

#include "core/aes.h"
#include "core/build.h"
#include "plugin/manifest.h"

namespace plugin {

enum class Side { Own, Foreign };

inline Side other(Side side) { return side == Side::Own ? Side::Foreign : Side::Own; }

// Game-save slots the plugin syncs: ManualSave0..9 and AutoSave0 (ESaveGameSlot 0..10).
constexpr int kSlotCount = kakarot::build::kAutoSlot + 1;

// File names of both builds' game-save slots and system saves, seen from the own build.
class Model {
public:
    explicit Model(kakarot::Build own);

    kakarot::Build build(Side side) const { return side == Side::Own ? own_ : foreign_; }
    const std::string& slot_name(int slot, Side side) const { return slots_[index(side)][slot]; }
    const std::string& system_name(Side side) const { return system_[index(side)]; }

    // Slot index of an own game-save file name, or -1.
    int own_slot(const char* name) const;
    bool is_own_system(const char* name) const { return system_[0] == name; }
    // Any game-save slot or system save of either build.
    bool is_save_file(const char* name) const;

private:
    static int index(Side side) { return side == Side::Own ? 0 : 1; }

    kakarot::Build own_;
    kakarot::Build foreign_;
    std::string slots_[2][kSlotCount];
    std::string system_[2];
};

struct Config {
    kakarot::Build own = kakarot::Build::Unknown;
    kakarot::Key key{};
    std::wstring local_manifest_path;  // the copy next to the DLL
};

struct Session {
    explicit Session(const Config& config);

    Model model;
    kakarot::Key key;
    std::wstring local_manifest_path;
    manifest::Manifest manifest;
    bool manifest_loaded = false;
    // Slots converted at save time whose Headers entry waits for the next own system-save write.
    std::vector<manifest::Entry> pending;
};

// Loads the manifest on first use (cloud, then the local copy).
void ensure_manifest(Session& session);

// Persists the manifest (cloud and local copy) and logs the outcome.
void save_manifest(Session& session);

// "HD->standard" style direction label for a conversion toward `target`.
std::string direction(const Model& model, Side target);

}  // namespace plugin
