// Orca Core — Wii signed metadata: tickets, TMDs, certificate chains and the
// verification of their RSA signatures.
//
// Layouts (WiiBrew "Ticket", "Title metadata", "Certificate chain"; Dolphin
// Core/IOS/IOSC.h and Core/IOS/ES/Formats.h, read as information):
//   Signature block: type u32 (0x10000 RSA-4096, 0x10001 RSA-2048, 0x10002 ECC),
//     signature (0x200 / 0x100 / 0x3C), padding (0x3C / 0x3C / 0x40), then the
//     issuer (0x40 bytes) at 0x240 / 0x140 / 0x80. Everything from the issuer to
//     the end of the blob is what is signed (SHA-1).
//   Ticket (v0, 0x2A4 bytes, RSA-2048): version 0x1BC, encrypted title key 0x1BF,
//     ticket ID 0x1D0, console ID 0x1D8, title ID 0x1DC, common key index 0x1F1.
//   TMD (RSA-2048): version 0x180, IOS title 0x184, title ID 0x18C, title type
//     0x194, group 0x198, region 0x19C, access rights 0x1D8, title version 0x1DC,
//     content count 0x1DE, boot index 0x1E0; contents of 0x24 bytes from 0x1E4
//     {ID u32, index u16, type u16, size u64, SHA-1}. Size 0x1E4 + 0x24 * count,
//     at most 0x49E4 (as Dolphin's IsValidTMDSize).
//   Certificate: signature block, key type u32 (0 RSA-4096, 1 RSA-2048, 2 ECC),
//     name (0x40), ID u32, public key; sizes 0x400 (RSA-4096 signed, RSA-2048 key),
//     0x300 (RSA-2048/RSA-2048), 0x240 (RSA-2048/ECC), 0x180 (ECC/ECC).
//
// A blob's issuer "Root-CA00000001-XS00000003" names its signer (XS00000003)
// and the signer's own issuer ("Root-CA00000001"). The chain on a disc holds the
// CA, CP (TMD signer) and XS (ticket signer) certificates; the CA certificate is
// signed with the root key, which is not on the disc and not part of Orca: it is
// checked only when the user supplies it. As in Dolphin, signatures do not stop a
// disc from being read (fakesigned images exist); they are reported.
#pragma once
#include "orca/core/bits.hpp"
#include "orca/crypto/rsa.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orca::disc {

inline constexpr u32 kSigRsa4096 = 0x00010000U, kSigRsa2048 = 0x00010001U, kSigEcc = 0x00010002U;
inline constexpr std::size_t kTicketSize = 0x2A4, kTmdHeaderSize = 0x1E4, kTmdContentSize = 0x24, kTmdMaxSize = 0x49E4;

// Size of the signature block up to the issuer (0x240 / 0x140 / 0x80); 0 if unknown.
std::size_t signature_issuer_offset(u32 type) noexcept;

struct SignedBlob {
    u32 type{0};
    std::vector<u8> signature;
    std::string issuer;
    std::size_t issuer_offset{0};
};
std::optional<SignedBlob> parse_signature(std::span<const u8> blob, std::string& error);

struct Ticket {
    SignedBlob sig;
    u8 version{0};
    std::array<u8, 16> encrypted_title_key{};
    u64 ticket_id{0};
    u32 console_id{0};
    u64 title_id{0};
    u8 common_key_index{0};
    std::vector<u8> bytes; // exactly kTicketSize
};
// A v0 ticket of exactly 0x2A4 bytes signed with RSA-2048, title key index 0-2.
std::optional<Ticket> parse_ticket_checked(std::span<const u8> bytes, std::string& error);

struct TmdContent {
    u32 id{0};
    u16 index{0};
    u16 type{0};
    u64 size{0};
    std::array<u8, 20> sha1{};
};
struct Tmd {
    SignedBlob sig;
    u8 version{0};
    u64 ios_id{0};
    u64 title_id{0};
    u32 title_type{0};
    u16 group_id{0};
    u16 region{0};
    u32 access_rights{0};
    u16 title_version{0};
    u16 boot_index{0};
    std::vector<TmdContent> contents;
    std::vector<u8> bytes;
};
std::optional<Tmd> parse_tmd_checked(std::span<const u8> bytes, std::string& error);

struct Certificate {
    SignedBlob sig;
    u32 key_type{0};
    std::string name;
    u32 id{0};
    crypto::RsaPublicKey key; // empty modulus for ECC keys
    std::vector<u8> bytes;    // the whole certificate
};
// Splits a chain into certificates; the chain must consist only of whole, known
// certificate structures.
std::optional<std::vector<Certificate>> parse_cert_chain(std::span<const u8> chain, std::string& error);

enum class SignatureStatus {
    Valid,          // the RSA signature verifies
    Invalid,        // it does not (wrong signature or modified data)
    Fakesigned,     // all-zero signature (the "Trucha" fake signing of modified images)
    MissingSigner,  // the signer's certificate is not in the chain
    NoRootKey,      // signed by the root key, which was not supplied
    Unsupported,    // ECC signatures (not used for disc tickets and TMDs)
};
const char* signature_status_name(SignatureStatus s) noexcept;

struct ChainReport {
    SignatureStatus ticket{SignatureStatus::MissingSigner};
    SignatureStatus tmd{SignatureStatus::MissingSigner};
    SignatureStatus ticket_signer{SignatureStatus::MissingSigner}; // XS certificate
    SignatureStatus tmd_signer{SignatureStatus::MissingSigner};    // CP certificate
    SignatureStatus ca{SignatureStatus::MissingSigner};            // CA certificate (root key)
    std::string detail;
    // true when every signature that can be checked with the material on the disc
    // (ticket, TMD, XS, CP) verifies; the CA one additionally needs the root key.
    bool disc_chain_valid() const noexcept;
};
ChainReport verify_chain(const Ticket& ticket, const Tmd& tmd, const std::vector<Certificate>& certs,
                         const crypto::RsaPublicKey* root_key);

// The root public key the user supplies: a raw RSA-4096 modulus (512 bytes,
// exponent 65537), the modulus followed by its 4-byte exponent (516 bytes), or a
// single certificate carrying an RSA key.
std::optional<crypto::RsaPublicKey> parse_root_key(std::span<const u8> data, std::string& error);

} // namespace orca::disc
