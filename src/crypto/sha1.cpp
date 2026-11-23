// Orca Core — SHA-1 (FIPS 180-4, section 6.1). See sha1.hpp.
#include "orca/crypto/sha1.hpp"

#include <cstring>

namespace orca::crypto {

namespace {
constexpr u32 rotl(u32 x, unsigned n) noexcept { return (x << n) | (x >> (32U - n)); }
} // namespace

void Sha1::reset() noexcept {
    h_ = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    used_ = 0;
    length_ = 0;
}

void Sha1::block(const u8* p) noexcept {
    std::array<u32, 80> w{};
    for (unsigned t = 0; t < 16; ++t)
        w[t] = u32{p[4 * t]} << 24 | u32{p[4 * t + 1]} << 16 | u32{p[4 * t + 2]} << 8 | u32{p[4 * t + 3]};
    for (unsigned t = 16; t < 80; ++t) w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (unsigned t = 0; t < 80; ++t) {
        u32 f, k;
        if (t < 20) { f = (b & c) | (~b & d); k = 0x5A827999U; }
        else if (t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1U; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCU; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6U; }
        const u32 temp = rotl(a, 5) + f + e + k + w[t];
        e = d; d = c; c = rotl(b, 30); b = a; a = temp;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update(std::span<const u8> data) noexcept {
    length_ += data.size();
    std::size_t i = 0;
    if (used_ != 0U) {
        const std::size_t n = std::min(data.size(), 64U - used_);
        std::memcpy(buf_.data() + used_, data.data(), n);
        used_ += n;
        i = n;
        if (used_ < 64U) return;
        block(buf_.data());
        used_ = 0;
    }
    for (; i + 64U <= data.size(); i += 64U) block(data.data() + i);
    if (i < data.size()) { std::memcpy(buf_.data(), data.data() + i, data.size() - i); used_ = data.size() - i; }
}

Sha1Digest Sha1::finish() noexcept {
    const u64 bits = length_ * 8U;
    const u8 one = 0x80;
    update(std::span<const u8>(&one, 1));
    const u8 zero = 0;
    while (used_ != 56U) update(std::span<const u8>(&zero, 1));
    std::array<u8, 8> len{};
    for (unsigned k = 0; k < 8; ++k) len[k] = static_cast<u8>(bits >> (56U - 8U * k));
    update(len);
    Sha1Digest out{};
    for (unsigned k = 0; k < 5; ++k)
        for (unsigned j = 0; j < 4; ++j) out[4 * k + j] = static_cast<u8>(h_[k] >> (24U - 8U * j));
    return out;
}

Sha1Digest sha1(std::span<const u8> data) noexcept {
    Sha1 s;
    s.update(data);
    return s.finish();
}

} // namespace orca::crypto
