// The save conversions the plugin performs. Every function takes and returns
// encrypted file contents; the output is byte-identical to the Python tools in
// tools/ (kakarot_transfer.py, kakarot_system.py).
#pragma once

#include <string>
#include <vector>

#include "aes.h"

namespace kakarot {
namespace ops {

// Decrypts a whole save file; false if its size is not a multiple of 16.
bool decrypt(const Bytes& encrypted, const Key& key, Bytes& plain, std::string* error = nullptr);

// Zero-pads to 16 bytes and encrypts.
Bytes encrypt(Bytes plain, const Key& key);

// Converts a ManualSave/AutoSave to the build of `ref` (engine header and Version from `ref`).
bool convert_game_save(const Bytes& src, const Bytes& ref, const Key& key, Bytes& out,
                       std::string* error = nullptr);

// Copies Headers[slot] for each slot from system save `src` into system save `dst`.
bool merge_headers(const Bytes& src, const Bytes& dst, const Key& key, const std::vector<int>& slots,
                   Bytes& out, std::string* error = nullptr);

// Converts a whole system save to the build of `ref`; `to_hd` says which direction that is.
bool convert_system_save(const Bytes& src, const Bytes& ref, const Key& key, bool to_hd, Bytes& out,
                         std::string* error = nullptr);

// Lists the slot indices of a system save's Headers entries, sorted ascending.
bool header_slots(const Bytes& encrypted, const Key& key, std::vector<int>& slots,
                  std::string* error = nullptr);

}  // namespace ops
}  // namespace kakarot
