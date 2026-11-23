// Orca Core — AES-128 (FIPS-197) and CBC (SP 800-38A). See aes.hpp.
//
// The S-box is built from its definition (FIPS-197 5.1.1: multiplicative
// inverse in GF(2^8) followed by the affine transformation) instead of being
// typed in; the tests check it against FIPS-197 Figure 7 entries and the full
// cipher against the Appendix C vectors.
#include "orca/crypto/aes.hpp"

namespace orca::crypto {

namespace {

constexpr u8 xtime(u8 x) noexcept { return static_cast<u8>((static_cast<unsigned>(x) << 1U) ^ ((x & 0x80U) != 0U ? 0x1BU : 0x00U)); }

constexpr u8 gmul(u8 a, u8 b) noexcept {
    u8 r = 0;
    while (b != 0U) {
        if ((b & 1U) != 0U) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

struct Tables {
    std::array<u8, 256> sbox{}, inv{};
    constexpr Tables() {
        for (unsigned x = 0; x < 256; ++x) {
            // multiplicative inverse: x^254 in GF(2^8) (0 -> 0)
            u8 v = 1, base = static_cast<u8>(x);
            for (unsigned e = 254; e != 0U; e >>= 1U) {
                if ((e & 1U) != 0U) v = gmul(v, base);
                base = gmul(base, base);
            }
            u8 s = v;
            for (unsigned i = 1; i <= 4; ++i) s ^= static_cast<u8>((v << i) | (v >> (8U - i)));
            s ^= 0x63U;
            sbox[x] = s;
            inv[s] = static_cast<u8>(x);
        }
    }
};
constexpr Tables kT{};

u32 sub_word(u32 w) noexcept {
    return u32{kT.sbox[w >> 24]} << 24 | u32{kT.sbox[(w >> 16) & 0xFFU]} << 16 | u32{kT.sbox[(w >> 8) & 0xFFU]} << 8 |
           kT.sbox[w & 0xFFU];
}

using State = std::array<u8, 16>; // byte r + 4c = row r, column c

void add_round_key(State& s, const u32* k) noexcept {
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r) s[r + 4 * c] ^= static_cast<u8>(k[c] >> (24U - 8U * r));
}

} // namespace

Aes128::Aes128(std::span<const u8, 16> key) noexcept {
    // FIPS-197 5.2 (Nk = 4, Nr = 10)
    for (unsigned i = 0; i < 4; ++i)
        round_keys_[i] = u32{key[4 * i]} << 24 | u32{key[4 * i + 1]} << 16 | u32{key[4 * i + 2]} << 8 | key[4 * i + 3];
    u8 rcon = 1;
    for (unsigned i = 4; i < 44; ++i) {
        u32 t = round_keys_[i - 1];
        if (i % 4U == 0U) {
            t = sub_word((t << 8) | (t >> 24)) ^ (u32{rcon} << 24);
            rcon = xtime(rcon);
        }
        round_keys_[i] = round_keys_[i - 4] ^ t;
    }
}

void Aes128::encrypt_block(const u8* in, u8* out) const noexcept {
    State s{};
    for (unsigned i = 0; i < 16; ++i) s[i] = in[i];
    add_round_key(s, &round_keys_[0]);
    for (unsigned round = 1; round <= 10; ++round) {
        for (auto& b : s) b = kT.sbox[b];
        State t = s; // ShiftRows: row r rotates left by r
        for (unsigned r = 1; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c) s[r + 4 * c] = t[r + 4 * ((c + r) % 4U)];
        if (round != 10U)
            for (unsigned c = 0; c < 4; ++c) { // MixColumns
                const u8 a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c] = static_cast<u8>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                s[4 * c + 1] = static_cast<u8>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                s[4 * c + 2] = static_cast<u8>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                s[4 * c + 3] = static_cast<u8>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        add_round_key(s, &round_keys_[4 * round]);
    }
    for (unsigned i = 0; i < 16; ++i) out[i] = s[i];
}

void Aes128::decrypt_block(const u8* in, u8* out) const noexcept {
    // FIPS-197 5.3 (inverse cipher)
    State s{};
    for (unsigned i = 0; i < 16; ++i) s[i] = in[i];
    add_round_key(s, &round_keys_[40]);
    for (unsigned round = 9;; --round) {
        State t = s; // InvShiftRows: row r rotates right by r
        for (unsigned r = 1; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c) s[r + 4 * ((c + r) % 4U)] = t[r + 4 * c];
        for (auto& b : s) b = kT.inv[b];
        add_round_key(s, &round_keys_[4 * round]);
        if (round == 0U) break;
        for (unsigned c = 0; c < 4; ++c) { // InvMixColumns
            const u8 a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
            s[4 * c] = static_cast<u8>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
            s[4 * c + 1] = static_cast<u8>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
            s[4 * c + 2] = static_cast<u8>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
            s[4 * c + 3] = static_cast<u8>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
        }
    }
    for (unsigned i = 0; i < 16; ++i) out[i] = s[i];
}

bool Aes128::cbc_decrypt(std::span<u8> data, Block& iv) const noexcept {
    if (data.size() % 16U != 0U) return false;
    for (std::size_t off = 0; off < data.size(); off += 16U) {
        Block c{}, p{};
        for (unsigned i = 0; i < 16; ++i) c[i] = data[off + i];
        decrypt_block(c.data(), p.data());
        for (unsigned i = 0; i < 16; ++i) data[off + i] = static_cast<u8>(p[i] ^ iv[i]);
        iv = c;
    }
    return true;
}

bool Aes128::cbc_encrypt(std::span<u8> data, Block& iv) const noexcept {
    if (data.size() % 16U != 0U) return false;
    for (std::size_t off = 0; off < data.size(); off += 16U) {
        Block p{};
        for (unsigned i = 0; i < 16; ++i) p[i] = static_cast<u8>(data[off + i] ^ iv[i]);
        encrypt_block(p.data(), iv.data());
        for (unsigned i = 0; i < 16; ++i) data[off + i] = iv[i];
    }
    return true;
}

} // namespace orca::crypto
