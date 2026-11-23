// Orca Core — SHA-1 (FIPS 180-4), for the Wii disc hash tree (H0-H3) and TMD
// content hashes. Written from the specification; checked against its examples.
#pragma once
#include "orca/core/bits.hpp"

#include <array>
#include <span>

namespace orca::crypto {

using Sha1Digest = std::array<u8, 20>;

class Sha1 {
public:
    Sha1() noexcept { reset(); }
    void reset() noexcept;
    void update(std::span<const u8> data) noexcept;
    Sha1Digest finish() noexcept; // the object must be reset() before reuse

private:
    void block(const u8* p) noexcept;
    std::array<u32, 5> h_{};
    std::array<u8, 64> buf_{};
    std::size_t used_{0};
    u64 length_{0};
};

Sha1Digest sha1(std::span<const u8> data) noexcept;

} // namespace orca::crypto
