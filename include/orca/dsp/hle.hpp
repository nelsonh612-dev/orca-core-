// Orca Core — high-level emulation of the DSP (HLE): instead of running DSP code,
// the effect of known microcodes ("ucodes") is computed in C++.
//
// Protocol (behaviour of the DSP ROM and of Nintendo's ucodes as Dolphin's DSPHLE
// documents it, used as information; the code is Orca's):
//   * After a reset the boot ROM sends 0x8071FEED and waits for 0x80F3xxxx
//     parameters: A001 = main-memory address of the ucode, A002 = its length in
//     bytes, B002 = DMEM length, C002 = IMEM destination, D001 = start PC (boots).
//     Any other mail is answered with 0xFEEE0000 | (low half).
//   * With CSR bit 11 the reset loads the "init" ucode, which sends 0x80544348.
//   * A running ucode can be told to load another one (task switch): after
//     0xCDD10001 ("new ucode") ten parameter mails describe it, and the new one
//     starts after it has sent DSP_RESUME (0xDCD10001).
//   * Ucodes are recognised by Ector's hash of the IMEM image in main memory
//     (crc ^= byte; crc = rotl(crc, 3)).
// A ucode Orca does not implement stops the emulation with its hash (no guessed
// behaviour); low-level emulation (LLE) with the user's DSP ROM dumps runs it.
//
// Mail to the CPU: a queue; each mail may carry an interrupt, raised when the mail
// becomes the first of the queue. While the DSP is halted (CSR HALT) the CPU sees
// the last mail and no new one. Reading the low half takes the mail and clears
// bit 31 of what the high half reads afterwards.
#pragma once
#include "orca/core/bits.hpp"
#include "orca/dsp/dsp_core.hpp"

#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace orca::dsp::hle {

// Ector's hash, used to identify ucodes.
u32 ucode_hash(std::span<const u8> data) noexcept;

class Host {
public:
    virtual ~Host() = default;
    virtual bool ram_read(u32 physical, std::span<u8> out) = 0;
    virtual bool ram_write(u32 physical, std::span<const u8> in) = 0;
    virtual void interrupt_cpu(u64 cycles_from_now) = 0; // DSPINT
    virtual std::vector<u8>& aram() = 0;                // empty on Wii
    // The DSP core, whose accelerator (and COEF ROM, when dumped) the ucodes use.
    virtual DspCore& core() = 0;
};

class Dsp;

class Ucode {
public:
    explicit Ucode(Dsp& dsp, u32 hash) : dsp_(dsp), hash_(hash) {}
    virtual ~Ucode() = default;
    virtual const char* name() const noexcept = 0;
    virtual void start() = 0;           // just switched to
    virtual void mail(u32 m) = 0;       // a mail from the CPU
    virtual void update() { send_resume_if_needed(); } // every millisecond of emulated time
    u32 hash() const noexcept { return hash_; }

protected:
    // Task switch ("new ucode" received): the next ten mails describe the ucode to
    // load; this one is kept and, when the CPU switches back to it, it sends
    // DSP_RESUME from its next update.
    void begin_task_switch() noexcept { switching_ = true; param_step_ = 0; }
    bool task_switching() const noexcept { return switching_; }
    void task_switch_param(u32 m);
    void send_resume_if_needed();
    Dsp& dsp_;
    u32 hash_;
private:
    bool switching_{false};
    unsigned param_step_{0};
    u32 params_[10]{};
    bool needs_resume_{false};
};

class Dsp {
public:
    Dsp(Host& host, bool wii);
    ~Dsp();

    void reset(bool init_ucode);     // CSR RES (bit 11: init ucode)
    void set_halted(bool halted) noexcept { halted_ = halted; }
    void cpu_write_high(u16 v) noexcept { cpu_mail_ = (cpu_mail_ & 0xFFFFU) | (u32{v} << 16); }
    void cpu_write_low(u16 v);       // completes the mail
    u16 dsp_mailbox_high() const noexcept { return static_cast<u16>(cpu_mail_ >> 16); }
    u16 dsp_mailbox_low() const noexcept { return static_cast<u16>(cpu_mail_); }
    u16 cpu_mailbox_high();
    u16 cpu_mailbox_low();
    void update();

    bool failed() const noexcept { return !error_.empty(); }
    const std::string& error() const noexcept { return error_; }
    const Ucode* ucode() const noexcept { return ucode_.get(); }

    // for ucodes
    Host& host() noexcept { return host_; }
    bool wii() const noexcept { return wii_; }
    void push_mail(u32 mail, bool interrupt, u64 cycles = 0);
    // By hash (unknown -> stop). The ucode being left is kept: switching back to
    // its hash resumes it instead of starting a new one.
    void switch_to(u32 hash);
    void fail(std::string why) { if (error_.empty()) error_ = std::move(why); }
    void warn(std::string w);        // approximations in use (each reported once)
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    bool has_pending_mail() const noexcept { return !mails_.empty(); }

private:
    Host& host_;
    bool wii_;
    bool halted_{true};
    u32 cpu_mail_{0};
    std::deque<std::pair<u32, bool>> mails_; // mail, raise an interrupt when the next becomes first
    u32 last_mail_{0};
    std::unique_ptr<Ucode> ucode_;
    std::unique_ptr<Ucode> last_;             // the ucode left by a task switch
    std::vector<std::unique_ptr<Ucode>> retired_; // kept alive while their code may still run
    std::string error_;
    std::vector<std::string> warnings_;
};

// Known ucodes (hash -> implementation); nullptr when Orca has none.
std::unique_ptr<Ucode> make_ucode(Dsp& dsp, u32 hash);

inline constexpr u32 kMailInit = 0xDCD10000U, kMailResume = 0xDCD10001U, kMailYield = 0xDCD10002U,
                     kMailDone = 0xDCD10003U, kMailSync = 0xDCD10004U, kMailFrameEnd = 0xDCD10005U;
inline constexpr u32 kCpuResume = 0xCDD10000U, kCpuNewUcode = 0xCDD10001U, kCpuReset = 0xCDD10002U,
                     kCpuContinue = 0xCDD10003U;
inline constexpr u32 kHashRom = 0x00000000U, kHashInit = 0x00000001U; // internal ids (not real hashes)

} // namespace orca::dsp::hle
