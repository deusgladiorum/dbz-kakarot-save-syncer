// The sync logic the hooks call into: the reconcile pass and its guard, the save-time hook and the
// delete record. Everything runs synchronously under one lock: the reconcile pass on the plugin's
// wait thread (on the game's thread only as a fallback), the rest on the game's thread inside the
// hooked call. Every failure is logged and swallowed: the game's own call has already been
// decided and is never affected.
#pragma once

#include <cstdint>

#include "plugin/session.h"

namespace plugin {
namespace sync {

// Starts a fresh session (no pending slots, reconcile not run, manifest not loaded).
void init(const Config& config);

// Drops the session; later calls do nothing.
void shutdown();

// On the plugin's wait thread, right after the hooks are installed: runs the reconcile pass now,
// once per session (nothing if it has already started).
void reconcile_early();

// Before any hooked call is forwarded: while the reconcile pass runs, a call naming a save file
// (either build's slots and system save) or the manifest waits for it to finish; other names
// pass through. If the pass has not started by the first call naming the own system save, that
// call runs it inline first (fallback).
void before_call(const char* name);

// After FileWrite: an own slot is converted to the foreign format and becomes pending; an own
// system-save write then merges the pending slots' Headers entries into the foreign one.
void after_write(const char* name, const void* data, int32_t size, bool written);

// Before FileDelete: hashes the own slot about to be deleted (0 if it is not an own slot).
uint64_t before_delete(const char* name);

// After FileDelete: records the deletion in the manifest. Never deletes the foreign copy.
void after_delete(const char* name, bool deleted, uint64_t deleted_hash);

}  // namespace sync
}  // namespace plugin
