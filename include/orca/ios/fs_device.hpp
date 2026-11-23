// Orca Core — IOS /dev/fs: NAND files (open/read/write/seek/close on paths
// outside /dev/) and the ISFS requests (public Wii documentation, e.g.
// WiiBrew "/dev/fs"):
//   ioctl 2 NAND statistics (out: 7 words: cluster size 0x4000, free, used,
//           bad and reserved clusters, free and used inodes)
//   ioctl 3 create directory, 9 create file, 5 set attributes; input block
//           (0x4A bytes): uid u32, gid u16, path[64], owner/group/other
//           permissions, attribute
//   ioctl 6 get attributes (in: path[64]; out: the same 0x4A-byte block)
//   ioctl 7 delete (in: path[64]); 8 rename (in: old path[64], new path[64])
//   ioctl 11 file status on a file descriptor (out: size, position)
//   ioctl 13 shutdown (flush; nothing to do here)
//   ioctlv 4 read directory: in path[64] (+ in max count), io count (+ names,
//           13 bytes each, packed: every name is followed by its zero byte)
//   ioctlv 12 usage: in path[64]; io clusters, inodes
// Permission checks use the owner/group IOS gave Broadway (the title's,
// set at boot: Kernel::set_ppc_credentials); without them every request
// stops explicitly. Format (ioctl 1), file version control (ioctl 10) and
// unknown requests stop explicitly.
//
// Reply delays approximate the hardware measurements reported in Dolphin's
// notes: 2700 ticks per request, 115000 / 300000 ticks per started 16 KiB
// cluster read / written, 3170000 ticks for each update of the FS metadata
// block (create, delete, rename, attributes, and closing a written file).
#pragma once
#include "orca/ios/kernel.hpp"
#include "orca/ios/nand_fs.hpp"

#include <array>
#include <optional>

namespace orca::ios {

class FsDevice final : public Device {
public:
    explicit FsDevice(NandFs& nand) : Device("/dev/fs"), nand_(nand) {}

    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result read(Kernel& k, const Request& r, u32 buffer, u32 size) override;
    Result write(Kernel& k, const Request& r, u32 buffer, u32 size) override;
    Result seek(Kernel& k, const Request& r, s32 offset, u32 whence) override;
    Result ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) override;
    Result ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in,
                  std::span<const IoVector> io) override;

private:
    struct Open {
        bool used{false};
        s32 file{-1}; // NandFs handle; -1 for /dev/fs itself
        u32 uid{0};
        u16 gid{0};
        bool written{false};
    };
    std::optional<Result> credentials(Kernel& k, u32& uid, u16& gid) const;
    Open* slot(const Request& r);

    NandFs& nand_;
    std::array<Open, kMaxFds> open_{};
};

} // namespace orca::ios
