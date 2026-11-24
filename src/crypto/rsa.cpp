// Orca Core — RSA signature verification. See rsa.hpp.
#include "orca/crypto/rsa.hpp"

#include <algorithm>
#include <cstring>

namespace orca::crypto {

namespace {

using Limbs = std::vector<u32>; // little-endian 32-bit limbs

Limbs from_bytes(std::span<const u8> be, std::size_t limbs) {
    Limbs r(limbs, 0);
    for (std::size_t i = 0; i < be.size(); ++i) {
        const std::size_t bit = (be.size() - 1U - i) * 8U;
        if (bit / 32U < limbs) r[bit / 32U] |= u32{be[i]} << (bit % 32U);
    }
    return r;
}

std::vector<u8> to_bytes(const Limbs& a, std::size_t len) {
    std::vector<u8> out(len, 0);
    for (std::size_t i = 0; i < len; ++i) {
        const std::size_t bit = (len - 1U - i) * 8U;
        if (bit / 32U < a.size()) out[i] = static_cast<u8>(a[bit / 32U] >> (bit % 32U));
    }
    return out;
}

// a >= b (same length)
bool geq(const Limbs& a, const Limbs& b) {
    for (std::size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return true;
}

void sub_in_place(Limbs& a, const Limbs& b) {
    u64 borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const u64 d = u64{a[i]} - b[i] - borrow;
        a[i] = static_cast<u32>(d);
        borrow = (d >> 63) & 1U;
    }
}

class Montgomery {
public:
    explicit Montgomery(const Limbs& n) : n_(n), k_(n.size()) {
        // n0inv = -n^-1 mod 2^32 (Newton iteration; n is odd)
        u32 inv = 1;
        for (int i = 0; i < 5; ++i) inv *= 2U - n[0] * inv;
        n0inv_ = 0U - inv;
        // R^2 mod n by doubling 1, 2 * 32k times
        Limbs r(k_, 0);
        r[0] = 1;
        for (std::size_t i = 0; i < 64U * k_; ++i) {
            u32 carry = 0;
            for (std::size_t j = 0; j < k_; ++j) {
                const u32 next = r[j] >> 31;
                r[j] = r[j] << 1 | carry;
                carry = next;
            }
            if (carry != 0U || geq(r, n_)) sub_in_place(r, n_);
        }
        r2_ = r;
    }

    // a * b * R^-1 mod n (CIOS)
    Limbs mul(const Limbs& a, const Limbs& b) const {
        std::vector<u32> t(k_ + 2, 0);
        for (std::size_t i = 0; i < k_; ++i) {
            u64 c = 0;
            for (std::size_t j = 0; j < k_; ++j) {
                const u64 s = u64{t[j]} + u64{a[j]} * b[i] + c;
                t[j] = static_cast<u32>(s);
                c = s >> 32;
            }
            u64 s = u64{t[k_]} + c;
            t[k_] = static_cast<u32>(s);
            t[k_ + 1] = static_cast<u32>(s >> 32);
            const u32 m = t[0] * n0inv_;
            s = u64{t[0]} + u64{m} * n_[0];
            c = s >> 32;
            for (std::size_t j = 1; j < k_; ++j) {
                s = u64{t[j]} + u64{m} * n_[j] + c;
                t[j - 1] = static_cast<u32>(s);
                c = s >> 32;
            }
            s = u64{t[k_]} + c;
            t[k_ - 1] = static_cast<u32>(s);
            t[k_] = t[k_ + 1] + static_cast<u32>(s >> 32);
        }
        Limbs r(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(k_));
        if (t[k_] != 0U || geq(r, n_)) sub_in_place(r, n_);
        return r;
    }

    Limbs to_mont(const Limbs& a) const { return mul(a, r2_); }
    Limbs from_mont(const Limbs& a) const {
        Limbs one(k_, 0);
        one[0] = 1;
        return mul(a, one);
    }

private:
    Limbs n_;
    std::size_t k_;
    u32 n0inv_{0};
    Limbs r2_;
};

} // namespace

bool rsa_raw(std::span<const u8> base, std::span<const u8> exponent, std::span<const u8> modulus, std::vector<u8>& out) {
    const std::size_t k = (modulus.size() + 3U) / 4U;
    if (k == 0U || base.size() > modulus.size()) return false;
    const Limbs n = from_bytes(modulus, k);
    if ((n[0] & 1U) == 0U) return false;
    const Limbs b = from_bytes(base, k);
    if (geq(b, n)) return false;
    const Montgomery mont(n);
    const Limbs bm = mont.to_mont(b);
    Limbs one(k, 0);
    one[0] = 1;
    Limbs acc = mont.to_mont(one);
    for (const u8 byte : exponent) {
        for (int bit = 7; bit >= 0; --bit) {
            acc = mont.mul(acc, acc);
            if (((byte >> bit) & 1U) != 0U) acc = mont.mul(acc, bm);
        }
    }
    out = to_bytes(mont.from_mont(acc), modulus.size());
    return true;
}

std::vector<u8> pkcs1_sha1_encoding(const Sha1Digest& digest, std::size_t length) {
    static constexpr u8 kDigestInfo[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14};
    std::vector<u8> em(length, 0xFF);
    if (length < 3U + sizeof kDigestInfo + digest.size() + 8U) return {};
    em[0] = 0x00;
    em[1] = 0x01;
    const std::size_t t = sizeof kDigestInfo + digest.size();
    em[length - t - 1U] = 0x00;
    std::memcpy(&em[length - t], kDigestInfo, sizeof kDigestInfo);
    std::memcpy(&em[length - digest.size()], digest.data(), digest.size());
    return em;
}

bool rsa_verify_sha1(const RsaPublicKey& key, std::span<const u8> signature, const Sha1Digest& digest) {
    if (key.modulus.empty() || signature.size() != key.modulus.size() || key.exponent == 0U) return false;
    const u8 e[4] = {static_cast<u8>(key.exponent >> 24), static_cast<u8>(key.exponent >> 16),
                     static_cast<u8>(key.exponent >> 8), static_cast<u8>(key.exponent)};
    std::vector<u8> m;
    if (!rsa_raw(signature, e, key.modulus, m)) return false;
    const std::vector<u8> em = pkcs1_sha1_encoding(digest, key.modulus.size());
    return !em.empty() && m == em;
}

} // namespace orca::crypto
