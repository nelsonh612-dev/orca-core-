// Orca Core — Wii tickets, TMDs, certificate chains and their signatures. See wii_meta.hpp.
#include "orca/disc/wii_meta.hpp"

#include "orca/crypto/sha1.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace orca::disc {

namespace {
u32 be32(const u8* p) noexcept { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }
u64 be64(const u8* p) noexcept { return u64{be32(p)} << 32 | be32(p + 4); }
u16 be16(const u8* p) noexcept { return static_cast<u16>(p[0] << 8 | p[1]); }

std::string fixed_string(const u8* p, std::size_t n) {
    std::size_t len = 0;
    while (len < n && p[len] != 0U) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}

std::size_t signature_length(u32 type) noexcept {
    switch (type) {
    case kSigRsa4096: return 0x200;
    case kSigRsa2048: return 0x100;
    case kSigEcc: return 0x3C;
    default: return 0;
    }
}

std::string hex32(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}
} // namespace

std::size_t signature_issuer_offset(u32 type) noexcept {
    switch (type) {
    case kSigRsa4096: return 0x240;
    case kSigRsa2048: return 0x140;
    case kSigEcc: return 0x80;
    default: return 0;
    }
}

std::optional<SignedBlob> parse_signature(std::span<const u8> blob, std::string& error) {
    if (blob.size() < 4U) { error = "signed data shorter than its signature type"; return std::nullopt; }
    SignedBlob s;
    s.type = be32(blob.data());
    s.issuer_offset = signature_issuer_offset(s.type);
    if (s.issuer_offset == 0U) { error = "unknown signature type " + hex32(s.type); return std::nullopt; }
    if (blob.size() < s.issuer_offset + 0x40U) { error = "signed data truncated inside its signature block"; return std::nullopt; }
    s.signature.assign(blob.begin() + 4, blob.begin() + 4 + static_cast<std::ptrdiff_t>(signature_length(s.type)));
    s.issuer = fixed_string(blob.data() + s.issuer_offset, 0x40);
    return s;
}

std::optional<Ticket> parse_ticket_checked(std::span<const u8> bytes, std::string& error) {
    if (bytes.size() != kTicketSize) {
        error = "ticket: " + std::to_string(bytes.size()) + " bytes instead of 0x2A4";
        return std::nullopt;
    }
    auto sig = parse_signature(bytes, error);
    if (!sig) { error = "ticket: " + error; return std::nullopt; }
    if (sig->type != kSigRsa2048) { error = "ticket: signature type " + hex32(sig->type) + " is not RSA-2048"; return std::nullopt; }
    if (sig->issuer.rfind("Root-", 0) != 0U) { error = "ticket: issuer \"" + sig->issuer + "\" does not start with Root-"; return std::nullopt; }
    Ticket t;
    t.sig = std::move(*sig);
    t.version = bytes[0x1BC];
    if (t.version != 0U) {
        error = "ticket: version " + std::to_string(t.version) + " (disc partitions carry v0 tickets)";
        return std::nullopt;
    }
    std::copy_n(bytes.begin() + 0x1BF, 16, t.encrypted_title_key.begin());
    t.ticket_id = be64(&bytes[0x1D0]);
    t.console_id = be32(&bytes[0x1D8]);
    t.title_id = be64(&bytes[0x1DC]);
    t.common_key_index = bytes[0x1F1];
    if (t.common_key_index > 2U) {
        error = "ticket: unsupported common key index " + std::to_string(t.common_key_index) +
                " (0 = normal, 1 = Korean, 2 = vWii)";
        return std::nullopt;
    }
    t.bytes.assign(bytes.begin(), bytes.end());
    return t;
}

std::optional<Tmd> parse_tmd_checked(std::span<const u8> bytes, std::string& error) {
    if (bytes.size() < kTmdHeaderSize || bytes.size() > kTmdMaxSize) {
        error = "TMD: size " + std::to_string(bytes.size()) + " outside 0x1E4..0x49E4";
        return std::nullopt;
    }
    auto sig = parse_signature(bytes, error);
    if (!sig) { error = "TMD: " + error; return std::nullopt; }
    if (sig->type != kSigRsa2048) { error = "TMD: signature type " + hex32(sig->type) + " is not RSA-2048"; return std::nullopt; }
    if (sig->issuer.rfind("Root-", 0) != 0U) { error = "TMD: issuer \"" + sig->issuer + "\" does not start with Root-"; return std::nullopt; }
    Tmd t;
    t.sig = std::move(*sig);
    t.version = bytes[0x180];
    t.ios_id = be64(&bytes[0x184]);
    t.title_id = be64(&bytes[0x18C]);
    t.title_type = be32(&bytes[0x194]);
    t.group_id = be16(&bytes[0x198]);
    t.region = be16(&bytes[0x19C]);
    t.access_rights = be32(&bytes[0x1D8]);
    t.title_version = be16(&bytes[0x1DC]);
    const u16 count = be16(&bytes[0x1DE]);
    t.boot_index = be16(&bytes[0x1E0]);
    if (count == 0U) { error = "TMD: no contents"; return std::nullopt; }
    const std::size_t need = kTmdHeaderSize + std::size_t{count} * kTmdContentSize;
    if (bytes.size() != need) {
        error = "TMD: " + std::to_string(count) + " contents need " + std::to_string(need) + " bytes, the TMD has " +
                std::to_string(bytes.size());
        return std::nullopt;
    }
    for (u16 i = 0; i < count; ++i) {
        const u8* c = &bytes[kTmdHeaderSize + std::size_t{i} * kTmdContentSize];
        TmdContent e;
        e.id = be32(c);
        e.index = be16(c + 4);
        e.type = be16(c + 6);
        e.size = be64(c + 8);
        std::copy_n(c + 16, 20, e.sha1.begin());
        t.contents.push_back(e);
    }
    t.bytes.assign(bytes.begin(), bytes.end());
    return t;
}

std::optional<std::vector<Certificate>> parse_cert_chain(std::span<const u8> chain, std::string& error) {
    std::vector<Certificate> out;
    std::size_t at = 0;
    if (chain.empty()) { error = "certificate chain: empty"; return std::nullopt; }
    while (at < chain.size()) {
        const std::span<const u8> rest = chain.subspan(at);
        std::string e;
        auto sig = parse_signature(rest, e);
        const std::string where = "certificate chain at 0x" + [&] { char b[24]; std::snprintf(b, sizeof b, "%zX", at); return std::string(b); }();
        if (!sig) { error = where + ": " + e; return std::nullopt; }
        const std::size_t h = sig->issuer_offset + 0x40U; // key type, name, ID follow the issuer
        if (rest.size() < h + 0x48U) { error = where + ": truncated certificate header"; return std::nullopt; }
        Certificate c;
        c.key_type = be32(&rest[h]);
        c.name = fixed_string(&rest[h + 4], 0x40);
        c.id = be32(&rest[h + 0x44]);
        std::size_t size = 0;
        if (sig->type == kSigRsa4096 && c.key_type == 1U) size = 0x400;
        else if (sig->type == kSigRsa2048 && c.key_type == 1U) size = 0x300;
        else if (sig->type == kSigRsa2048 && c.key_type == 2U) size = 0x240;
        else if (sig->type == kSigEcc && c.key_type == 2U) size = 0x180;
        if (size == 0U) {
            error = where + ": unsupported signature/key combination " + hex32(sig->type) + "/" + std::to_string(c.key_type);
            return std::nullopt;
        }
        if (rest.size() < size) { error = where + ": certificate \"" + c.name + "\" is truncated"; return std::nullopt; }
        if (c.key_type == 1U) {
            const u8* k = &rest[h + 0x48];
            c.key.modulus.assign(k, k + 0x100);
            c.key.exponent = be32(k + 0x100);
        }
        c.sig = std::move(*sig);
        c.bytes.assign(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(size));
        out.push_back(std::move(c));
        at += size;
    }
    return out;
}

const char* signature_status_name(SignatureStatus s) noexcept {
    switch (s) {
    case SignatureStatus::Valid: return "valid";
    case SignatureStatus::Invalid: return "INVALID";
    case SignatureStatus::Fakesigned: return "fakesigned";
    case SignatureStatus::MissingSigner: return "signer certificate missing";
    case SignatureStatus::NoRootKey: return "not checked (no root key supplied)";
    case SignatureStatus::Unsupported: return "unsupported signature type";
    }
    return "?";
}

bool ChainReport::disc_chain_valid() const noexcept {
    return ticket == SignatureStatus::Valid && tmd == SignatureStatus::Valid && ticket_signer == SignatureStatus::Valid &&
           tmd_signer == SignatureStatus::Valid;
}

namespace {

SignatureStatus check(const SignedBlob& sig, std::span<const u8> blob, const crypto::RsaPublicKey* key) {
    if (std::all_of(sig.signature.begin(), sig.signature.end(), [](u8 b) { return b == 0U; })) return SignatureStatus::Fakesigned;
    if (sig.type == kSigEcc) return SignatureStatus::Unsupported;
    if (key == nullptr || key->modulus.empty()) return SignatureStatus::MissingSigner;
    const auto digest = crypto::sha1(blob.subspan(sig.issuer_offset));
    return crypto::rsa_verify_sha1(*key, sig.signature, digest) ? SignatureStatus::Valid : SignatureStatus::Invalid;
}

// "Root-CA00000001-XS00000003" -> signer "XS00000003", signer's issuer "Root-CA00000001"
const Certificate* find_signer(const std::string& issuer, const std::vector<Certificate>& certs) {
    const auto dash = issuer.rfind('-');
    if (dash == std::string::npos) return nullptr;
    const std::string name = issuer.substr(dash + 1), parent = issuer.substr(0, dash);
    for (const Certificate& c : certs)
        if (c.name == name && c.sig.issuer == parent) return &c;
    return nullptr;
}

SignatureStatus check_cert(const Certificate& c, const std::vector<Certificate>& certs, const crypto::RsaPublicKey* root) {
    if (c.sig.issuer == "Root") return root != nullptr ? check(c.sig, c.bytes, root) : SignatureStatus::NoRootKey;
    const Certificate* signer = find_signer(c.sig.issuer, certs);
    return check(c.sig, c.bytes, signer != nullptr ? &signer->key : nullptr);
}

} // namespace

ChainReport verify_chain(const Ticket& ticket, const Tmd& tmd, const std::vector<Certificate>& certs,
                         const crypto::RsaPublicKey* root_key) {
    ChainReport r;
    const Certificate* xs = find_signer(ticket.sig.issuer, certs);
    const Certificate* cp = find_signer(tmd.sig.issuer, certs);
    r.ticket = check(ticket.sig, ticket.bytes, xs != nullptr ? &xs->key : nullptr);
    r.tmd = check(tmd.sig, tmd.bytes, cp != nullptr ? &cp->key : nullptr);
    if (xs != nullptr) r.ticket_signer = check_cert(*xs, certs, root_key);
    if (cp != nullptr) r.tmd_signer = check_cert(*cp, certs, root_key);
    const Certificate* ca = xs != nullptr ? find_signer(xs->sig.issuer, certs) : nullptr;
    if (ca == nullptr && cp != nullptr) ca = find_signer(cp->sig.issuer, certs);
    if (ca != nullptr) r.ca = check_cert(*ca, certs, root_key);
    r.detail = "ticket (" + ticket.sig.issuer + "): " + signature_status_name(r.ticket) + "; TMD (" + tmd.sig.issuer +
               "): " + signature_status_name(r.tmd) + "; XS certificate: " + signature_status_name(r.ticket_signer) +
               "; CP certificate: " + signature_status_name(r.tmd_signer) + "; CA certificate: " + signature_status_name(r.ca);
    return r;
}

std::optional<crypto::RsaPublicKey> parse_root_key(std::span<const u8> data, std::string& error) {
    crypto::RsaPublicKey k;
    if (data.size() == 0x200U || data.size() == 0x204U) {
        k.modulus.assign(data.begin(), data.begin() + 0x200);
        if (data.size() == 0x204U) k.exponent = be32(&data[0x200]);
    } else {
        auto certs = parse_cert_chain(data, error);
        if (!certs) { error = "root key: neither a 512-byte modulus nor a certificate (" + error + ")"; return std::nullopt; }
        if (certs->size() != 1U || (*certs)[0].key.modulus.empty()) { error = "root key: expected one certificate with an RSA key"; return std::nullopt; }
        k = (*certs)[0].key;
    }
    if ((k.modulus.back() & 1U) == 0U || k.exponent < 3U) { error = "root key: not a valid RSA public key"; return std::nullopt; }
    return k;
}

} // namespace orca::disc
