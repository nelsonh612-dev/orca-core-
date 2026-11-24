// Orca Core — OrcaIOSC: the object store of IOS's crypto engine (IOSC): default
// entries, handles, object types and sizes, owner masks, dynamic objects, key
// import and the IOSC return codes (WiiBrew "IOS/Syscalls"; Dolphin
// Core/IOS/IOSC.{h,cpp} read as information - the code here is Orca's own), and the
// title-key derivation of tickets (Dolphin TicketReader::GetTitleKey).
//
// How Dolphin initialises its keys (studied in IOSC.cpp): the constructor calls
// LoadDefaultEntries(), which fills the 12 default entries; the common key, the
// Korean ("new") common key, the SD key and the root public key are byte arrays
// compiled into the program (retail and RVT sets), so nothing is asked of the
// user. LoadEntries() then reads the console's own keys (NG key, NAND key/HMAC,
// PRNG key, IDs) from keys.bin if present - never the common keys.
//
// Orca has the same structure, with the source of the values separated from the
// mechanism:
//   1. the mechanism (this class): identical handles, entries, sizes, owners,
//      CreateObject/DeleteObject, ImportSecretKey (decrypting a key with another
//      key, as ES does with title keys), ImportPublicKey, Get/SetOwnership,
//      Decrypt/Encrypt;
//   2. the selection: kCommonKeyHandles[ticket index], console profile by the
//      ticket's issuer;
//   3. the values: load_default_entries() takes them from the build-time key
//      bundle (key_bundle.hpp: whoever builds Orca and is entitled to the values
//      passes a file with -DORCA_DEFAULT_KEYS_FILE; they are compiled into the
//      binary like Dolphin's, and no emulator code changes); load_entries() then
//      applies the session's values (the user's own console, imported once). The
//      repository contains no values: a build without the bundle has the entries
//      but not their values, and says so in its errors.
#pragma once
#include "orca/core/bits.hpp"
#include "orca/disc/wii_disc.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orca::ios {

class Iosc {
public:
    using Handle = u32;
    enum class ConsoleType { Retail, RVT };

    // The default handles of the real IOSC.
    enum DefaultHandle : Handle {
        HANDLE_CONSOLE_KEY = 0,    // NG private key (ECC-233, per console)
        HANDLE_CONSOLE_ID = 1,
        HANDLE_FS_KEY = 2,         // NAND FS AES key
        HANDLE_FS_MAC = 3,         // NAND FS HMAC key
        HANDLE_COMMON_KEY = 4,
        HANDLE_PRNG_KEY = 5,
        HANDLE_SD_KEY = 6,
        HANDLE_BOOT2_VERSION = 7,
        HANDLE_UNKNOWN_8 = 8,
        HANDLE_UNKNOWN_9 = 9,
        HANDLE_FS_VERSION = 10,
        HANDLE_NEW_COMMON_KEY = 11, // the "new" (Korean) common key
        HANDLE_ROOT_KEY = 0xFFFFFFF,
    };
    // Orca extension (not an IOSC handle): the vWii common key for tickets with
    // index 2. Dolphin has no slot for it and uses the common key instead.
    static constexpr Handle kOrcaVwiiCommonKey = 0x100;
    static constexpr std::array<Handle, 2> kCommonKeyHandles{HANDLE_COMMON_KEY, HANDLE_NEW_COMMON_KEY};
    static constexpr std::size_t kEntries = 32; // the object table, default entries included

    enum ObjectType : u8 { TYPE_SECRET_KEY = 0, TYPE_PUBLIC_KEY = 1, TYPE_DATA = 3 };
    enum class ObjectSubType : u8 { AES128 = 0, MAC = 1, RSA2048 = 2, RSA4096 = 3, ECC233 = 4, Data = 5, Version = 6 };
    // Value size of an object type (16 AES, 20 MAC, 30 ECC private, 256/512 RSA, 60 ECC public, 0 data).
    static std::size_t size_for_type(ObjectType type, ObjectSubType subtype) noexcept;

    // IOS process IDs used for ownership (owner mask bit = 1 << pid)
    static constexpr u32 PID_KERNEL = 0, PID_ES = 1, PID_FS = 2, PID_DI = 3;
    // IOSC return codes
    static constexpr s32 IPC_SUCCESS = 0, IOSC_EACCES = -2000, IOSC_EINVAL = -2002, IOSC_INVALID_OBJTYPE = -2005,
                         IOSC_FAIL_INTERNAL = -2012, IOSC_FAIL_ALLOC = -2013;

    struct KeyEntry {
        bool in_use{false};
        ObjectType type{TYPE_DATA};
        ObjectSubType subtype{ObjectSubType::Data};
        std::vector<u8> data;
        u32 misc_data{0};
        u32 owner_mask{0};
        bool provided{false};   // the value is present (false: no source supplied it)
        const char* name{""};
        const char* source{""}; // "fixed default", "build-time default values", "session (user)"
    };

    // Default entries (values from the build-time bundle), then the session's values.
    Iosc(ConsoleType type, const disc::WiiKeys& provision);
    // Same, with an explicit default-value bundle instead of the build-time one (tests, tools).
    Iosc(ConsoleType type, const disc::WiiKeys& provision, std::span<const u8> default_bundle);
    ~Iosc();
    Iosc(const Iosc&) = delete;
    Iosc& operator=(const Iosc&) = delete;

    ConsoleType console_type() const noexcept { return type_; }
    const KeyEntry* entry(Handle h) const noexcept;
    // Problems with the default-value bundle (invalid file, wrong sizes); empty if none.
    const std::string& bundle_error() const noexcept { return bundle_error_; }

    // Object management (IOSC_CreateObject, _DeleteObject, _GetOwnership, _SetOwnership).
    s32 create_object(Handle& handle, ObjectType type, ObjectSubType subtype, u32 pid);
    s32 delete_object(Handle handle, u32 pid);
    s32 get_ownership(Handle handle, u32& owner) const;
    s32 set_ownership(Handle handle, u32 new_owner, u32 pid);
    bool has_ownership(Handle h, u32 pid) const noexcept;
    static bool is_default_handle(Handle h) noexcept;
    // IOSC_ImportSecretKey: a plain AES key, or one decrypted with `decrypt_key` and `iv`.
    s32 import_secret_key(Handle dest, std::span<const u8, 16> key, u32 pid);
    s32 import_secret_key(Handle dest, Handle decrypt_key, std::array<u8, 16> iv, std::span<const u8, 16> encrypted, u32 pid, std::string& error);
    // IOSC_ImportPublicKey (RSA keys carry their exponent in misc_data).
    s32 import_public_key(Handle dest, std::span<const u8> key, u32 exponent, u32 pid);

    // AES-128-CBC with a secret key entry; return an IOSC code.
    s32 decrypt(Handle key, std::array<u8, 16>& iv, std::span<const u8> in, std::span<u8> out, u32 pid, std::string& error) const;
    s32 encrypt(Handle key, std::array<u8, 16>& iv, std::span<const u8> in, std::span<u8> out, u32 pid, std::string& error) const;

    // The key a ticket's common key index selects (TicketReader::GetTitleKey).
    std::optional<Handle> common_key_handle(u8 index, std::string& error) const;
    // Title key = Decrypt(common key of the ticket's index, IV = title ID || 0^8, encrypted key),
    // done as ES does: into a new secret-key object owned by ES (ImportSecretKey), whose value
    // is handed to the disc reader and which is then deleted (and overwritten).
    std::optional<disc::AesKey> title_key(u64 title_id, std::span<const u8, 16> encrypted, u8 index, std::string& error);

    // Tickets from Root-CA00000002-XS00000006 come from development (RVT) consoles.
    static ConsoleType console_type_of(const std::string& ticket_issuer);

private:
    void load_default_entries(std::span<const u8> bundle);
    void load_entries(const disc::WiiKeys& provision);
    KeyEntry* entry_mut(Handle h) noexcept;
    s32 crypt(bool decrypt, Handle key, std::array<u8, 16>& iv, std::span<const u8> in, std::span<u8> out, u32 pid, std::string& error) const;
    ConsoleType type_;
    std::array<KeyEntry, kEntries> entries_{};
    KeyEntry root_;
    KeyEntry vwii_;
    std::string bundle_error_;
};

// The default values compiled into this build (key_bundle.hpp), parsed: which
// keys they hold ("common, Korean" / "none"), or why the bundle is invalid.
std::string default_values_status();

// The keys the user keeps for Orca, found without asking: the first file found
// for each slot wins, in this order:
//   1. the file named by the environment variable ORCA_WII_KEYS;
//   2. in each of `dirs`, then $ORCA_KEYS_DIR, $XDG_CONFIG_HOME/orca and
//      $HOME/.config/orca: wii_keys.txt (text form), common-key.bin,
//      korean-common-key.bin, vwii-common-key.bin, dev-common-key.bin (16 bytes
//      each), keys.bin (BootMii dump: its common key only).
// Slots already filled in `keys` are kept. `report` lists what was loaded from
// where. False only when a file exists but is invalid (`error` says which).
bool load_default_key_provision(const std::vector<std::string>& dirs, disc::WiiKeys& keys, std::string& report, std::string& error);
// The places load_default_key_provision looks in, for messages.
std::string key_provision_places(const std::vector<std::string>& dirs);

// Provisioning once: imports key material the user extracted from their own
// console (any form disc::parse_wii_keys accepts: a 16-byte key stored at `index`,
// a BootMii keys.bin - its common key only -, or a text key file) into
// `dir`/wii_keys.txt, merged with the keys already there (new values replace old
// ones), so that load_default_key_provision finds them by itself from then on.
// The file is written with owner-only permissions. `summary` names the keys the
// store now holds.
bool import_key_provision(const std::string& dir, std::span<const u8> data, unsigned index, std::string& summary, std::string& error);
// Names of the keys in `keys` ("common, Korean, development, root"), or "none".
std::string describe_keys(const disc::WiiKeys& keys);

} // namespace orca::ios
