// Orca Core — DSP HLE framework, boot ROM and init ucodes. See hle.hpp.
#include "orca/dsp/hle.hpp"

#include <cstdio>

namespace orca::dsp::hle {

u32 ucode_hash(std::span<const u8> data) noexcept {
    u32 crc = 0;
    for (const u8 b : data) {
        crc ^= b;
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}

// ---------------------------------------------------------------- task switch
bool Ucode::task_switch_mail(u32 m) {
    if (!expecting_params_) {
        if (m != kCpuNewUcode) return false;
        expecting_params_ = true;
        param_step_ = 0;
        return true;
    }
    params_[param_step_++] = m;
    if (param_step_ < 10U) return true;
    expecting_params_ = false;
    // params: 0 MRAM dest (DRAM save), 1 its size, 2 DRAM source, 3 IMEM image address in
    // main memory, 4 its size, 5 IMEM destination, 6 start PC, 7 DRAM image address, 8 its size, 9 dest
    const u32 iram_addr = params_[3] & 0x7FFFFFFFU, iram_size = params_[4] & 0xFFFFU;
    if ((params_[1] & 0xFFFFU) != 0U || (params_[8] & 0xFFFFU) != 0U) {
        dsp_.fail("DSP HLE: task switch with a DRAM save or load is not implemented");
        return true;
    }
    std::vector<u8> image(iram_size);
    if (!dsp_.host().ram_read(iram_addr & 0x1FFFFFFFU, image)) {
        dsp_.fail("DSP HLE: the new ucode is outside RAM");
        return true;
    }
    dsp_.switch_to(ucode_hash(image));
    return true;
}

// ---------------------------------------------------------------- boot ROM
namespace {
class RomUcode final : public Ucode {
public:
    using Ucode::Ucode;
    const char* name() const noexcept override { return "boot ROM"; }
    void start() override { dsp_.push_mail(0x8071FEEDU, false); }
    void mail(u32 m) override {
        if (next_ == 0U) {
            if ((m & 0xFFFF0000U) != 0x80F30000U) dsp_.push_mail(0xFEEE0000U | (m & 0xFFFFU), false);
            else next_ = m;
            return;
        }
        const u32 which = next_;
        next_ = 0;
        switch (which) {
        case 0x80F3A001U: ram_address_ = m; break;
        case 0x80F3A002U: length_ = m & 0xFFFFU; break;
        case 0x80F3B002U: break; // DMEM length: the ucodes Orca knows do not need it
        case 0x80F3C002U: break; // IMEM destination
        case 0x80F3D001U: {      // start PC: boot
            std::vector<u8> image(length_);
            if (!dsp_.host().ram_read(ram_address_ & 0x1FFFFFFFU, image)) { dsp_.fail("DSP HLE: ucode upload from outside RAM"); return; }
            dsp_.switch_to(ucode_hash(image));
            return;
        }
        default: {
            char buf[96];
            std::snprintf(buf, sizeof buf, "DSP HLE: unknown boot ROM parameter %08X", which);
            dsp_.fail(buf);
        }
        }
    }
private:
    u32 next_{0}, ram_address_{0}, length_{0};
};

class InitUcode final : public Ucode {
public:
    using Ucode::Ucode;
    const char* name() const noexcept override { return "init"; }
    void start() override { dsp_.push_mail(0x80544348U, false); }
    void mail(u32) override {} // the init ucode ignores mail (it is replaced by a reset)
};
} // namespace

void Ucode::send_resume_if_needed() {
    if (needs_resume_) { needs_resume_ = false; dsp_.push_mail(kMailResume, true); }
}

// ---------------------------------------------------------------- Dsp
Dsp::Dsp(Host& host, bool wii) : host_(host), wii_(wii) {}
Dsp::~Dsp() = default;

void Dsp::reset(bool init_ucode) {
    mails_.clear();
    error_.clear();
    retired_.reset();
    if (init_ucode) ucode_ = std::make_unique<InitUcode>(*this, kHashInit);
    else ucode_ = std::make_unique<RomUcode>(*this, kHashRom);
    ucode_->start();
}

void Dsp::push_mail(u32 mail, bool interrupt, u64 cycles) {
    if (interrupt) {
        if (mails_.empty()) host_.interrupt_cpu(cycles);
        else mails_.front().second = true;
    }
    mails_.emplace_back(mail, false);
}

void Dsp::cpu_write_low(u16 v) {
    cpu_mail_ = (cpu_mail_ & 0xFFFF0000U) | v;
    const u32 m = cpu_mail_;
    cpu_mail_ &= 0x7FFFFFFFU; // taken by the DSP at once
    if (ucode_ && !failed()) ucode_->mail(m);
}

u16 Dsp::cpu_mailbox_high() {
    if (!halted_ && !mails_.empty()) last_mail_ = mails_.front().first;
    return static_cast<u16>(last_mail_ >> 16);
}

u16 Dsp::cpu_mailbox_low() {
    if (!halted_ && !mails_.empty()) {
        last_mail_ = mails_.front().first;
        const bool irq = mails_.front().second;
        mails_.pop_front();
        if (irq) host_.interrupt_cpu(0);
    }
    const auto low = static_cast<u16>(last_mail_);
    last_mail_ &= 0x7FFFFFFFU;
    return low;
}

void Dsp::update() {
    if (ucode_ && !failed() && !halted_) ucode_->update();
}

void Dsp::switch_to(u32 hash) {
    auto next = make_ucode(*this, hash);
    if (!next) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "DSP HLE: ucode with hash %08X is not implemented (low-level emulation with the DSP ROM dumps can run it)", hash);
        fail(buf);
        return;
    }
    mails_.clear();
    retired_ = std::move(ucode_); // the caller may still be running inside it
    ucode_ = std::move(next);
    ucode_->start();
}

std::unique_ptr<Ucode> make_ucode(Dsp&, u32) {
    return nullptr; // AX and other ucodes are added in the following steps
}

} // namespace orca::dsp::hle
