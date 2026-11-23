// Orca Core — IOS /dev/di: the disc drive as a Wii game uses it (public Wii
// documentation, e.g. WiiBrew "/dev/di").
//
// ioctl requests carry a 0x20-byte input whose first byte repeats the request
// number; results are DI codes: 1 success, 2 drive error, 0x20 security
// error (request refused by IOS), 0x80 bad argument. Requests run one at a
// time, in order; IOS drives the DI hardware itself (DiscInterface in Wii
// mode) and answers 2700 ticks after the drive finishes.
//   0x12 inquiry (DMA 0x20)            0x70 read disc ID (DMA 0x20)
//   0x71 read from the open partition: length (+4), offset >> 2 (+8); data
//        decrypted and hash-checked (disc::WiiPartition)
//   0x7A cover register   0x83 last length (IOS stores the last read's
//        offset there, a known IOS bug)   0x84 immediate buffer
//   0x85 / 0x89 mask / unmask and 0x86 clear the cover interrupt; 0x87 (does
//        nothing in IOS)   0x88 cover status (1 no disc, 2 disc)
//   0x8A reset (+4: spin up)   0x8C close partition
//   0x8D unencrypted read, only inside the system area (offsets < 0x50000)
//        or the two 8-byte areas past the first layer used to test the drive's
//        out-of-bounds error
//   0x95 status register   0x96 control register
//   0xAB seek, 0xE0 request error, 0xE1 audio stream, 0xE2 audio status,
//   0xE3 stop motor, 0xE4 audio buffer config: passed to the drive
// ioctlv 0x8B opens a partition: in 0x20-byte command (+4: offset >> 2),
// ticket, certificate chain (both may be empty: read from the disc); io TMD
// buffer, ES result. The partition's ticket and TMD become ES's active title
// and Broadway's owner/group are set from it (see Es and uid.sys below).
// Every other request stops explicitly (DVD-video commands, wait for cover
// close, notify reset, spin-up flag, other partition-open forms).
//
// Opening the partition also creates the title's NAND directories if missing
// (/title/<high>/<low>/data owned by the title, as ES does for a title it runs).
// Owner IDs: ES keeps /sys/uid.sys on the NAND, a list of {title ID u64,
// uid u32}; a title not listed gets the next uid (0x1000 for the first).
#pragma once
#include "orca/disc/wii_disc.hpp"
#include "orca/hw/disc_interface.hpp"
#include "orca/ios/es.hpp"
#include "orca/ios/kernel.hpp"
#include "orca/ios/nand_fs.hpp"

#include <deque>
#include <memory>
#include <optional>

namespace orca::ios {

namespace di_res {
inline constexpr s32 kSuccess = 1, kDriveError = 2, kSecurity = 0x20, kBadArgument = 0x80;
} // namespace di_res

// Returns Broadway's uid for `title` from /sys/uid.sys, adding it if missing.
std::optional<u32> uid_for_title(NandFs& nand, u64 title, std::string& error);

class DiDevice final : public Device {
public:
    DiDevice(hw::DiscInterface& drive, Es& es, NandFs* nand, disc::WiiKeys keys);

    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) override;
    Result ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in,
                  std::span<const IoVector> io) override;

    // Opens the partition at `offset` (bytes) the way DVDLowOpenPartition does,
    // without a request (HLE boot). On failure `error` says why.
    bool open_partition(Kernel& k, u64 offset, std::string& error);
    disc::WiiPartition* partition() noexcept { return partition_.get(); }

private:
    struct Pending {
        u32 request;
        u32 command;
        u32 out;      // output buffer
        u32 out_size;
        u32 args[7];  // input words 1..7
    };
    std::optional<Result> start(Kernel& k, const Pending& p); // nullopt: waiting for the drive
    void drive_done(Kernel& k, bool ok);
    void next(Kernel& k);
    bool program(Kernel& k, u32 cmd0, u32 cmd1, u32 cmd2, u32 dma_address, u32 dma_length, bool dma);

    hw::DiscInterface& drive_;
    Es& es_;
    NandFs* nand_;
    disc::WiiKeys keys_;
    std::unique_ptr<disc::WiiPartition> partition_;
    std::deque<Pending> queue_;
    std::optional<Pending> running_; // waiting for the drive
    Kernel* kernel_{nullptr};
    u32 last_length_{0};
};

} // namespace orca::ios
