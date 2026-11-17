// Portable AES-128-CBC decryption primitive for Wii optical-disc clusters.
// The caller supplies the title key; no Nintendo keys are embedded in Orca.
#pragma once
#include "orca/core/bits.hpp"
#include <array>
#include <span>

namespace orca::crypto {
using Aes128Key = std::array<u8, 16>;
using Aes128Iv = std::array<u8, 16>;

// Decrypts complete AES-CBC blocks in place. Input length must be a multiple
// of 16. The IV is updated to the final ciphertext block.
bool aes128_cbc_decrypt(const Aes128Key& key, Aes128Iv& iv, std::span<u8> data) noexcept;
// Encrypts complete AES-CBC blocks in place; primarily useful for round-trip and format tests.
bool aes128_cbc_encrypt(const Aes128Key& key, Aes128Iv& iv, std::span<u8> data) noexcept;
} // namespace orca::crypto
