// Runtime lookup of the save-file AES key inside a game executable.
//
// Both builds embed the key as the only NUL-terminated run of exactly 128
// characters in [a-z0-9]; the 256-bit key schedule uses its first 32 bytes.
// The key is never stored anywhere by this library.
#pragma once

#include <string>

#include <windows.h>

#include "aes.h"

namespace kakarot {
namespace key {

// Scans the PE file at `exe_path` on disk; false (with `error`) unless exactly one candidate exists.
bool find_in_file(const std::wstring& exe_path, Key& key, std::string* error = nullptr);

// Scans the readable sections of a module mapped in this process; same rule as find_in_file.
bool find_in_module(HMODULE module, Key& key, std::string* error = nullptr);

// Returns true if decrypting the first 16 bytes of an encrypted save yields the "GVAS" tag.
bool verify_key(const Key& key, const uint8_t* encrypted, size_t size);

// Returns true if decrypting the first 16 bytes of an encrypted save yields the "GVAS" tag.
inline bool verify_key(const Key& key, const Bytes& encrypted) {
    return verify_key(key, encrypted.data(), encrypted.size());
}

}  // namespace key
}  // namespace kakarot
