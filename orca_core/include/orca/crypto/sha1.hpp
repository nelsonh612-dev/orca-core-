// Small SHA-1 implementation used only to validate Wii disc cluster hashes.
#pragma once
#include "orca/core/bits.hpp"
#include <array>
#include <span>

namespace orca::crypto {
using Sha1Digest = std::array<u8,20>;
Sha1Digest sha1(std::span<const u8> data);
} // namespace orca::crypto
