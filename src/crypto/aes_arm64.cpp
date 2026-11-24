// Orca Core — AES-128-CBC with the ARMv8 Cryptography Extension (AESE/AESD/
// AESMC/AESIMC), used by crypto::Aes128 when the CPU has it (checked at run time).
// Built only for AArch64, with +crypto for this file alone. The round keys come
// from Aes128's own FIPS-197 key expansion; the results are those of the portable
// code (checked by the crypto tests on ARM64).
#if defined(__aarch64__)
#include <arm_neon.h>

#include <cstddef>
#include <cstdint>

namespace orca::crypto::detail {

// rk: the 11 encryption round keys, 16 bytes each, in FIPS-197 byte order.
void aes_hw_cbc_decrypt(const std::uint8_t* rk, std::uint8_t* data, std::size_t size, std::uint8_t* iv_bytes) {
    uint8x16_t ek[11], dk[11];
    for (int i = 0; i < 11; ++i) ek[i] = vld1q_u8(rk + 16 * i);
    // equivalent inverse cipher: the middle round keys go through InvMixColumns
    dk[0] = ek[10];
    for (int i = 1; i < 10; ++i) dk[i] = vaesimcq_u8(ek[10 - i]);
    dk[10] = ek[0];
    uint8x16_t iv = vld1q_u8(iv_bytes);
    for (std::size_t off = 0; off < size; off += 16) {
        const uint8x16_t c = vld1q_u8(data + off);
        uint8x16_t s = c;
        for (int i = 0; i < 9; ++i) s = vaesimcq_u8(vaesdq_u8(s, dk[i]));
        s = veorq_u8(vaesdq_u8(s, dk[9]), dk[10]);
        vst1q_u8(data + off, veorq_u8(s, iv));
        iv = c;
    }
    vst1q_u8(iv_bytes, iv);
}

void aes_hw_cbc_encrypt(const std::uint8_t* rk, std::uint8_t* data, std::size_t size, std::uint8_t* iv_bytes) {
    uint8x16_t ek[11];
    for (int i = 0; i < 11; ++i) ek[i] = vld1q_u8(rk + 16 * i);
    uint8x16_t iv = vld1q_u8(iv_bytes);
    for (std::size_t off = 0; off < size; off += 16) {
        uint8x16_t s = veorq_u8(vld1q_u8(data + off), iv);
        for (int i = 0; i < 9; ++i) s = vaesmcq_u8(vaeseq_u8(s, ek[i]));
        s = veorq_u8(vaeseq_u8(s, ek[9]), ek[10]);
        vst1q_u8(data + off, s);
        iv = s;
    }
    vst1q_u8(iv_bytes, iv);
}

} // namespace orca::crypto::detail
#endif
