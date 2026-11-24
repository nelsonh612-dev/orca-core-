// Orca Core — OrcaIOSC. See iosc.hpp.
#include "orca/ios/iosc.hpp"

#include "orca/crypto/aes.hpp"
#include "orca/crypto/secure.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace orca::ios {

namespace {
std::vector<u8> key_bytes(const std::optional<disc::AesKey>& k) {
    return k ? std::vector<u8>(k->begin(), k->end()) : std::vector<u8>(16, 0);
}
} // namespace

Iosc::Iosc(ConsoleType type, const disc::WiiKeys& provision) : type_(type) { load_default_entries(provision); }

Iosc::~Iosc() {
    for (KeyEntry& e : entries_) crypto::secure_zero(e.data.data(), e.data.size());
    crypto::secure_zero(vwii_.data.data(), vwii_.data.size());
}

// The entries of IOSC::LoadDefaultEntries, in the same order and with the same
// types and owner masks. Values Orca cannot ship come from the provision.
void Iosc::load_default_entries(const disc::WiiKeys& p) {
    const auto set = [this](Handle h, ObjectType t, ObjectSubType st, std::vector<u8> data, u32 misc, u32 owner, bool provided, const char* name) {
        entries_[h] = KeyEntry{true, t, st, std::move(data), misc, owner, provided, name};
    };
    // Console-specific values (NG key and signature, console ID): not emulated.
    set(HANDLE_CONSOLE_KEY, TYPE_SECRET_KEY, ObjectSubType::ECC233, {}, 0, 3, false, "console (NG) private key");
    set(HANDLE_CONSOLE_ID, TYPE_DATA, ObjectSubType::Data, {}, 0, 0xFFFFFFF, false, "console ID");
    // NAND keys: zero, as Dolphin's defaults (Orca's NAND is a host directory, not encrypted)
    set(HANDLE_FS_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, std::vector<u8>(16, 0), 0, 5, true, "NAND FS key");
    set(HANDLE_FS_MAC, TYPE_SECRET_KEY, ObjectSubType::MAC, std::vector<u8>(20, 0), 0, 5, true, "NAND FS HMAC key");
    if (type_ == ConsoleType::Retail) {
        set(HANDLE_COMMON_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, key_bytes(p.common[0]), 0, 3, p.common[0].has_value(), "common key (index 0)");
    } else {
        set(HANDLE_COMMON_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, key_bytes(p.dev_common), 0, 3, p.dev_common.has_value(), "development common key");
    }
    root_ = KeyEntry{true, TYPE_PUBLIC_KEY, ObjectSubType::RSA4096, p.root ? p.root->modulus : std::vector<u8>{},
                     p.root ? p.root->exponent : 0x00010001U, 0, p.root.has_value(), "root public key"};
    set(HANDLE_PRNG_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, std::vector<u8>(16, 0), 0, 3, true, "PRNG key");
    set(HANDLE_SD_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, std::vector<u8>(16, 0), 0, 3, false, "SD key");
    set(HANDLE_BOOT2_VERSION, TYPE_DATA, ObjectSubType::Version, {}, 0, 3, true, "boot2 version");
    set(HANDLE_UNKNOWN_8, TYPE_DATA, ObjectSubType::Version, {}, 0, 3, true, "unknown 8");
    set(HANDLE_UNKNOWN_9, TYPE_DATA, ObjectSubType::Version, {}, 0, 3, true, "unknown 9");
    set(HANDLE_FS_VERSION, TYPE_DATA, ObjectSubType::Version, {}, 0, 3, true, "FS version");
    set(HANDLE_NEW_COMMON_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, key_bytes(p.common[1]), 0, 3, p.common[1].has_value(), "Korean common key (index 1)");
    vwii_ = KeyEntry{true, TYPE_SECRET_KEY, ObjectSubType::AES128, key_bytes(p.common[2]), 0, 3, p.common[2].has_value(), "vWii common key (index 2)"};
}

const Iosc::KeyEntry* Iosc::entry(Handle h) const noexcept {
    if (h == HANDLE_ROOT_KEY) return &root_;
    if (h == kOrcaVwiiCommonKey) return &vwii_;
    return h < entries_.size() && entries_[h].in_use ? &entries_[h] : nullptr;
}

bool Iosc::has_ownership(Handle h, u32 pid) const noexcept {
    if (h == HANDLE_ROOT_KEY) return true;
    const KeyEntry* e = entry(h);
    return e != nullptr && pid < 32U && ((1U << pid) & e->owner_mask) != 0U;
}

s32 Iosc::decrypt(Handle key, std::array<u8, 16>& iv, std::span<const u8> in, std::span<u8> out, u32 pid, std::string& error) const {
    if (!has_ownership(key, pid)) { error = "IOSC: process " + std::to_string(pid) + " does not own key handle " + std::to_string(key); return IOSC_EACCES; }
    const KeyEntry* e = entry(key);
    if (e == nullptr) { error = "IOSC: no key with handle " + std::to_string(key); return IOSC_EINVAL; }
    if (e->type != TYPE_SECRET_KEY || e->subtype != ObjectSubType::AES128) { error = std::string("IOSC: ") + e->name + " is not an AES-128 key"; return IOSC_INVALID_OBJTYPE; }
    if (!e->provided || e->data.size() != 16U) {
        error = std::string("the ") + e->name + " (IOSC handle " + std::to_string(key) + ") is not provided";
        return IOSC_FAIL_INTERNAL;
    }
    if (in.size() != out.size() || in.size() % 16U != 0U) { error = "IOSC: decryption size is not a multiple of 16"; return IOSC_EINVAL; }
    crypto::Aes128 aes(std::span<const u8, 16>(e->data.data(), 16));
    std::copy(in.begin(), in.end(), out.begin());
    (void)aes.cbc_decrypt(out, iv);
    return IPC_SUCCESS;
}

std::optional<Iosc::Handle> Iosc::common_key_handle(u8 index, std::string& error) const {
    if (index < kCommonKeyHandles.size()) return kCommonKeyHandles[index];
    if (index == 2U) return kOrcaVwiiCommonKey;
    error = "unsupported common key index " + std::to_string(index) + " (0 = normal, 1 = Korean, 2 = vWii)";
    return std::nullopt;
}

std::optional<disc::AesKey> Iosc::title_key(u64 title_id, std::span<const u8, 16> encrypted, u8 index, std::string& error) const {
    const auto handle = common_key_handle(index, error);
    if (!handle) return std::nullopt;
    std::array<u8, 16> iv{};
    for (unsigned k = 0; k < 8; ++k) iv[k] = static_cast<u8>(title_id >> (56U - 8U * k));
    disc::AesKey key{};
    if (decrypt(*handle, iv, encrypted, key, PID_ES, error) != IPC_SUCCESS) return std::nullopt;
    return key;
}

Iosc::ConsoleType Iosc::console_type_of(const std::string& issuer) {
    return issuer == "Root-CA00000002-XS00000006" ? ConsoleType::RVT : ConsoleType::Retail;
}

// ---------------------------------------------------------------- provision
namespace {
std::vector<std::string> search_dirs(const std::vector<std::string>& dirs) {
    std::vector<std::string> out = dirs;
    if (const char* d = std::getenv("ORCA_KEYS_DIR"); d != nullptr && *d != '\0') out.emplace_back(d);
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x != nullptr && *x != '\0') out.push_back(std::string(x) + "/orca");
    if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') out.push_back(std::string(h) + "/.config/orca");
    return out;
}
} // namespace

std::string key_provision_places(const std::vector<std::string>& dirs) {
    std::string s = "$ORCA_WII_KEYS";
    for (const std::string& d : search_dirs(dirs)) s += ", " + d + "/(wii_keys.txt|common-key.bin|keys.bin)";
    return s;
}

bool load_default_key_provision(const std::vector<std::string>& dirs, disc::WiiKeys& keys, std::string& report, std::string& error) {
    namespace fs = std::filesystem;
    // load into a scratch store, then fill only the empty slots of `keys`
    const auto merge = [&](const std::string& path, unsigned index) -> bool {
        disc::WiiKeys found;
        std::string e;
        if (!disc::load_wii_key_file(path, found, index, e)) { error = e; return false; }
        std::string got;
        for (unsigned i = 0; i < 3; ++i)
            if (found.common[i] && !keys.common[i]) { keys.common[i] = found.common[i]; got += i == 0 ? " common" : i == 1 ? " korean" : " vwii"; }
        if (found.dev_common && !keys.dev_common) { keys.dev_common = found.dev_common; got += " dev"; }
        if (found.root && !keys.root) { keys.root = found.root; got += " root"; }
        if (!got.empty()) report += path + ":" + got + "\n";
        return true;
    };
    if (const char* f = std::getenv("ORCA_WII_KEYS"); f != nullptr && *f != '\0')
        if (!merge(f, 0)) return false;
    static const struct { const char* name; unsigned index; } kFiles[] = {
        {"wii_keys.txt", 0}, {"common-key.bin", 0}, {"korean-common-key.bin", 1}, {"vwii-common-key.bin", 2}, {"dev-common-key.bin", 3}, {"keys.bin", 0}};
    for (const std::string& d : search_dirs(dirs)) {
        for (const auto& f : kFiles) {
            const std::string path = d + "/" + f.name;
            std::error_code ec;
            if (!fs::is_regular_file(path, ec)) continue;
            if (!merge(path, f.index)) return false;
        }
    }
    return true;
}

std::string describe_keys(const disc::WiiKeys& k) {
    std::string s;
    const auto add = [&](bool have, const char* name) { if (have) s += (s.empty() ? "" : ", ") + std::string(name); };
    add(k.common[0].has_value(), "common");
    add(k.common[1].has_value(), "Korean");
    add(k.common[2].has_value(), "vWii");
    add(k.dev_common.has_value(), "development");
    add(k.root.has_value(), "root");
    return s.empty() ? "none" : s;
}

bool import_key_provision(const std::string& dir, std::span<const u8> data, unsigned index, std::string& summary, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) { error = "cannot create the key directory " + dir; return false; }
    disc::WiiKeys incoming;
    if (!disc::parse_wii_keys(data, incoming, index, error)) return false;
    const std::string file = dir + "/wii_keys.txt";
    disc::WiiKeys keys;
    if (fs::is_regular_file(file, ec) && !disc::load_wii_key_file(file, keys, 0, error)) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (incoming.common[i]) keys.common[i] = incoming.common[i];
    if (incoming.dev_common) keys.dev_common = incoming.dev_common;
    std::string root_path;
    if (incoming.root) {
        root_path = dir + "/root_key.bin";
        std::vector<u8> r = incoming.root->modulus;
        for (int k = 3; k >= 0; --k) r.push_back(static_cast<u8>(incoming.root->exponent >> (8 * k)));
        std::ofstream o(root_path, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(r.data()), static_cast<std::streamsize>(r.size()));
        if (!o) { error = "cannot write " + root_path; return false; }
        keys.root = incoming.root;
    } else if (keys.root && fs::is_regular_file(dir + "/root_key.bin", ec)) {
        root_path = dir + "/root_key.bin";
    }
    std::string text = "# Orca Core key store: keys from the user's own console (see docs/DISCOS.md)\n";
    const auto line = [&](const char* name, const std::optional<disc::AesKey>& k) {
        if (!k) return;
        text += std::string(name) + " = ";
        char b[3];
        for (const u8 x : *k) { std::snprintf(b, sizeof b, "%02x", x); text += b; }
        text += "\n";
    };
    line("common_key", keys.common[0]);
    line("korean_common_key", keys.common[1]);
    line("vwii_common_key", keys.common[2]);
    line("dev_common_key", keys.dev_common);
    if (!root_path.empty()) text += "root_key_file = " + root_path + "\n";
    const std::string tmp = file + ".new";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!o) { error = "cannot write " + tmp; return false; }
    }
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    fs::rename(tmp, file, ec);
    crypto::secure_zero(text.data(), text.size());
    if (ec) { error = "cannot replace " + file + ": " + ec.message(); return false; }
    summary = describe_keys(keys);
    return true;
}

} // namespace orca::ios
