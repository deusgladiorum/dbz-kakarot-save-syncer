// Replacement functions for the hooked ISteamRemoteStorage (v014) vtable slots.
//
// Each replacement logs the game's call, gives the sync logic its turn (the reconcile guard and
// fallback trigger before forwarding; the save-time and delete handling after), and returns the
// original's result unchanged. Calls made while the plugin's own I/O is in progress are forwarded as is.
#pragma once

#include <cstdint>

namespace plugin {
namespace hooks {

enum Function { kFileWrite, kFileRead, kFileDelete, kFileExists, kFunctionCount };

struct Slot {
    const char* name;
    int vtable_index;
    void* replacement;
};

// The hooked functions with their v014 vtable indices.
const Slot& slot(int function);

// Must be set before the slot is patched; the replacement forwards to it.
void set_original(int function, void* original);
void* original(int function);

// Number of game calls per function so far (calls forwarded for the plugin's own I/O excluded).
void counts(int64_t out[kFunctionCount]);

}  // namespace hooks
}  // namespace plugin
