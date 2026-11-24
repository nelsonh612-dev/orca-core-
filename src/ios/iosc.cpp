// Orca Core — OrcaIOSC. See iosc.hpp.
#include "orca/ios/iosc.hpp"

#include "orca/crypto/aes.hpp"
#include "orca/crypto/secure.hpp"
#include "orca/ios/key_bundle.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace orca::ios {

namespace {
std::vector<u8> key_bytes(const std::optional<disc::AesKey>& k) {
    return k ? std::vector<u8>(k->begin(), k->end()) : std::vector<u8>();
}
constexpr const char* kFixed = "fixed default";
constexpr const char* kBuilt = "build-time default values";
constexpr const char* kSession = "session (user)";
} // namespace

std::size_t Iosc::size_for_type(ObjectType type, ObjectSubType st) noexcept {
    if (type == TYPE_SECRET_KEY) return st == ObjectSubType::AES128 ? 16U : st == ObjectSubType::MAC ? 20U : st == ObjectSubType::ECC233 ? 30U : 0U;
    if (type == TYPE_PUBLIC_KEY) return st == ObjectSubType::RSA2048 ? 256U : st == ObjectSubType::RSA4096 ? 512U : st == ObjectSubType::ECC233 ? 60U : 0U;
    return 0U;
}

Iosc::Iosc(ConsoleType type, const disc::WiiKeys& provision) : Iosc(type, provision, build_time_key_bundle()) {}

Iosc::Iosc(ConsoleType type, const disc::WiiKeys& provision, std::span<const u8> bundle) : type_(type) {
    load_default_entries(bundle);
    load_entries(provision);
}

Iosc::~Iosc() {
    for (KeyEntry& e : entries_) crypto::secure_zero(e.data.data(), e.data.size());
    crypto::secure_zero(vwii_.data.data(), vwii_.data.size());
}

// The entries of IOS's default table, in the same order and with the same types
// and owner masks. Values that are fixed (zero NAND keys: Orca's NAND is a host
// directory) are set here; the secret ones come from the build-time bundle.
void Iosc::load_default_entries(std::span<const u8> bundle) {
    disc::WiiKeys d; // the bundle's values
    if (!bundle.empty() && !disc::parse_wii_keys(bundle, d, 0, bundle_error_)) {
        bundle_error_ = "the build-time key bundle is invalid: " + bundle_error_;
        d = disc::WiiKeys{};
    }
    const auto set = [this](Handle h, ObjectType t, ObjectSubType st, std::vector<u8> data, u32 misc, u32 owner, bool provided,
                            const char* name, const char* source) {
        entries_[h] = KeyEntry{true, t, st, std::move(data), misc, owner, provided, name, provided ? source : ""};
    };
    set(HANDLE_CONSOLE_KEY, TYPE_SECRET_KEY, ObjectSubType::ECC233, {}, 0, 3, false, "console (NG) private key", "");
    set(HANDLE_CONSOLE_ID, TYPE_DATA, ObjectSubType::Data, {}, 0, 0xFFFFFFF, false, "console ID", "");
    set(HANDLE_FS_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, std::vector<u8>(16, 0), 0, 5, true, "NAND FS key", kFixed);
    set(HANDLE_FS_MAC, TYPE_SECRET_KEY, ObjectSubType::MAC, std::vector<u8>(20, 0), 0, 5, true, "NAND FS HMAC key", kFixed);
    if (type_ == ConsoleType::Retail)
        set(HANDLE_COMMON_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, key_bytes(d.common[0]), 0, 3, d.common[0].has_value(), "common key (index 0)", kBuilt);
    else
        set(HANDLE_COMMON_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, key_bytes(d.dev_common), 0, 3, d.dev_common.has_value(), "development common key", kBuilt);
    root_ = KeyEntry{true, TYPE_PUBLIC_KEY, ObjectSubType::RSA4096, d.root ? d.root->modulus : std::vector<u8>{},
                     d.root ? d.root->exponent : 0x00010001U, 0, d.root.has_value(), "root public key", d.root ? kBuilt : ""};
    set(HANDLE_PRNG_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, std::vector<u8>(16, 0), 0, 3, true, "PRNG key", kFixed);
    set(HANDLE_SD_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, {}, 0, 3, false, "SD key", "");
    set(HANDLE_BOOT2_VERSION, TYPE_DATA, ObjectSubType::Version, {}, 0, 3, true, "boot2 version", kFixed);
    set(HANDLE_UNKNOWN_8, TYPE_DATA, ObjectSubType::Version, {}, 0, 3, true, "unknown 8", kFixed);
    set(HANDLE_UNKNOWN_9, TYPE_DATA, ObjectSubType::Version, {}, 0, 3, true, "unknown 9", kFixed);
    set(HANDLE_FS_VERSION, TYPE_DATA, ObjectSubType::Version, {}, 0, 3, true, "FS version", kFixed);
    const bool korean = type_ == ConsoleType::Retail && d.common[1].has_value();
    set(HANDLE_NEW_COMMON_KEY, TYPE_SECRET_KEY, ObjectSubType::AES128, korean ? key_bytes(d.common[1]) : std::vector<u8>{}, 0, 3, korean,
        "Korean common key (index 1)", kBuilt);
    const bool vwii = type_ == ConsoleType::Retail && d.common[2].has_value();
    vwii_ = KeyEntry{true, TYPE_SECRET_KEY, ObjectSubType::AES128, vwii ? key_bytes(d.common[2]) : std::vector<u8>{}, 0, 3, vwii,
                     "vWii common key (index 2)", vwii ? kBuilt : ""};
}

// The session's values (the user's own console) replace the defaults.
void Iosc::load_entries(const disc::WiiKeys& p) {
    const auto put = [](KeyEntry& e, const std::optional<disc::AesKey>& k) {
        if (!k) return;
        crypto::secure_zero(e.data.data(), e.data.size());
        e.data.assign(k->begin(), k->end());
        e.provided = true;
        e.source = kSession;
    };
    if (type_ == ConsoleType::Retail) {
        put(entries_[HANDLE_COMMON_KEY], p.common[0]);
        put(entries_[HANDLE_NEW_COMMON_KEY], p.common[1]);
        put(vwii_, p.common[2]);
    } else {
        put(entries_[HANDLE_COMMON_KEY], p.dev_common);
    }
    if (p.root) {
        root_.data = p.root->modulus;
        root_.misc_data = p.root->exponent;
        root_.provided = true;
        root_.source = kSession;
    }
}

const Iosc::KeyEntry* Iosc::entry(Handle h) const noexcept {
    if (h == HANDLE_ROOT_KEY) return &root_;
    if (h == kOrcaVwiiCommonKey) return &vwii_;
    return h < entries_.size() && entries_[h].in_use ? &entries_[h] : nullptr;
}

Iosc::KeyEntry* Iosc::entry_mut(Handle h) noexcept { return const_cast<KeyEntry*>(static_cast<const Iosc*>(this)->entry(h)); }

bool Iosc::is_default_handle(Handle h) noexcept { return h <= HANDLE_NEW_COMMON_KEY || h == HANDLE_ROOT_KEY || h == kOrcaVwiiCommonKey; }

s32 Iosc::get_ownership(Handle h, u32& owner) const {
    const KeyEntry* e = entry(h);
    if (e == nullptr) return IOSC_EINVAL;
    owner = e->owner_mask;
    return IPC_SUCCESS;
}

bool Iosc::has_ownership(Handle h, u32 pid) const noexcept {
    if (h == HANDLE_ROOT_KEY) return true;
    u32 owner = 0;
    return pid < 32U && get_ownership(h, owner) == IPC_SUCCESS && ((1U << pid) & owner) != 0U;
}

s32 Iosc::set_ownership(Handle h, u32 new_owner, u32 pid) {
    if (!has_ownership(h, pid)) return IOSC_EACCES;
    KeyEntry* e = entry_mut(h);
    if (e == nullptr) return IOSC_EINVAL;
    // only the sole owner may hand the object on; the three low PIDs stay as they were
    const u32 me = 1U << pid;
    if ((e->owner_mask | me) != me) return IOSC_EACCES;
    e->owner_mask = (new_owner & ~7U) | e->owner_mask | me;
    return IPC_SUCCESS;
}

s32 Iosc::create_object(Handle& handle, ObjectType type, ObjectSubType subtype, u32 pid) {
    if (pid >= 32U) return IOSC_EINVAL;
    for (Handle h = HANDLE_NEW_COMMON_KEY + 1U; h < entries_.size(); ++h) {
        if (entries_[h].in_use) continue;
        entries_[h] = KeyEntry{true, type, subtype, {}, 0, 1U << pid, false, "object", ""};
        handle = h;
        return IPC_SUCCESS;
    }
    return IOSC_FAIL_ALLOC;
}

s32 Iosc::delete_object(Handle h, u32 pid) {
    if (is_default_handle(h) || !has_ownership(h, pid)) return IOSC_EACCES;
    KeyEntry* e = entry_mut(h);
    if (e == nullptr) return IOSC_EINVAL;
    crypto::secure_zero(e->data.data(), e->data.size());
    *e = KeyEntry{};
    return IPC_SUCCESS;
}

s32 Iosc::import_secret_key(Handle dest, std::span<const u8, 16> key, u32 pid) {
    if (is_default_handle(dest) || !has_ownership(dest, pid)) return IOSC_EACCES;
    KeyEntry* e = entry_mut(dest);
    if (e == nullptr) return IOSC_EINVAL;
    if (e->type != TYPE_SECRET_KEY || e->subtype != ObjectSubType::AES128) return IOSC_INVALID_OBJTYPE;
    crypto::secure_zero(e->data.data(), e->data.size());
    e->data.assign(key.begin(), key.end());
    e->provided = true;
    e->source = "imported";
    return IPC_SUCCESS;
}

s32 Iosc::import_secret_key(Handle dest, Handle decrypt_key, std::array<u8, 16> iv, std::span<const u8, 16> encrypted, u32 pid, std::string& error) {
    std::array<u8, 16> plain{};
    const s32 r = decrypt(decrypt_key, iv, encrypted, plain, pid, error);
    if (r != IPC_SUCCESS) return r;
    const s32 i = import_secret_key(dest, plain, pid);
    crypto::secure_zero(plain.data(), plain.size());
    return i;
}

s32 Iosc::import_public_key(Handle dest, std::span<const u8> key, u32 exponent, u32 pid) {
    if (is_default_handle(dest) || !has_ownership(dest, pid)) return IOSC_EACCES;
    KeyEntry* e = entry_mut(dest);
    if (e == nullptr) return IOSC_EINVAL;
    if (e->type != TYPE_PUBLIC_KEY) return IOSC_INVALID_OBJTYPE;
    const std::size_t size = size_for_type(e->type, e->subtype);
    if (size == 0U) return IOSC_INVALID_OBJTYPE;
    if (key.size() != size) return IOSC_EINVAL;
    e->data.assign(key.begin(), key.end());
    if (e->subtype == ObjectSubType::RSA2048 || e->subtype == ObjectSubType::RSA4096) e->misc_data = exponent;
    e->provided = true;
    e->source = "imported";
    return IPC_SUCCESS;
}

s32 Iosc::crypt(bool dec, Handle key, std::array<u8, 16>& iv, std::span<const u8> in, std::span<u8> out, u32 pid, std::string& error) const {
    if (!has_ownership(key, pid)) {
        error = "IOSC: process " + std::to_string(pid) + " does not own key handle " + std::to_string(key);
        return IOSC_EACCES; // checked first, as IOS (a handle that does not exist has no owner)
    }
    const KeyEntry* e = entry(key);
    if (e == nullptr) { error = "IOSC: no key with handle " + std::to_string(key); return IOSC_EINVAL; }
    if (e->type != TYPE_SECRET_KEY || e->subtype != ObjectSubType::AES128) { error = std::string("IOSC: ") + e->name + " is not an AES-128 key"; return IOSC_INVALID_OBJTYPE; }
    if (!e->provided || e->data.size() != 16U) {
        error = std::string("the ") + e->name + " (IOSC handle " + std::to_string(key) + ") is not provided";
        return IOSC_FAIL_INTERNAL;
    }
    if (in.size() != out.size() || in.size() % 16U != 0U) { error = "IOSC: size is not a multiple of 16"; return IOSC_EINVAL; }
    crypto::Aes128 aes(std::span<const u8, 16>(e->data.data(), 16));
    std::copy(in.begin(), in.end(), out.begin());
    if (dec) (void)aes.cbc_decrypt(out, iv);
    else (void)aes.cbc_encrypt(out, iv);
    return IPC_SUCCESS;
}

s32 Iosc::decrypt(Handle key, std::array<u8, 16>& iv, std::span<const u8> in, std::span<u8> out, u32 pid, std::string& error) const {
    return crypt(true, key, iv, in, out, pid, error);
}

s32 Iosc::encrypt(Handle key, std::array<u8, 16>& iv, std::span<const u8> in, std::span<u8> out, u32 pid, std::string& error) const {
    return crypt(false, key, iv, in, out, pid, error);
}

std::optional<Iosc::Handle> Iosc::common_key_handle(u8 index, std::string& error) const {
    if (index < kCommonKeyHandles.size()) return kCommonKeyHandles[index];
    if (index == 2U) return kOrcaVwiiCommonKey;
    error = "unsupported common key index " + std::to_string(index) + " (0 = normal, 1 = Korean, 2 = vWii)";
    return std::nullopt;
}

std::optional<disc::AesKey> Iosc::title_key(u64 title_id, std::span<const u8, 16> encrypted, u8 index, std::string& error) {
    const auto common = common_key_handle(index, error);
    if (!common) return std::nullopt;
    std::array<u8, 16> iv{};
    for (unsigned k = 0; k < 8; ++k) iv[k] = static_cast<u8>(title_id >> (56U - 8U * k));
    Handle object = 0;
    if (create_object(object, TYPE_SECRET_KEY, ObjectSubType::AES128, PID_ES) != IPC_SUCCESS) { error = "IOSC: no free object for the title key"; return std::nullopt; }
    std::optional<disc::AesKey> key;
    if (import_secret_key(object, *common, iv, encrypted, PID_ES, error) == IPC_SUCCESS) {
        disc::AesKey k{};
        std::copy_n(entries_[object].data.begin(), 16, k.begin());
        key = k;
        crypto::secure_zero(k.data(), k.size());
    }
    (void)delete_object(object, PID_ES);
    return key;
}

Iosc::ConsoleType Iosc::console_type_of(const std::string& issuer) {
    return issuer == "Root-CA00000002-XS00000006" ? ConsoleType::RVT : ConsoleType::Retail;
}

std::string default_values_status() {
    const auto b = build_time_key_bundle();
    if (b.empty()) return "none";
    disc::WiiKeys d;
    std::string error;
    if (!disc::parse_wii_keys(b, d, 0, error)) return "invalid (" + error + ")";
    return describe_keys(d);
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
