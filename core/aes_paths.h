// The two AES-256 ECB implementations behind aes.h, exposed for tests and benchmarks.
//
// aes::encrypt_ecb / aes::decrypt_ecb use the hardware (AES-NI) path when the CPU supports it
// and the software (FIPS-197) path otherwise. Both produce identical bytes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "aes.h"

namespace kakarot {
namespace aes {
namespace paths {

// CPUID leaf 1, ECX bit 25; checked once per process.
bool hardware_supported();

// Same contract as aes::encrypt_ecb / aes::decrypt_ecb (false if size is not a multiple of 16).
bool encrypt_ecb_software(const Key& key, uint8_t* data, size_t size);
bool decrypt_ecb_software(const Key& key, uint8_t* data, size_t size);

// Also false, with the data untouched, if the CPU has no AES-NI.
bool encrypt_ecb_hardware(const Key& key, uint8_t* data, size_t size);
bool decrypt_ecb_hardware(const Key& key, uint8_t* data, size_t size);

}  // namespace paths
}  // namespace aes
}  // namespace kakarot
