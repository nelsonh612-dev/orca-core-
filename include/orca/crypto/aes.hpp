// Orca Core — AES-128 (FIPS-197) with CBC mode (NIST SP 800-38A), for Wii disc
// partitions, titles and NAND content. Written from the FIPS-197 specification;
// checked against its Appendix B/C vectors and SP 800-38A F.2 (tests/test_crypto.cpp).
#pragma once
#include "orca/core/bits.hpp"
#include "orca/crypto/secure.hpp"

#include <array>
#include <span>

namespace orca::crypto {

class Aes128 {
public:
    using Block = std::array<u8, 16>;
    explicit Aes128(std::span<const u8, 16> key) noexcept;
    ~Aes128() { secure_zero(round_keys_.data(), sizeof round_keys_); }
    Aes128(const Aes128&) = default;
    Aes128& operator=(const Aes128&) = default;

    void encrypt_block(const u8* in, u8* out) const noexcept;
    void decrypt_block(const u8* in, u8* out) const noexcept;

    // CBC over whole 16-byte blocks; `data.size()` must be a multiple of 16
    // (false otherwise, nothing changed). `iv` is updated to the last ciphertext
    // block, so consecutive calls continue one stream. In-place is allowed.
    bool cbc_decrypt(std::span<u8> data, Block& iv) const noexcept;
    bool cbc_encrypt(std::span<u8> data, Block& iv) const noexcept;

    // The ARMv8 Cryptography Extension path (AArch64 CPUs that have it; same
    // results as the portable code). enabled() can be switched off to compare.
    static bool hardware_available() noexcept;
    static bool hardware_enabled() noexcept;
    static void set_hardware_enabled(bool on) noexcept;

private:
    std::array<u32, 44> round_keys_{};
};

} // namespace orca::crypto
