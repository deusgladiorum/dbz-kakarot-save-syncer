// Patching the hooked slots (hooks.h) into a live ISteamRemoteStorage object's vtable, and
// putting the originals back.
#pragma once

#include <cstddef>

namespace plugin {
namespace vtable {

// Patches every hooked slot of `object`'s vtable; returns the number patched (logged).
int install(void* object);

// Puts back the original pointers in every slot that still holds a replacement.
void restore();

// True if every patched slot still holds its replacement.
bool intact();

// Writes "<module file name>+0x<offset>" for an address, or "not in a module". With
// `in_module`, also reports whether the address lies in the module named `module_name`.
void describe_address(const void* address, char* out, size_t size, const wchar_t* module_name = nullptr,
                      bool* in_module = nullptr);

}  // namespace vtable
}  // namespace plugin
