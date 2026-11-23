// Orca Core — the frontend layer shared by the Android app and desktop tools:
// one emulated console, its picture, sound and controls.
//
//   * load_disc(): GameCube or Wii disc (decided by the magic words at 0x18 /
//     0x1C); a Wii disc needs the NAND directory (created with the chosen
//     region if missing) and the common key file.
//   * run_frame(): emulates until the next video field starts (at most 1/10 s
//     of console time); the field's frame buffer is decoded to RGBA.
//   * audio: the mixer's 48 kHz stereo output is collected; take_audio() hands
//     it to the platform's audio thread (bounded: the oldest samples are
//     dropped when nobody reads them, and the count of dropped samples is kept).
//   * controls: GameCube controllers on ports 0-3, Wii Remotes 0-3 (with a
//     Nunchuk when its state is set), reset/power buttons.
// CPU identification (PVR): Gekko 0x00083214, Broadway 0x00087102.
#pragma once
#include "orca/disc/disc_image.hpp"
#include "orca/hw/serial_interface.hpp"
#include "orca/ios/nand_setup.hpp"
#include "orca/system/system.hpp"
#include "orca/wiimote/wiimote.hpp"

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orca::frontend {

struct Settings {
    bool jit{true};
    // DSP: with both ROM dumps (the user's dsp_rom.bin 8 KiB, dsp_coef.bin 4 KiB)
    // the DSP runs its real code (every ucode); otherwise the known ucodes run
    // at a high level and an unknown one stops with its hash.
    bool dsp_hle{true};
    std::string dsp_irom_file, dsp_coef_file;
    std::string nand_dir;      // Wii
    std::string wii_key_file;  // Wii: 16-byte common key or BootMii keys.bin
    ios::NandSetup new_nand;   // used when the NAND has no SYSCONF yet
};

struct Frame {
    u32 width{0}, height{0};
    std::vector<u8> rgba;
    u64 number{0};
};

class Emulator {
public:
    explicit Emulator(Settings settings);
    ~Emulator();
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    bool load_disc(std::unique_ptr<disc::DiscReader> disc, std::string& error);
    bool load_executable(std::vector<u8> file, bool wii, std::string& error);
    bool loaded() const noexcept { return sys_ != nullptr; }
    bool wii() const noexcept { return wii_; }
    const std::string& game_id() const noexcept { return game_id_; }

    // False when emulation stopped (error() says why).
    bool run_frame();
    const Frame& frame() const noexcept { return frame_; }
    // Decodes the field the VI shows now into frame() (run_frame does this at every field).
    bool capture_frame();
    const std::string& error() const noexcept { return error_; }
    u64 emulated_cycles() const noexcept { return cycles_; }
    u64 cpu_hz() const noexcept;

    void set_pad(unsigned port, const hw::PadState& state);
    void set_remote(unsigned index, const wiimote::InputState& state);
    bool connect_remote(unsigned index);
    std::size_t remote_count() const;
    void press_button(bool power);

    // Interleaved stereo 16-bit samples at 48 kHz; returns the number of values written.
    std::size_t take_audio(std::span<s16> out);
    u64 dropped_audio() const noexcept { return dropped_; }

    system::System* system() noexcept { return sys_.get(); }

private:
    bool make_system(bool wii, std::string& error);

    Settings settings_;
    std::unique_ptr<system::System> sys_;
    std::array<std::shared_ptr<hw::StandardController>, 4> pads_{};
    bool wii_{false};
    std::string game_id_;
    Frame frame_;
    bool field_seen_{false};
    u64 cycles_{0};
    std::string error_;
    std::mutex audio_mutex_;
    std::deque<s16> audio_;
    u64 dropped_{0};
};

} // namespace orca::frontend
