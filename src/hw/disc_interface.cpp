#include "orca/hw/disc_interface.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace orca::hw {

using di::DriveError;
using di::DriveState;

namespace {
// One DTK sample: predictor from the header's high nibble, shift from its low nibble;
// history kept with 6 fractional bits.
s16 dtk_sample(s32 nibble, u8 header, s32& h1, s32& h2) noexcept {
    s32 pred = 0;
    switch (header >> 4) {
    case 0: pred = 0; break;
    case 1: pred = h1 * 0x3C; break;
    case 2: pred = h1 * 0x73 - h2 * 0x34; break;
    default: pred = h1 * 0x62 - h2 * 0x37; break; // only 0-3 exist (2-bit filter number)
    }
    pred = std::clamp((pred + 0x20) >> 6, -0x200000, 0x1FFFFF);
    const s32 cur = ((static_cast<s32>(static_cast<s16>(static_cast<u16>(nibble << 12))) >> (header & 0xFU)) << 6) + pred;
    h2 = h1;
    h1 = cur;
    return static_cast<s16>(std::clamp(cur >> 6, -0x8000, 0x7FFF));
}
} // namespace

void decode_dtk_block(const u8* block, s16* out, s32* hist) noexcept {
    for (unsigned i = 0; i < 28; ++i) {
        const u8 b = block[4 + i];
        out[2 * i] = dtk_sample(b & 0xF, block[0], hist[0], hist[1]);
        out[2 * i + 1] = dtk_sample(b >> 4, block[1], hist[2], hist[3]);
    }
}

DiscInterface::DiscInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi)
    : bus_(bus), scheduler_(scheduler), pi_(pi) {}

void DiscInterface::insert(std::unique_ptr<disc::DiscReader> disc) {
    disc_ = std::move(disc);
    state_ = disc_ ? DriveState::DiscIdNotRead : DriveState::NoMediumPresent;
    error_ = DriveError::None;
    dtk_enabled_ = false;
    stream_ = false;
}

void DiscInterface::after_ipl_boot(bool streaming, u8 size) {
    // BS2 enables DTK from header byte 8 with the size in byte 9 (0 = its default 10)
    dtk_enabled_ = streaming;
    dtk_buffer_ = streaming ? (size == 0U ? u8{10} : size) : u8{0};
    state_ = DriveState::Ready;
}

void DiscInterface::update_interrupt() {
    const bool tc = (disr_ & di::DISR_TCINT) != 0U && (disr_ & di::DISR_TCINTMASK) != 0U;
    const bool de = (disr_ & di::DISR_DEINT) != 0U && (disr_ & di::DISR_DEINTMASK) != 0U;
    const bool br = (disr_ & di::DISR_BRKINT) != 0U && (disr_ & di::DISR_BRKINTMASK) != 0U;
    const bool cv = (dicvr_ & di::DICVR_CVRINT) != 0U && (dicvr_ & di::DICVR_CVRINTMASK) != 0U;
    if (wii_) return; // Starlet's interrupt (see set_wii_mode)
    if (tc || de || br || cv) pi_.raise(pi::CAUSE_DI); else pi_.lower(pi::CAUSE_DI);
}

bool DiscInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size != 4U) return fail("DI: only 32-bit accesses are implemented");
    switch (offset) {
    case 0x00: value = disr_; return true;
    case 0x04: value = dicvr_ | (disc_ ? 0U : di::DICVR_CVR); return true; // open without a disc
    case 0x08: case 0x0C: case 0x10: value = cmd_[(offset - 0x08U) / 4U]; return true;
    case 0x14: value = dimar_; return true;
    case 0x18: value = dilength_; return true;
    case 0x1C: value = dicr_; return true;
    case 0x20: value = diimmbuf_; return true;
    // DICFG: configuration, read-only. Bits 0-7 CONFIG; after the boot ROM has run
    // (the state Orca starts games in) the descrambler is disabled: CONFIG = 1, as
    // Dolphin's DVDInterface sets it.
    case 0x24: value = di::kDicfgAfterBoot; return true;
    default: return fail("DI: register not implemented");
    }
}

bool DiscInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size != 4U) return fail("DI: only 32-bit accesses are implemented");
    const auto v = static_cast<u32>(value);
    switch (offset) {
    case 0x00: {
        if ((v & di::DISR_BRK) != 0U) return fail("DI: break requests are not implemented");
        const u32 masks = di::DISR_DEINTMASK | di::DISR_TCINTMASK | di::DISR_BRKINTMASK;
        const u32 ints = di::DISR_DEINT | di::DISR_TCINT | di::DISR_BRKINT;
        disr_ = (disr_ & ~masks) | (v & masks);
        disr_ &= ~(v & ints); // write 1 to clear
        update_interrupt();
        return true;
    }
    case 0x04:
        dicvr_ = (dicvr_ & ~di::DICVR_CVRINTMASK) | (v & di::DICVR_CVRINTMASK);
        dicvr_ &= ~(v & di::DICVR_CVRINT);
        update_interrupt();
        return true;
    case 0x08: case 0x0C: case 0x10: cmd_[(offset - 0x08U) / 4U] = v; return true;
    case 0x14: dimar_ = v & (wii_ ? ~0x1FU : di::kAddressMask); return true;
    case 0x18: dilength_ = v & ~0x1FU; return true;
    case 0x1C:
        if (busy()) return fail("DI: DICR written while a transfer is in progress");
        dicr_ = v & (di::DICR_TSTART | di::DICR_DMA | di::DICR_RW);
        if ((dicr_ & di::DICR_TSTART) != 0U && !start_command()) { dicr_ = 0; return false; }
        return true;
    case 0x20: diimmbuf_ = v; return true;
    case 0x24: return fail("DI: DICFG (0x24) is read-only; write not supported");
    default: return fail("DI: register not implemented");
    }
}

bool DiscInterface::read_preconditions() {
    if (!disc_) { error_ = DriveError::MediumNotPresent; return false; }
    if (state_ == DriveState::MotorStopped) { error_ = DriveError::MotorStopped; return false; }
    if (state_ == DriveState::DiscIdNotRead) { error_ = DriveError::NoDiscId; return false; }
    return true;
}

bool DiscInterface::start_command() {
    const u32 command = cmd_[0] >> 24U, sub = cmd_[0] & 0xFFU, sub16 = (cmd_[0] >> 16U) & 0xFFU;
    char buf[112];
    if ((dicr_ & di::DICR_RW) != 0U) return fail("DI: writes to the drive are not implemented");
    if (command != 0xE0U) error_ = DriveError::None; // 0xE0 reports the previous command's error
    auto dma_only = [&]() -> bool {
        if ((dicr_ & di::DICR_DMA) == 0U) return fail("DI: immediate-mode transfer of a DMA command is not implemented");
        if (dilength_ != 0U && !bus_.is_ram(dimar_, dilength_)) return fail("DI: DMA destination is not RAM");
        return true;
    };
    auto done = [&](bool ok, u32 from_disc = 0, u64 off = 0) {
        scheduler_.schedule_in(latency_, "DI command", [this, ok, from_disc, off](u64, u64) { finish(ok, from_disc, off); });
        return true;
    };
    switch (command) {
    case 0x12: { // inquiry: revision level/device code, release date, version (a drive's report)
        if (!dma_only()) return false;
        const u32 words[3] = {0x00000002U, 0x20060526U, 0x41000000U};
        for (unsigned k = 0; k < 3 && 4U * k + 4U <= dilength_; ++k) (void)bus_.write32(dimar_ + 4U * k, words[k]);
        return done(true);
    }
    case di::kCommandRead: break;
    case 0xAB: return done(true); // seek: no drive model, nothing else to do
    case 0xA4: // report key: a retail drive rejects it (games use it to tell retail
               // hardware from a development drive; Dolphin DVDInterface.cpp)
        error_ = DriveError::InvalidCommand;
        return done(false);
    case 0xE0: { // request error
        const u32 st = state_ == DriveState::Ready ? 0U : static_cast<u32>(state_) - 1U;
        diimmbuf_ = (st << 24) | static_cast<u32>(error_);
        error_ = DriveError::None;
        return done(true);
    }
    case 0xE1: case 0xE2: { // audio stream / audio status
        if (!read_preconditions()) return done(false);
        if (!dtk_enabled_) { error_ = DriveError::NoAudioBuffer; return done(false); }
        if (command == 0xE1) {
            if (state_ == DriveState::ReadyNoReadsMade) state_ = DriveState::Ready;
            if (sub16 == 0U) {
                const u64 off = u64{cmd_[1]} << 2;
                const u32 len = cmd_[2];
                if (off == 0U && len == 0U) {
                    stop_at_track_end_ = true;
                } else if (!stop_at_track_end_) {
                    next_start_ = off;
                    next_length_ = len;
                    if (!stream_) {
                        current_start_ = next_start_;
                        current_length_ = next_length_;
                        audio_position_ = current_start_;
                        std::fill(std::begin(hist_), std::end(hist_), 0);
                        stream_ = true;
                    }
                }
            } else if (sub16 == 1U) {
                stop_at_track_end_ = false;
                stream_ = false;
            } else {
                error_ = DriveError::InvalidAudioCommand;
                return done(false);
            }
            return done(true);
        }
        switch (sub16) {
        case 0: diimmbuf_ = stream_ ? 1U : 0U; break;
        case 1: diimmbuf_ = static_cast<u32>((audio_position_ & ~u64{0x7FFF}) >> 2); break;
        case 2: diimmbuf_ = static_cast<u32>(current_start_ >> 2); break;
        case 3: diimmbuf_ = current_length_; break;
        default: error_ = DriveError::InvalidAudioCommand; return done(false);
        }
        return done(true);
    }
    case 0xE3: // stop motor (bit 17 = eject, bit 20 = "kill")
        if ((cmd_[0] & (1U << 17)) != 0U && (cmd_[0] & (1U << 20)) == 0U) return fail("DI: disc eject is not implemented");
        if (state_ == DriveState::Ready || state_ == DriveState::ReadyNoReadsMade || state_ == DriveState::DiscIdNotRead)
            state_ = DriveState::MotorStopped;
        return done(true);
    case 0xE4: // audio buffer config: only right after reading the disc ID
        if (!read_preconditions()) return done(false);
        if (state_ == DriveState::Ready) { error_ = DriveError::InvalidPeriod; return done(false); }
        dtk_enabled_ = ((cmd_[0] >> 16) & 1U) != 0U;
        dtk_buffer_ = static_cast<u8>(cmd_[0] & 0xFU);
        return done(true);
    default:
        std::snprintf(buf, sizeof buf, "DI: command 0x%02X not implemented (CMDBUF0=%08X)", command, cmd_[0]);
        return fail(buf);
    }
    // 0xA8 read
    if (!dma_only()) return false;
    if (sub == 0x40U) { // disc ID
        if (!disc_) { error_ = DriveError::MediumNotPresent; return done(false); }
        if (state_ == DriveState::MotorStopped) { error_ = DriveError::MotorStopped; return done(false); }
        if (state_ == DriveState::DiscIdNotRead) state_ = DriveState::ReadyNoReadsMade;
        else if (state_ == DriveState::ReadyNoReadsMade) state_ = DriveState::Ready;
        return done(true, std::min<u32>(0x20U, dilength_), 0);
    }
    if (sub != 0x00U) {
        std::snprintf(buf, sizeof buf, "DI: read sub-command 0x%02X not implemented (CMDBUF0=%08X)", sub, cmd_[0]);
        return fail(buf);
    }
    if (!read_preconditions()) return done(false);
    if (state_ == DriveState::ReadyNoReadsMade) state_ = DriveState::Ready;
    const u64 offset = u64{cmd_[1]} << 2;
    const u32 length = std::min(cmd_[2], dilength_);
    if (offset + length > disc_->size()) { error_ = DriveError::BlockOutOfBounds; return done(false); }
    return done(true, length, offset);
}

std::string DiscInterface::recent_commands() const {
    std::string out;
    for (const auto& h : history_) out += (out.empty() ? "" : " ") + h;
    return out;
}

void DiscInterface::finish(bool ok, u32 from_disc, u64 offset) {
    {
        char b[48];
        std::snprintf(b, sizeof b, "[%08X %08X %08X]", cmd_[0], cmd_[1], cmd_[2]);
        history_.push_back(b);
    }
    if (ok && from_disc != 0U) {
        std::vector<u8> data(from_disc);
        if (!disc_ || !disc_->read(offset, data)) {
            ok = false;
            error_ = DriveError::BlockOutOfBounds;
            if (disc_ && offset + from_disc <= disc_->size()) {
                char b[96];
                std::snprintf(b, sizeof b, "DI: the disc image could not deliver 0x%X bytes at 0x%llX", from_disc, static_cast<unsigned long long>(offset));
                image_failure_ = b;
                const std::string why = disc_->failure_reason();
                if (!why.empty()) image_failure_ += ": " + why;
            }
        }
        else (void)bus_.dma_write(dimar_, data);
    }
    history_.back() += ok ? "ok" : "error";
    if (history_.size() > 6U) history_.pop_front();
    if (ok) {
        dimar_ += dilength_;
        dilength_ = 0;
        disr_ |= di::DISR_TCINT;
    } else {
        disr_ |= di::DISR_DEINT;
    }
    dicr_ &= ~di::DICR_TSTART;
    update_interrupt();
    if (completion_) completion_(ok);
}

bool DiscInterface::begin_host_read() {
    error_ = DriveError::None;
    if (!read_preconditions()) return false;
    if (state_ == DriveState::ReadyNoReadsMade) state_ = DriveState::Ready;
    return true;
}

void DiscInterface::reset_drive(bool spinup) {
    disr_ = 0;
    dicvr_ = 0;
    cmd_[0] = cmd_[1] = cmd_[2] = 0;
    dimar_ = dilength_ = dicr_ = diimmbuf_ = 0;
    error_ = DriveError::None;
    stream_ = false;
    stop_at_track_end_ = false;
    dtk_enabled_ = false; // configured again after the disc ID is read
    if (disc_) state_ = spinup ? DriveState::DiscIdNotRead : DriveState::MotorStopped;
    update_interrupt();
}

std::size_t DiscInterface::dtk_pull(std::span<s16> out, std::size_t frames) {
    std::size_t done = 0;
    u8 block[32];
    while (done + 28U <= frames && 2U * (done + 28U) <= out.size() && stream_) {
        if (audio_position_ >= current_start_ + current_length_) {
            audio_position_ = next_start_;
            current_start_ = next_start_;
            current_length_ = next_length_;
            if (stop_at_track_end_) { stop_at_track_end_ = false; stream_ = false; break; }
            std::fill(std::begin(hist_), std::end(hist_), 0);
        }
        if (!disc_ || !disc_->read(audio_position_, block)) {
            if (disc_ && audio_position_ + sizeof block <= disc_->size()) {
                image_failure_ = "DI: streaming audio: the disc image could not deliver its data";
                const std::string why = disc_->failure_reason();
                if (!why.empty()) image_failure_ += ": " + why;
            }
            stream_ = false;
            break;
        } // past the disc: the stream ends
        decode_dtk_block(block, out.data() + 2U * done, hist_);
        audio_position_ += 32U;
        done += 28U;
    }
    return done;
}

} // namespace orca::hw
