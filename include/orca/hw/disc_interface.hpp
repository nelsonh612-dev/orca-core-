// Orca Core — Disc Interface (DI), physical 0x0C006000, and the drive behind it.
//
// Registers (32-bit; public GameCube documentation, YAGCD / libogc):
//   0x00 DISR    status: BRK(0) DEINTMASK(1) DEINT(2) TCINTMASK(3) TCINT(4)
//                BRKINTMASK(5) BRKINT(6); interrupt bits are write-1-to-clear
//   0x04 DICVR   cover: CVR(0, 1 = open) CVRINTMASK(1) CVRINT(2, w1c)
//   0x08-0x10    DICMDBUF0-2   command words
//   0x14 DIMAR   DMA memory address (physical, 32-byte aligned)
//   0x18 DILENGTH DMA length (multiple of 32)
//   0x1C DICR    control: TSTART(0) DMA(1) RW(2: 1 = write to drive)
//   0x20 DIIMMBUF immediate data
//
// Drive commands (CMDBUF0 >> 24; behaviour of the drive as documented by YAGCD
// and as Dolphin's DVDInterface.cpp describes it, used as information):
//   0x12 inquiry (DMA: revision, date, version)          0xAB seek
//   0xA8 read: sub 0x00 sector data (offset = CMDBUF1 << 2, length CMDBUF2),
//        sub 0x40 disc ID (0x20 bytes from offset 0)     0xE0 request error
//   0xE1 audio stream (sub 0 play/queue, 1 stop)          0xE2 audio status
//   0xE3 stop motor                                        0xE4 audio buffer config
// The drive keeps a state (disc ID not read -> ready without reads -> ready;
// motor stopped) and an error code that 0xE0 returns as (state << 24) | error and
// clears. Reads need a disc, a running motor and the disc ID read; past the end of
// the disc they fail with "block out of bounds". A failing command sets DEINT, a
// successful one TCINT (DMA commands advance DIMAR by DILENGTH and clear it).
// Streaming audio (DTK): 32-byte blocks of 28 stereo ADPCM samples read from the
// disc while the AI plays (decoded by dtk_pull()).
// Stops with a reason (no invented behaviour): other commands and sub-commands,
// break requests, writes to the drive, immediate-mode reads, eject.
//
// Timing: there is no drive model; a command completes `latency_cycles` after
// TSTART (default 0 = at the next scheduler step, still asynchronous).
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/disc/disc_image.hpp"
#include "orca/hw/processor_interface.hpp"

#include <functional>
#include <memory>
#include <span>
#include <deque>
#include <string>

namespace orca::hw {

namespace di {
// DICFG (0x24) value once the boot ROM has run: CONFIG = 1 (descrambler disabled).
inline constexpr u32 kDicfgAfterBoot = 1U;
inline constexpr u32 kBase = 0x0C006000U;
inline constexpr u32 DISR_BRK = 0x01U, DISR_DEINTMASK = 0x02U, DISR_DEINT = 0x04U, DISR_TCINTMASK = 0x08U,
                     DISR_TCINT = 0x10U, DISR_BRKINTMASK = 0x20U, DISR_BRKINT = 0x40U;
inline constexpr u32 DICVR_CVR = 0x01U, DICVR_CVRINTMASK = 0x02U, DICVR_CVRINT = 0x04U;
inline constexpr u32 DICR_TSTART = 0x01U, DICR_DMA = 0x02U, DICR_RW = 0x04U;
inline constexpr u32 kCommandRead = 0xA8U;
inline constexpr u32 kAddressMask = 0x03FFFFE0U;

enum class DriveState : u8 { Ready = 0, ReadyNoReadsMade = 1, CoverOpened = 2, DiscChangeDetected = 3,
                             NoMediumPresent = 4, MotorStopped = 5, DiscIdNotRead = 6 };
// Sense key (nibble) + ASC/ASCQ, as the drive reports them
enum class DriveError : u32 { None = 0x00000, MotorStopped = 0x20400, NoDiscId = 0x20401, MediumNotPresent = 0x23A00,
                              InvalidCommand = 0x52000, NoAudioBuffer = 0x52001, BlockOutOfBounds = 0x52100,
                              InvalidAudioCommand = 0x52401, InvalidPeriod = 0x52402 };
} // namespace di

class DiscInterface final : public mem::MmioDevice {
public:
    // Diagnostics: the last commands (command words and outcome), oldest first.
    std::string recent_commands() const;
    DiscInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi);

    // A new disc: the drive must read its ID before data (state DiscIdNotRead).
    void insert(std::unique_ptr<disc::DiscReader> disc);
    disc::DiscReader* disc() noexcept { return disc_.get(); }
    void set_latency_cycles(u64 cycles) noexcept { latency_ = cycles; }

    // What the IPL leaves after booting a disc (HLE boot): disc ID and apploader
    // read, so the drive is Ready; streaming configured from the header (bytes 8/9).
    void after_ipl_boot(bool streaming, u8 stream_buffer_size);

    di::DriveState drive_state() const noexcept { return state_; }
    di::DriveError drive_error() const noexcept { return error_; }
    bool streaming_enabled() const noexcept { return dtk_enabled_; }
    bool stream_playing() const noexcept { return stream_; }

    // Streaming audio: decodes the next `frames` stereo samples (multiple of 28)
    // into `out` (left, right interleaved) and advances; returns the frames
    // produced (fewer when the stream ends or is stopped; 0 if not playing).
    std::size_t dtk_pull(std::span<s16> out, std::size_t frames);

    bool busy() const noexcept { return (dicr_ & di::DICR_TSTART) != 0U; }

    // Wii: the DI belongs to Starlet (IOS). DMA addresses span MEM1 and MEM2, and
    // the interrupt does not reach Broadway's PI; IOS learns of finished commands
    // through the completion listener (called after the status bits are set).
    void set_wii_mode() noexcept { wii_ = true; }
    void set_completion_listener(std::function<void(bool ok)> f) { completion_ = std::move(f); }
    // A read that IOS serves itself (decrypted partition data): the same drive
    // checks and state change as a 0xA8 read; false sets the drive error.
    bool begin_host_read();
    // Drive reset (IOS DVDLowReset): registers cleared; with `spinup` the drive
    // must read the disc ID again, otherwise its motor is stopped.
    void reset_drive(bool spinup);
    const std::string& last_error() const noexcept { return last_error_; }
    // A read inside the disc that the image could not deliver (corrupt or
    // missing data): the emulation stops with this reason.
    bool image_failed() const noexcept { return !image_failure_.empty(); }
    const std::string& image_failure() const noexcept { return image_failure_; }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "DI"; }
    std::string mmio_error() const override { return last_error_; }

private:
    bool start_command();
    void finish(bool ok, u32 dma_bytes_from_disc, u64 disc_offset);
    bool read_preconditions();
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    mem::Bus& bus_;
    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    std::unique_ptr<disc::DiscReader> disc_;
    u64 latency_{0};
    u32 disr_{0}, dicvr_{0}, cmd_[3]{}, dimar_{0}, dilength_{0}, dicr_{0}, diimmbuf_{0};
    std::deque<std::string> history_;
    di::DriveState state_{di::DriveState::NoMediumPresent};
    di::DriveError error_{di::DriveError::None};
    // streaming audio
    bool dtk_enabled_{false};
    u8 dtk_buffer_{0};
    bool stream_{false}, stop_at_track_end_{false};
    u64 current_start_{0}, next_start_{0}, audio_position_{0};
    u32 current_length_{0}, next_length_{0};
    s32 hist_[4]{}; // left h1, h2, right h1, h2
    std::function<void(bool)> completion_;
    bool wii_{false};
    std::string last_error_;
    std::string image_failure_;
};

// DTK ADPCM: one 32-byte block (header byte 0 left, 1 right; 28 data bytes, low
// nibble left, high nibble right) -> 28 stereo samples. `hist` = left h1, h2, right h1, h2.
void decode_dtk_block(const u8* block, s16* stereo_out, s32* hist) noexcept;

} // namespace orca::hw
