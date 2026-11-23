// Orca Core — the RTC / SRAM / IPL-ROM chip on EXI channel 0, CS1.
//
// Protocol (public sources: YAGCD 10.3 via WiiBrew "MX23L4005"; NetBSD
// sys/arch/evbppc/wii/dev/rtcsram.c (BSD); Linux drivers/rtc/rtc-gamecube.c;
// "GameCube Hacking", 21C3 2004, ROM access protocol slide):
//   After CS is asserted the CPU sends one 32-bit command word:
//     bit 31      1 = write, 0 = read
//     bits 30-6   address (low six bits are not part of the address)
//   then the data bytes are streamed (read or written) from that address,
//   by immediate transfers or DMA, until CS is released.
//   Address map (address = (command & 0x7FFFFFFF) >> 6):
//     0x000000-0x1FFFFF  IPL mask ROM (2 MiB; "00000AAA ... AAxxxxxx")
//     0x800000           RTC counter, 4 bytes  (command 0x20000000)
//     0x800004-0x800043  SRAM, 64 bytes         (command 0x20000100)
//   The RTC counts seconds; the date is counter + SRAM counter bias.
//
// Model:
//   * The ROM is supplied by the user as the bytes the CPU reads with the
//     descrambler on (ROMDIS = 0). No ROM is bundled. Reading it without an
//     image, after ROMDIS (scrambled data is not modelled), past the image or
//     writing it is a device error.
//   * The RTC advances with emulated time (CPU cycles / CPU clock), from a
//     value set by the host (default 0) or by a guest write.
//   * SRAM starts as 64 zero bytes unless the host provides contents; no
//     checksums are computed by the device (the chip does not either; the
//     IPL/OS maintain them).
//   * Streaming past the end of the RTC or SRAM block, other addresses, and
//     data sent before a full command word are device errors.
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/external_interface.hpp"

#include <array>
#include <vector>

namespace orca::hw {

namespace exi_ipl {
inline constexpr u32 kWriteBit = 0x80000000U;
inline constexpr u32 kRomSize = 0x200000U;
inline constexpr u32 kRtcAddress = 0x800000U;   // command 0x20000000
inline constexpr u32 kSramAddress = 0x800004U;  // command 0x20000100
inline constexpr u32 kSramSize = 64U;
constexpr u32 command(u32 address, bool write) noexcept { return (write ? kWriteBit : 0U) | (address << 6); }
} // namespace exi_ipl

class IplRtcSram final : public ExiDevice {
public:
    IplRtcSram(core::Scheduler& scheduler, u64 cpu_hz);

    // ROM bytes as seen with the descrambler on; at most 2 MiB.
    bool load_rom(std::vector<u8> rom);
    void set_rtc(u32 seconds);
    u32 rtc() const;
    void set_sram(const std::array<u8, exi_ipl::kSramSize>& s) { sram_ = s; }
    const std::array<u8, exi_ipl::kSramSize>& sram() const noexcept { return sram_; }
    bool descrambler_disabled() const noexcept { return romdis_; }

    const char* exi_name() const noexcept override { return "EXI0/CS1 RTC-SRAM-ROM"; }
    void select() override;
    void deselect() override;
    bool write(std::span<const u8> bytes, std::string& error) override;
    bool read(std::span<u8> bytes, std::string& error) override;
    void on_romdis() override { romdis_ = true; }

private:
    enum class Phase : u8 { Command, Read, Write };
    enum class Target : u8 { Rom, Rtc, Sram };
    bool decode(std::string& error);
    bool limit(u32 count, std::string& error) const;

    core::Scheduler& scheduler_;
    u64 cpu_hz_;
    std::vector<u8> rom_;
    std::array<u8, exi_ipl::kSramSize> sram_{};
    u32 rtc_base_{0};
    u64 rtc_base_cycle_{0};
    bool romdis_{false};

    Phase phase_{Phase::Command};
    Target target_{Target::Rom};
    u32 command_{0};
    unsigned command_bytes_{0};
    u32 cursor_{0};              // offset inside the target
    std::array<u8, 4> rtc_buf_{}; // RTC snapshot (read) or incoming bytes (write)
};

} // namespace orca::hw
