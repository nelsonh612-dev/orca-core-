#pragma once
#include "orca/core/bits.hpp"
#include <array>
#include <span>
#include <string>
namespace orca::crypto {
using Aes128Key = std::array<u8,16>;
using Aes128Iv = std::array<u8,16>;
// Decrypt complete blocks in place. No key material is embedded in Orca.
bool aes128_cbc_decrypt(const Aes128Key&, const Aes128Iv&, std::span<u8>, std::string&);
}
