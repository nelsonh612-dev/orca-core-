// Orca Core — IOS /dev/es (title management), the part a disc game uses.
//
// The active title is the one whose ticket and TMD were handed to ES when its
// partition was opened (DI open-partition, or the HLE disc boot). Supported
// ioctlv requests (public Wii documentation, e.g. WiiBrew "/dev/es"):
//   0x12 ticket view count   0x13 ticket views     0x14 TMD view size
//   0x15 TMD view            0x16 consumption      0x19 DI TMD view size
//   0x1A DI TMD view         0x1B DI ticket view   0x1D title data directory
//   0x20 title ID
//   0x07 device ID (the console ID from the user's keys.bin; without it, stop)
//   0x0C/0x0D owned titles (tickets in /ticket)  0x0E/0x0F installed titles
//   (TMDs in /title/*/*/content/title.tmd)  0x34/0x35 stored TMD size / TMD
//   0x2E boot2 version (from keys.bin, else stop)  0x45 Korea check (-1017:
//   no Korean key, as IOS70 answers on a non-Korean console; 0 with one)
//   The NAND title store is read as IOS's ES does, as root.
// Views:
//   ticket view (0xD8 bytes) = ticket version byte, three zero bytes, then the
//     ticket's 0xD4 bytes from the ticket ID (0x1D0);
//   TMD view = TMD bytes 0x180-0x1D7 (version .. reserved), title version,
//     content count, then for each content its first 0x10 bytes (no hash).
// Titles other than the active one (the NAND title and ticket stores), and
// every other request (launch, device ID, installs, signing, ...), stop
// explicitly. Consumption answers zero entries (no time/usage limits), as
// other independent ES implementations do.
#pragma once
#include "orca/ios/kernel.hpp"
#include "orca/ios/nand_fs.hpp"

#include <optional>
#include <vector>

namespace orca::ios {

namespace es_err {
inline constexpr s32 kInvalid = -1017;  // ES: invalid argument
inline constexpr s32 kFsNotFound = -106;
} // namespace es_err

struct TitleContext {
    std::vector<u8> ticket; // signed ticket, 0x2A4 bytes
    std::vector<u8> tmd;    // signed TMD

    u64 title_id() const;
    std::vector<u8> ticket_view() const;
};

// Checks the sizes of a TMD; returns the content count or nullopt.
std::optional<u32> tmd_content_count(std::span<const u8> tmd);
std::vector<u8> tmd_view(std::span<const u8> tmd); // empty if the TMD is malformed

class Es final : public Device {
public:
    Es() : Device("/dev/es") {}
    // Validates and installs the active title; false (with `error`) if malformed.
    bool set_active_title(std::vector<u8> ticket, std::vector<u8> tmd, std::string& error);
    const std::optional<TitleContext>& active_title() const noexcept { return title_; }
    // NAND title/ticket store (nullptr: none) and the console's identity
    // (from the user's keys.bin) used by the requests above.
    void set_nand(NandFs* nand) noexcept { nand_ = nand; }
    void set_console(std::optional<u32> console_id, std::optional<u8> boot2_version, bool korean_key) noexcept {
        console_id_ = console_id;
        boot2_version_ = boot2_version;
        korean_key_ = korean_key;
    }
    std::vector<u64> installed_titles() const; // with a TMD
    std::vector<u64> owned_titles() const;     // with a ticket
    std::optional<std::vector<u8>> stored_tmd(u64 title_id) const;

    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in,
                  std::span<const IoVector> io) override;

private:
    std::optional<TitleContext> title_;
    NandFs* nand_{nullptr};
    std::optional<u32> console_id_;
    std::optional<u8> boot2_version_;
    bool korean_key_{false};
    std::optional<std::vector<u8>> read_nand_file(const std::string& path) const;
};

} // namespace orca::ios
