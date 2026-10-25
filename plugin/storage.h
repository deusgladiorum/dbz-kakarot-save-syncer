// The plugin's own Remote Storage I/O.
//
// Every call goes straight to the original v014 functions (never through the patched vtable)
// and runs with a thread-local "inside plugin" flag set, so a hook that is reached anyway (for
// example if Steam calls back through the interface) forwards without running sync logic.
// Each call is logged with its size, time and outcome.
#pragma once

#include <cstdint>
#include <string>

#include "core/aes.h"

namespace plugin {
namespace storage {

// v014 vtable indices used by the plugin's own I/O.
constexpr int kFileWriteIndex = 0;
constexpr int kFileReadIndex = 1;
constexpr int kFileExistsIndex = 13;
constexpr int kGetFileSizeIndex = 15;
constexpr int kGetFileTimestampIndex = 16;

// Captures `object` and its original functions. Call before the hooks are patched in; a slot
// that already holds a hook is resolved to the hook's saved original.
void bind(void* object);
void unbind();
bool ready();

// True while the plugin's own Remote Storage call is in progress on this thread.
bool inside_plugin();

bool exists(const std::string& name);

// Steam's file time (Unix seconds), or 0 if unknown.
int64_t timestamp(const std::string& name);

// Reads a whole file (GetFileSize + FileRead); false if missing, empty or short.
bool read(const std::string& name, kakarot::Bytes& out);

// FileWrite; false (logged as REFUSED) if Steam refuses it. Never retried.
bool write(const std::string& name, const kakarot::Bytes& data);

}  // namespace storage
}  // namespace plugin
