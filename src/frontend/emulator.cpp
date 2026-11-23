#include "orca/frontend/emulator.hpp"

#include "orca/video/xfb.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <cstdio>

namespace orca::frontend {

namespace {
constexpr u32 kGekkoPvr = 0x00083214U, kBroadwayPvr = 0x00087102U;
constexpr std::size_t kMaxAudio = 48000U * 2U / 2U; // half a second of stereo samples
} // namespace

Emulator::Emulator(Settings settings) : settings_(std::move(settings)) {}
Emulator::~Emulator() = default;

u64 Emulator::cpu_hz() const noexcept { return sys_ ? sys_->clocks().cpu_hz : 0; }

bool Emulator::make_system(bool wii, std::string& error) {
    system::SystemConfig cfg;
    cfg.console = wii ? system::Console::Wii : system::Console::GameCube;
    cfg.pvr = wii ? kBroadwayPvr : kGekkoPvr;
    cfg.use_jit = settings_.jit;
    // DSP ROM dumps (big-endian 16-bit words)
    std::vector<u16> irom, coef;
    const auto words = [](const std::string& path, std::size_t n, std::vector<u16>& out) {
        if (path.empty()) return false;
        std::ifstream in(path, std::ios::binary);
        const std::vector<u8> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (b.size() != n * 2U) return false;
        out.resize(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<u16>(b[2 * i] << 8 | b[2 * i + 1]);
        return true;
    };
    const bool roms = words(settings_.dsp_irom_file, dsp::kIromWords, irom) && words(settings_.dsp_coef_file, dsp::kCoefWords, coef);
    if (!roms && (!settings_.dsp_irom_file.empty() || !settings_.dsp_coef_file.empty())) {
        error = "DSP: the ROM dumps must be 8192 (IROM) and 4096 (COEF) bytes";
        return false;
    }
    cfg.dsp_hle = roms ? false : settings_.dsp_hle;
    if (wii) {
        if (settings_.nand_dir.empty()) { error = "Wii: no NAND directory configured"; return false; }
        ios::NandFs nand(settings_.nand_dir);
        if (!nand.load(error)) return false;
        ios::FsMeta m;
        if (nand.metadata(0, 0, "/shared2/sys/SYSCONF", m) != 0 && !ios::create_nand(nand, settings_.new_nand, error))
            return false;
        cfg.nand_root = settings_.nand_dir;
        if (settings_.wii_key_file.empty()) { error = "Wii: no common key file configured"; return false; }
        if (!disc::load_wii_key_file(settings_.wii_key_file, cfg.wii_keys, 0, error)) return false;
    }
    sys_ = std::make_unique<system::System>(cfg);
    if (roms && !(sys_->dsp().core().load_irom(irom) && sys_->dsp().core().load_coef(coef))) {
        error = "DSP: the ROM dumps were rejected";
        sys_.reset();
        return false;
    }
    if (wii && !sys_->nand_error().empty()) { error = "NAND: " + sys_->nand_error(); sys_.reset(); return false; }
    wii_ = wii;
    for (unsigned p = 0; p < 4; ++p) pads_[p] = std::make_shared<hw::StandardController>();
    sys_->si().connect(0, pads_[0]); // one controller plugged in; others when they send input
    sys_->set_field_listener([this](bool) { field_seen_ = true; });
    sys_->mixer().set_sink([this](std::span<const s16> f) {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_.insert(audio_.end(), f.begin(), f.end());
        while (audio_.size() > kMaxAudio) { audio_.pop_front(); ++dropped_; }
    });
    return true;
}

bool Emulator::load_disc(std::unique_ptr<disc::DiscReader> d, std::string& error) {
    std::array<u8, 0x20> id{};
    if (!d || !d->read(0, id)) { error = "the disc image is unreadable"; return false; }
    const auto word = [&](std::size_t o) { return static_cast<u32>(id[o]) << 24 | static_cast<u32>(id[o + 1]) << 16 | static_cast<u32>(id[o + 2]) << 8 | id[o + 3]; };
    const bool wii = word(0x18) == disc::kWiiMagic;
    if (!wii && word(0x1C) != disc::kGameCubeMagic) { error = "not a GameCube or Wii disc image (no magic word)"; return false; }
    if (!make_system(wii, error)) return false;
    game_id_.assign(reinterpret_cast<const char*>(id.data()), 6);
    sys_->insert_disc(std::move(d));
    if (!(wii ? sys_->boot_wii_disc(error) : sys_->boot_disc(error))) { sys_.reset(); return false; }
    return true;
}

bool Emulator::load_executable(std::vector<u8> file, bool wii, std::string& error) {
    if (!make_system(wii, error)) return false;
    if (!sys_->load_executable(file, error)) { sys_.reset(); return false; }
    game_id_.clear();
    return true;
}

bool Emulator::capture_frame() {
    if (!sys_) return false;
    const hw::XfbField f = sys_->vi().xfb_field(sys_->vi().bottom_field());
    if (f.width == 0U || f.height == 0U) return false;
    std::vector<u8> raw(static_cast<std::size_t>(f.stride) * f.height);
    if (sys_->bus().dma_read(f.address, raw) != mem::BusStatus::Ok) return false;
    if (!video::decode_xfb(raw, f.width, f.height, f.stride, frame_.rgba)) return false;
    frame_.width = f.width;
    frame_.height = f.height;
    ++frame_.number;
    return true;
}

bool Emulator::run_frame() {
    if (!sys_) { error_ = "nothing loaded"; return false; }
    const u64 slice = sys_->clocks().cpu_hz / 2000U; // 0.5 ms
    const u64 limit = sys_->clocks().cpu_hz / 10U;
    field_seen_ = false;
    u64 ran = 0;
    while (!field_seen_ && ran < limit) {
        const auto r = sys_->run(slice);
        ran += r.cycles;
        cycles_ += r.cycles;
        if (r.device_stop) { error_ = r.device_error; return false; }
        if (r.stop != ppc::StepResult::Executed) {
            const auto& st = sys_->cpu().last_stop();
            char where[48];
            std::snprintf(where, sizeof where, " at %08X (%08X)", st.pc, st.instruction);
            error_ = std::string("the CPU stopped: ") + ppc::to_string(r.stop) + where;
            if (!st.reason.empty()) error_ += ": " + st.reason;
            return false;
        }
        if (r.cycles == 0U) { error_ = "no progress"; return false; }
    }
    if (field_seen_) (void)capture_frame();
    return true;
}

void Emulator::set_pad(unsigned port, const hw::PadState& state) {
    if (!sys_ || port > 3U) return;
    sys_->si().connect(port, pads_[port]);
    pads_[port]->state = state;
}

void Emulator::set_remote(unsigned index, const wiimote::InputState& state) {
    if (sys_ && sys_->bluetooth() && index < sys_->bluetooth()->remote_count()) sys_->bluetooth()->set_input(index, state);
}

bool Emulator::connect_remote(unsigned index) {
    return sys_ && sys_->bluetooth() && index < sys_->bluetooth()->remote_count() && sys_->bluetooth()->connect(index);
}

std::size_t Emulator::remote_count() const {
    return sys_ && sys_->bluetooth() ? sys_->bluetooth()->remote_count() : 0U;
}

void Emulator::press_button(bool power) {
    if (sys_) (void)sys_->press_console_button(power);
}

std::size_t Emulator::take_audio(std::span<s16> out) {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    const std::size_t n = std::min(out.size(), audio_.size());
    std::copy_n(audio_.begin(), n, out.begin());
    audio_.erase(audio_.begin(), audio_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

} // namespace orca::frontend
