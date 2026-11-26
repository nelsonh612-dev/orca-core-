// Orca Core — the frontend layer shared by the Android app and desktop tools:
// one emulated console, its picture, sound and controls.
//
//   * load_disc(): a disc image in any supported format (ISO/GCM, CISO, WBFS,
//     GCZ; see disc/blob.hpp), GameCube or Wii (decided by the magic words at
//     0x18 / 0x1C); a Wii disc needs the NAND directory (created with the chosen
//     region if missing) and, when its partition is encrypted, the user's common
//     key (the error names the key that is missing or wrong).
//   * run_frame(): emulates until the next video field starts (at most 1/10 s
//     of console time); the field's frame buffer is decoded to RGBA.
//   * audio: the mixer's 48 kHz stereo output is collected; take_audio() hands
//     it to the platform's audio thread (bounded: the oldest samples are
//     dropped when nobody reads them, and the count of dropped samples is kept).
//   * controls: GameCube controllers on ports 0-3, Wii Remotes 0-3 (with a
//     Nunchuk when its state is set), reset/power buttons.
// CPU identification (PVR): Gekko 0x00083214, Broadway 0x00087102.
#pragma once
#include "orca/disc/blob.hpp"
#include "orca/disc/disc_image.hpp"
#include "orca/gpu/gpu.hpp"
#include "orca/hw/serial_interface.hpp"
#include "orca/ios/nand_setup.hpp"
#include "orca/system/system.hpp"
#include "orca/wiimote/wiimote.hpp"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
    std::string wii_key_file;  // Wii: a key file load_wii_key_file accepts (optional for unencrypted discs)
    // Wii: extra directories OrcaIOSC searches automatically for the user's key
    // files (besides $ORCA_WII_KEYS, $ORCA_KEYS_DIR and ~/.config/orca).
    std::vector<std::string> key_dirs;
    ios::NandSetup new_nand;   // used when the NAND has no SYSCONF yet
};

struct Frame {
    u32 width{0}, height{0};
    std::vector<u8> rgba;  // empty when the frame is on the GPU (gpu.valid)
    u64 number{0};
    // The field is a display copy still on the GPU backend (Gpu::gpu_field):
    // a frontend with that backend presents it from there, without reading RAM.
    gpu::Gpu::GpuField gpu;
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
    disc::ImageFormat image_format() const noexcept { return format_; }

    // False when emulation stopped (error() says why).
    bool run_frame();
    // Diagnostics for a game that runs but shows nothing: where the CPU spends
    // its time (most sampled PCs since the last call), LR, MSR[EE], the VI frame
    // buffer, pending interrupt causes and the IOS requests waiting. Resets the
    // PC samples.
    std::string diagnostics();
    const Frame& frame() const noexcept { return frame_; }
    // Decodes the field the VI shows now into frame() (run_frame does this at every field).
    bool capture_frame();
    // With a GPU presenter (it tells whether copy `handle` is still on the GPU),
    // capture_frame() leaves a field that is a display copy on the GPU there
    // (frame().gpu) instead of decoding RAM. frame_rgba() decodes it from RAM
    // when the frontend needs the pixels after all.
    void set_gpu_presenter(std::function<bool(u32 handle)> on_gpu) { gpu_presenter_ = std::move(on_gpu); }
    bool frame_rgba();
    u64 gpu_frames() const noexcept { return gpu_frames_; }
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
    bool decode_field();

    Settings settings_;
    std::unique_ptr<system::System> sys_;
    std::array<std::shared_ptr<hw::StandardController>, 4> pads_{};
    bool wii_{false};
    std::string game_id_;
    disc::ImageFormat format_{disc::ImageFormat::Raw};
    Frame frame_;
    hw::XfbField frame_field_;
    std::function<bool(u32)> gpu_presenter_;
    u64 gpu_frames_{0};
    std::map<u32, u32> pc_samples_;
    bool halted_by_guest();
    enum class LoopKind { None, NoMemory, ReadsMemory };
    // A busy loop starting at pc and closed by an unconditional branch back to it;
    // `words` (optional) gets the instructions read.
    LoopKind closed_loop(u32 pc, std::string* words = nullptr, u32* length = nullptr);
    bool read_guest32(u32 ea, u32& v);
    std::string polling_loop(u32 hot_pc);
    std::string panic_message(u32 sp);
    std::string video_mode_word();
    std::string guest_text(u32 ea);
    u32 pc_sample_total_{0};
    bool field_seen_{false};
    u64 cycles_{0};
    u64 run_ns_{0};
    std::map<u32, std::string> store_sites_; // diagnostics: stores to a polled global   // host time in System::run (diagnostics)
    struct HostMark {
        u64 cycles{0}, run{0}, gpu{0}, dsp{0}, events_ns{0}, slices{0}, fired{0};
        u64 copies{0}, readback{0}, encode{0}, upload{0};
        u64 jit_steps{0}, jit_runs{0}, interp{0}, compiled{0}, invalidations{0}, helper{0}, fallbacks{0}, resumed{0};
        std::unordered_map<std::string, u64> by_name;
        gpu::Gpu::CopyPaths paths;
        std::unordered_map<u32, u64> helper_ops, fallback_ops;
        u64 fields{0}, captured{0};
    } host_mark_;
    // VI fields seen by run_frame, fields captured, and why the last capture failed
    u64 fields_seen_{0}, captured_{0};
    std::string capture_failure_;
    std::string error_;
    std::mutex audio_mutex_;
    std::deque<s16> audio_;
    u64 dropped_{0};
};

} // namespace orca::frontend
