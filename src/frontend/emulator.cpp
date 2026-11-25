#include "orca/frontend/emulator.hpp"

#include "orca/video/xfb.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <cstdio>
#include <cstring>

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
        cfg.key_dirs = settings_.key_dirs;
        // keys are needed only by encrypted partitions; WiiPartition names the missing one
        if (!settings_.wii_key_file.empty() && !disc::load_wii_key_file(settings_.wii_key_file, cfg.wii_keys, 0, error)) return false;
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

bool Emulator::load_disc(std::unique_ptr<disc::DiscReader> file, std::string& error) {
    // the image file's format (ISO/GCM, CISO, WBFS, GCZ) is detected here
    auto d = disc::open_image(std::move(file), &format_, error);
    if (!d) return false;
    std::array<u8, 0x20> id{};
    if (!d->read(0, id)) { error = "the disc image is unreadable"; return false; }
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
        if (pc_samples_.size() < 4096U) { ++pc_samples_[sys_->cpu().state().pc]; ++pc_sample_total_; }
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
        if (halted_by_guest()) return false;
    }
    if (field_seen_) (void)capture_frame();
    return true;
}

namespace {
// Physical address of an effective address in the usual BAT layout games use
// (0x8/0x9 cached, 0xC/0xD uncached).
std::optional<u32> bat_physical(u32 ea) {
    if (ea < 0x80000000U) return std::nullopt;
    return ea & 0x3FFFFFFFU;
}
} // namespace

bool Emulator::read_guest32(u32 ea, u32& v) {
    const auto pa = bat_physical(ea);
    return pa && sys_->bus().read32(*pa, v) == mem::BusStatus::Ok;
}

// OSPanic(file, line, format, ...) is variadic, so its prologue stores r3-r10 in
// its frame, one after another: find file, line and format (a text with '%')
// next to each other above r1 and print the format with the words that follow
// (r6-r10; integer and string conversions; floating-point ones are in FPRs).
std::string Emulator::panic_message(u32 sp) {
    const auto raw = [&](u32 ea, std::string& out) {
        out.clear();
        const auto pa = bat_physical(ea);
        if (!pa) return false;
        for (u32 i = 0; i < 256U; ++i) {
            u8 c = 0;
            if (sys_->bus().read8(*pa + i, c) != mem::BusStatus::Ok) return false;
            if (c == 0U) return true;
            if (c != '\n' && c != '\t' && (c < 0x20U || c > 0x7EU)) return false;
            out += static_cast<char>(c);
        }
        return true;
    };
    std::string fmt, t;
    for (u32 a = sp; a < sp + 0x800U; a += 4) {
        u32 file = 0, line = 0, f = 0;
        if (!read_guest32(a, file) || !read_guest32(a + 4U, line) || !read_guest32(a + 8U, f)) return {};
        if (line > 100000U || guest_text(file).empty() || !raw(f, fmt) || fmt.find('%') == std::string::npos) continue;
        std::string out;
        u32 next = a + 12U, used = 0;
        for (std::size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%') { out += fmt[i] == '\n' ? std::string("\\n") : std::string(1, fmt[i]); continue; }
            std::size_t j = i + 1;
            while (j < fmt.size() && std::strchr("-+ #0123456789.lhz", fmt[j]) != nullptr) ++j;
            if (j >= fmt.size()) break;
            const char c = fmt[j];
            const std::string spec = fmt.substr(i, j - i);
            i = j;
            if (c == '%') { out += '%'; continue; }
            if (c == 'f' || c == 'g' || c == 'e') { out += "<float>"; continue; }
            u32 v = 0;
            if (used >= 5U || !read_guest32(next, v)) { out += "<?>"; continue; }
            next += 4U;
            ++used;
            char b[48];
            switch (c) {
            case 'd': case 'i': std::snprintf(b, sizeof b, "%d", static_cast<int>(v)); out += b; break;
            case 'u': std::snprintf(b, sizeof b, "%u", v); out += b; break;
            case 'x': std::snprintf(b, sizeof b, "%x", v); out += b; break;
            case 'X': std::snprintf(b, sizeof b, "%X", v); out += b; break;
            case 'p': std::snprintf(b, sizeof b, "%08X", v); out += b; break;
            case 'c': out += static_cast<char>(v & 0x7FU); break;
            case 's': out += raw(v, t) ? t : std::string("<?>"); break;
            default: out += spec + c; break;
            }
        }
        return out;
    }
    return {};
}

std::string Emulator::guest_text(u32 ea) {
    // a NUL-terminated printable string of at least 4 characters, cut at 80
    const auto pa = bat_physical(ea);
    if (!pa) return {};
    std::string out;
    for (u32 i = 0; i < 80U; ++i) {
        u8 c = 0;
        if (sys_->bus().read8(*pa + i, c) != mem::BusStatus::Ok) return {};
        if (c == 0U) break;
        if (c == '\n') { out += "\\n"; continue; }
        if (c == '\t') { out += ' '; continue; }
        if (c < 0x20U || c > 0x7EU) return {};
        out += static_cast<char>(c);
    }
    return out.size() >= 4U ? out : std::string();
}

Emulator::LoopKind Emulator::closed_loop(u32 pc, std::string* words, u32* length) {
    // instructions from pc up to a branch back to pc (at most 16)
    bool loads = false;
    for (u32 n = 0; n < 16U; ++n) {
        u32 ins = 0;
        if (!read_guest32(pc + 4U * n, ins)) return LoopKind::None;
        if (words != nullptr) {
            char b[12];
            std::snprintf(b, sizeof b, " %08X", ins);
            *words += b;
        }
        const u32 op = ins >> 26;
        bool ok = false;
        switch (op) {
        case 32: case 34: case 40: case 42: loads = true; ok = true; break;      // lwz lbz lhz lha
        case 10: case 11: case 14: case 15: case 21: case 24: case 25: case 26: case 27: case 28: case 29: ok = true; break;
        case 16: case 18: ok = (ins & 1U) == 0U; break;                          // branches without link
        case 19: ok = ((ins >> 1) & 0x3FFU) == 150U; break;                       // isync
        case 31: {
            const u32 xo = (ins >> 1) & 0x3FFU;
            if (xo == 23U || xo == 87U || xo == 279U || xo == 343U) { loads = true; ok = true; }
            else ok = xo == 0U || xo == 32U || xo == 28U || xo == 60U || xo == 124U || xo == 444U || xo == 412U ||
                      xo == 316U || xo == 598U;
            break;
        }
        default: break;
        }
        if (!ok) return LoopKind::None;
        if ((op == 16U || op == 18U) && (ins & 2U) == 0U) {
            const s32 disp = op == 18U ? static_cast<s32>((ins & 0x03FFFFFCU) << 6) >> 6 : static_cast<s32>(static_cast<s16>(ins & 0xFFFCU));
            const bool always = op == 18U || ((ins >> 21) & 0x14U) == 0x14U;
            if (pc + 4U * n + static_cast<u32>(disp) == pc && always) {
                if (length != nullptr) *length = n + 1U;
                return loads ? LoopKind::ReadsMemory : LoopKind::NoMemory;
            }
        }
    }
    return LoopKind::None;
}

bool Emulator::halted_by_guest() {
    // A loop proven not to change anything, reading no memory, with external
    // interrupts disabled, can never be left (no decrementer, no external
    // interrupt, nothing it reads can change):
    // the way Nintendo's SDK stops the console after a fatal error (PPCHalt:
    // sync, then nop / li / nop / b back, after OSPanic). Report it with the
    // call chain and any text the registers point to (OSPanic's file and message).
    const auto& st = sys_->cpu().state();
    if ((st.msr & 0x8000U) != 0U) return false;
    // proven by the system's idle-loop check (one iteration changed nothing),
    // not by the loop's shape alone: a delay loop that counts down looks alike
    const u32 loop_pc = sys_->endless_loop_pc();
    if (loop_pc == 0U || st.pc < loop_pc || st.pc >= loop_pc + 4U * sys_->endless_loop_length()) return false;
    char b[96];
    std::snprintf(b, sizeof b, "the game stopped itself: endless loop with interrupts off at %08X", loop_pc);
    error_ = b;
    error_ += " (how Nintendo's SDK halts after a fatal error, e.g. OSPanic). Calls:";
    std::snprintf(b, sizeof b, " %08X", st.lr);
    error_ += b;
    u32 sp = st.gpr[1];
    for (int i = 0; i < 8; ++i) {
        u32 next = 0, lr = 0;
        if (!read_guest32(sp, next) || next == 0U || next == 0xFFFFFFFFU || next <= sp) break;
        if (!read_guest32(next + 4U, lr) || lr == 0U) break;
        std::snprintf(b, sizeof b, " <- %08X", lr);
        error_ += b;
        sp = next;
    }
    // The call into the function that panicked (the first LR saved on the stack is
    // the return address in its caller): rebuild r3-r6 from the instructions just
    // before that call (lis/addi/ori/li, also relative to r2 and r13), which gives
    // OSPanic's file, line and message.
    {
        u32 caller_lr = 0, sp0 = 0;
        if (read_guest32(st.gpr[1], sp0) && sp0 > st.gpr[1] && read_guest32(sp0 + 4U, caller_lr) && caller_lr >= 0x80000000U) {
            const u32 call = caller_lr - 4U;
            std::array<std::optional<u32>, 32> v{};
            v[2] = st.gpr[2];
            v[13] = st.gpr[13];
            for (u32 a = call - 4U * 24U; a < call; a += 4) {
                u32 ins = 0;
                if (!read_guest32(a, ins)) continue;
                const u32 op = ins >> 26, d = (ins >> 21) & 31U, ra = (ins >> 16) & 31U;
                const auto simm = static_cast<u32>(static_cast<s32>(static_cast<s16>(ins & 0xFFFFU)));
                const u32 xo = (ins >> 1) & 0x3FFU, rb = (ins >> 11) & 31U;
                if (op == 15U) v[d] = ra == 0U ? std::optional<u32>((ins & 0xFFFFU) << 16) : (v[ra] ? std::optional<u32>(*v[ra] + ((ins & 0xFFFFU) << 16)) : std::nullopt); // addis
                else if (op == 14U) v[d] = ra == 0U ? std::optional<u32>(simm) : (v[ra] ? std::optional<u32>(*v[ra] + simm) : std::nullopt); // addi
                else if (op == 24U) v[ra] = v[d] ? std::optional<u32>(*v[d] | (ins & 0xFFFFU)) : std::nullopt; // ori rA,rS
                else if (op == 18U || op == 16U) { if ((ins & 1U) != 0U) for (unsigned i = 3; i <= 12; ++i) v[i].reset(); } // a call clobbers r3-r12
                else if (op == 31U && xo == 444U && d == rb) v[ra] = v[d];                 // mr rA,rS
                else if (op >= 20U && op <= 29U) v[ra].reset();                            // rotate/logical: rA
                else if (op == 31U) { v[ra].reset(); v[d].reset(); }                      // X-form: rD or rA
                else if (op == 10U || op == 11U || op == 19U || (op >= 36U && op <= 39U) || (op >= 44U && op <= 47U) ||
                         op >= 48U) {}                                                     // compares, CR ops, stores, FP
                else v[d].reset();                                                         // loads and the rest write rD
            }
            std::snprintf(b, sizeof b, ". Panic call at %08X:", call);
            std::string args = b;
            bool any = false;
            for (unsigned rnum = 3; rnum <= 6; ++rnum) {
                if (!v[rnum]) continue;
                any = true;
                const std::string t = guest_text(*v[rnum]);
                if (!t.empty()) args += " r" + std::to_string(rnum) + "=\"" + t + "\"";
                else {
                    std::snprintf(b, sizeof b, " r%u=%u", rnum, *v[rnum]);
                    args += b;
                }
            }
            if (any) error_ += args;
        }
    }
    std::string texts;
    for (unsigned rnum = 3; rnum <= 10; ++rnum) {
        const std::string t = guest_text(st.gpr[rnum]);
        if (!t.empty()) texts += " r" + std::to_string(rnum) + "=\"" + t + "\"";
    }
    // OSPanic is variadic: its frame keeps copies of r3-r10 (file, line, format and
    // the format's arguments). Strings pointed to from the first three stack frames.
    {
        // the first 2 KiB of the stack above r1 (the frames of PPCHalt, OSPanic and
        // its caller, whatever their sizes)
        const u32 frame = st.gpr[1], end = st.gpr[1] + 0x800U;
        for (u32 a = frame; a < end && texts.size() < 400U; a += 4) {
            u32 v = 0;
            if (!read_guest32(a, v)) break;
            const std::string t = guest_text(v);
            if (!t.empty() && texts.find(t) == std::string::npos) texts += " \"" + t + "\"";
        }
    }
    if (!texts.empty()) error_ += ". Text (from the stack; may include earlier calls):" + texts;
    if (const std::string m = panic_message(st.gpr[1]); !m.empty()) error_ += ". Message: \"" + m + "\"";
    error_ += ". " + video_mode_word();
    // an exception taken on the way (the SDK's unhandled-exception handler also
    // ends in PPCHalt): where it happened
    std::snprintf(b, sizeof b, ". SRR0 %08X SRR1 %08X DAR %08X DSISR %08X", st.srr0, st.srr1, st.dar, st.dsisr);
    error_ += b;
    if (u32 w = 0; read_guest32(st.srr0, w)) {
        std::snprintf(b, sizeof b, " (instruction at SRR0: %08X)", w);
        error_ += b;
    }
    // the last fault the CPU raised, with its registers and call chain at that moment
    if (const auto& f = sys_->cpu().last_fault()) {
        const auto& fs = f->state;
        const char* kind = f->kind == ppc::Exception::DSI ? "DSI" : f->kind == ppc::Exception::ISI ? "ISI"
                         : f->kind == ppc::Exception::Alignment ? "alignment" : "program";
        std::snprintf(b, sizeof b, ". Last %s exception at %08X: LR %08X r1 %08X", kind, fs.pc, fs.lr, fs.gpr[1]);
        error_ += b;
        for (unsigned rn = 3; rn <= 6; ++rn) {
            std::snprintf(b, sizeof b, " r%u %08X", rn, fs.gpr[rn]);
            error_ += b;
        }
        std::snprintf(b, sizeof b, " HID2 %08X DBAT3 %08X/%08X; calls %08X", fs.hid2, fs.dbat[3].upper, fs.dbat[3].lower, fs.lr);
        error_ += b;
        u32 fsp = fs.gpr[1];
        for (int i = 0; i < 6; ++i) {
            u32 next = 0, lr = 0;
            if (!read_guest32(fsp, next) || next <= fsp || !read_guest32(next + 4U, lr) || lr == 0U) break;
            std::snprintf(b, sizeof b, " <- %08X", lr);
            error_ += b;
            fsp = next;
        }
    }
    if (auto* k = sys_->ios(); k != nullptr && !k->history().empty()) error_ += ". Last IOS requests: " + k->history();
    if (const std::string di = sys_->di().recent_commands(); !di.empty()) error_ += ". Last disc commands: " + di;
    return true;
}

std::string Emulator::diagnostics() {
    if (!sys_) return {};
    const auto& st = sys_->cpu().state();
    char b[160];
    std::snprintf(b, sizeof b, "PC %08X LR %08X EE%u", st.pc, st.lr, (st.msr >> 15) & 1U);
    std::string out = b;
    // most sampled PCs (where a waiting game loops)
    std::vector<std::pair<u32, u32>> hot(pc_samples_.begin(), pc_samples_.end());
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& c) { return a.second > c.second; });
    if (pc_sample_total_ != 0U) {
        out += " hot";
        for (std::size_t i = 0; i < hot.size() && i < 3U; ++i) {
            std::snprintf(b, sizeof b, " %08X:%u%%", hot[i].first, hot[i].second * 100U / pc_sample_total_);
            out += b;
        }
    }
    if (!hot.empty()) {
        std::string words;
        const LoopKind k = closed_loop(hot[0].first, &words);
        out += k == LoopKind::None ? "\ncode" : (k == LoopKind::NoMemory ? "\nloop (no reads)" : "\nloop (reads memory)");
        out += words;
    }
    if (!hot.empty()) out += polling_loop(hot[0].first);
    pc_samples_.clear();
    pc_sample_total_ = 0;
    const hw::XfbField f = sys_->vi().xfb_field(false);
    u64 dcr64 = 0;
    (void)sys_->vi().mmio_read(0x02, 2, dcr64); // display configuration (bit 0: enabled)
    const auto dcr = static_cast<unsigned>(dcr64);
    std::snprintf(b, sizeof b, "\nVI %ux%u xfb %08X DCR %04X | PI cause %08X mask %08X", f.width, f.height, f.address,
                  dcr, sys_->pi().causes(), sys_->pi().mask());
    out += b;
    // interrupts raised so far, by source (a game waiting on a flag that an
    // interrupt handler sets shows which source never came)
    static constexpr const char* kNames[] = {"ERR", "RSW", "DI", "SI", "EXI", "AI", "DSP", "MEM",
                                             "VI", "PETOKEN", "PEFINISH", "CP", "DEBUG", "HSP", "IPC"};
    std::string irq;
    for (unsigned i = 0; i < std::size(kNames); ++i)
        if (const u64 n = sys_->pi().raised(i); n != 0U) {
            std::snprintf(b, sizeof b, " %s:%llu", kNames[i], static_cast<unsigned long long>(n));
            irq += b;
        }
    out += "\nirq" + (irq.empty() ? std::string(" none") : irq);
    {
        auto& dsp = sys_->dsp();
        std::snprintf(b, sizeof b, "\nDSP CSR %04X", dsp.csr());
        out += b;
        if (auto* h = dsp.hle_dsp()) {
            if (const auto* u = h->ucode()) {
                std::snprintf(b, sizeof b, " %s (%08X)", u->name(), u->hash());
                out += b;
            }
            out += " cpu->dsp";
            for (const u32 m : h->mails_from_cpu()) { std::snprintf(b, sizeof b, " %08X", m); out += b; }
            out += " dsp->cpu";
            for (const u32 m : h->mails_to_cpu()) { std::snprintf(b, sizeof b, " %08X", m); out += b; }
            std::snprintf(b, sizeof b, " pending %zu", h->pending_mails());
            out += b;
        } else {
            out += " (low-level)";
        }
    }
    out += "\n" + video_mode_word();
    if (auto* k = sys_->ios()) out += "\n" + k->activity();
    return out;
}

// The video mode the IPL / system menu left at 0x800000CC (0 NTSC, 1 PAL, 2 MPAL,
// 5 EURGB60), which VIConfigure compares with the mode a game asks for, and the
// last write to it after boot.
std::string Emulator::video_mode_word() {
    u32 v = 0;
    char b[160];
    if (sys_->bus().read32(0xCC, v) != mem::BusStatus::Ok) return "video mode word unreadable";
    const auto& h = sys_->bus().watch_hit();
    if (h.count == 0U) {
        std::snprintf(b, sizeof b, "video mode word 800000CC=%u (as set at boot)", v);
    } else {
        std::snprintf(b, sizeof b, "video mode word 800000CC=%u, written %llu times since boot, last by %s%s%08X (%u bytes at %08X)", v,
                      static_cast<unsigned long long>(h.count), h.source == mem::AccessSource::Dma ? "DMA" : "the CPU",
                      h.source == mem::AccessSource::Dma ? " while the CPU was at " : " at ", h.pc, h.size, h.address);
    }
    return b;
}

// A loop that waits for memory to change (a branch back over a few instructions
// that read memory): name the words it reads, with their current values, and the
// callers, so the report says what the game is waiting for.
std::string Emulator::polling_loop(u32 hot) {
    const auto& st = sys_->cpu().state();
    u32 start = 0, end = 0;
    for (u32 n = 0; n < 16U && end == 0U; ++n) {
        u32 ins = 0;
        const u32 at = hot + 4U * n;
        if (!read_guest32(at, ins)) return {};
        if ((ins >> 26) == 16U && (ins & 3U) == 0U) {
            const u32 target = at + static_cast<u32>(static_cast<s32>(static_cast<s16>(ins & 0xFFFCU)));
            if (target <= hot && hot - target <= 64U) { start = target; end = at; }
        } else if ((ins >> 26) == 18U && (ins & 3U) == 0U) {
            const u32 target = at + static_cast<u32>(static_cast<s32>((ins & 0x03FFFFFCU) << 6) >> 6);
            if (target <= hot && hot - target <= 64U) { start = target; end = at; }
            else return {};
        }
    }
    if (end == 0U) return {};
    std::string out;
    char b[96];
    for (u32 at = start; at <= end; at += 4U) {
        u32 ins = 0;
        if (!read_guest32(at, ins)) return {};
        const u32 op = ins >> 26;
        unsigned size = 0;
        if (op == 32U || op == 33U) size = 4;
        else if (op == 40U || op == 41U || op == 42U || op == 43U) size = 2;
        else if (op == 34U || op == 35U) size = 1;
        if (size == 0U) continue;
        const u32 ra = (ins >> 16) & 31U;
        const u32 ea = (ra == 0U ? 0U : st.gpr[ra]) + static_cast<u32>(static_cast<s32>(static_cast<s16>(ins & 0xFFFFU)));
        u32 word = 0;
        if (!read_guest32(ea & ~3U, word)) {
            std::snprintf(b, sizeof b, " [%08X]=?", ea);
        } else {
            const u32 shift = (4U - size - (ea & 3U)) * 8U;
            const u32 v = size == 4U ? word : (word >> shift) & (size == 2U ? 0xFFFFU : 0xFFU);
            std::snprintf(b, sizeof b, " [%08X]=%0*X", ea, static_cast<int>(size * 2U), v);
        }
        out += b;
    }
    if (out.empty()) return {};
    std::snprintf(b, sizeof b, "\nwaits %08X-%08X on", start, end);
    out = b + out;
    std::snprintf(b, sizeof b, "; calls %08X", st.lr);
    out += b;
    u32 sp = st.gpr[1];
    for (int i = 0; i < 5; ++i) {
        u32 next = 0, lr = 0;
        if (!read_guest32(sp, next) || next <= sp || !read_guest32(next + 4U, lr) || lr == 0U) break;
        std::snprintf(b, sizeof b, " <- %08X", lr);
        out += b;
        sp = next;
    }
    return out;
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
