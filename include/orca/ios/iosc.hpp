// Orca Core — OrcaIOSC: the key store of IOS's crypto engine (IOSC), with the
// default entries, handles, object types, owner masks and return codes that IOS
// uses (WiiBrew "IOS/Syscalls"; Dolphin Core/IOS/IOSC.h/.cpp read as
// information), and the title-key derivation of tickets (Dolphin
// TicketReader::GetTitleKey).
//
// Three separate parts:
//   1. this implementation (entries, ownership, Decrypt);
//   2. the handles and the choice of key: a ticket's common key index selects
//      kCommonKeyHandles[index] (4 = common key, 11 = new/Korean common key);
//      tickets issued by Root-CA00000002-XS00000006 belong to development (RVT)
//      consoles, whose entries hold the development values;
//   3. the provision of the values. Dolphin's LoadDefaultEntries() fills the
//      common keys, the SD key and the root public key with Nintendo's values,
//      written in its source. Orca does not ship them: the entries are created
//      exactly the same way, empty and marked "not provided", and their values come
//      from the user's own console through disc::WiiKeys, which
//      load_default_key_provision() fills automatically from fixed places when the
//      emulator starts (see docs/DISCOS.md). An operation on an entry without its
//      value fails with IOSC_FAIL_INTERNAL and a message naming the key.
// Dolphin's LoadEntries() takes only console-specific values from a BootMii
// keys.bin (NG key, NAND key/HMAC, backup key, IDs), never its common key; Orca
// does not emulate the operations that need those and does not load them.
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

    // The default handles of the real IOSC (the PPC can pass them to ioctlvs).
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

    enum ObjectType : u8 { TYPE_SECRET_KEY = 0, TYPE_PUBLIC_KEY = 1, TYPE_DATA = 3 };
    enum class ObjectSubType : u8 { AES128 = 0, MAC = 1, RSA2048 = 2, RSA4096 = 3, ECC233 = 4, Data = 5, Version = 6 };

    // IOS process IDs used for ownership (owner mask bit = 1 << pid)
    static constexpr u32 PID_KERNEL = 0, PID_ES = 1, PID_FS = 2, PID_DI = 3;
    // IOSC return codes
    static constexpr s32 IPC_SUCCESS = 0, IOSC_EACCES = -2000, IOSC_EINVAL = -2002, IOSC_INVALID_OBJTYPE = -2005,
                         IOSC_FAIL_INTERNAL = -2012;

    struct KeyEntry {
        bool in_use{false};
        ObjectType type{TYPE_DATA};
        ObjectSubType subtype{ObjectSubType::Data};
        std::vector<u8> data;
        u32 misc_data{0};
        u32 owner_mask{0};
        bool provided{false};   // the value is present (false: Orca cannot ship it and it was not supplied)
        const char* name{""};
    };

    Iosc(ConsoleType type, const disc::WiiKeys& provision);
    ~Iosc();
    Iosc(const Iosc&) = delete;
    Iosc& operator=(const Iosc&) = delete;

    ConsoleType console_type() const noexcept { return type_; }
    const KeyEntry* entry(Handle h) const noexcept;
    bool has_ownership(Handle h, u32 pid) const noexcept;
    // AES-128-CBC decryption with a secret key entry; returns an IOSC code.
    s32 decrypt(Handle key, std::array<u8, 16>& iv, std::span<const u8> in, std::span<u8> out, u32 pid, std::string& error) const;

    // The key a ticket's common key index selects (TicketReader::GetTitleKey).
    std::optional<Handle> common_key_handle(u8 index, std::string& error) const;
    // Title key = Decrypt(common key of the ticket's index, IV = title ID || 0^8, encrypted key), as ES.
    std::optional<disc::AesKey> title_key(u64 title_id, std::span<const u8, 16> encrypted, u8 index, std::string& error) const;

    // Tickets from Root-CA00000002-XS00000006 come from development (RVT) consoles.
    static ConsoleType console_type_of(const std::string& ticket_issuer);

private:
    void load_default_entries(const disc::WiiKeys& provision);
    ConsoleType type_;
    std::array<KeyEntry, 32> entries_{};
    KeyEntry root_;
    KeyEntry vwii_;
};

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
