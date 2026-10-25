// AES-256 in ECB mode (FIPS-197), as used by the game's FAES::EncryptData /
// FAES::DecryptData on save files.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kakarot {

using Bytes = std::vector<uint8_t>;
using Key = std::array<uint8_t, 32>;

namespace aes {

constexpr size_t kBlockSize = 16;

// Encrypts `size` bytes in place; returns false if size is not a multiple of 16.
bool encrypt_ecb(const Key& key, uint8_t* data, size_t size);

// Decrypts `size` bytes in place; returns false if size is not a multiple of 16.
bool decrypt_ecb(const Key& key, uint8_t* data, size_t size);

// Appends zero bytes until the size is a multiple of 16 (no-op if already aligned).
void pad16(Bytes& data);

}  // namespace aes
}  // namespace kakarot
