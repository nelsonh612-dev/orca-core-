#include "orca/hw/hollywood_ipc.hpp"

#include <cstdio>

namespace orca::hw {

using namespace hollywood;

namespace {
constexpr u64 kInterruptDelayTicks = 100U; // Starlet Y1/Y2 -> Broadway interrupt

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}
} // namespace

HollywoodIpc::HollywoodIpc(core::Scheduler& scheduler, ProcessorInterface& pi, u32 cpu_cycles_per_timebase_tick)
    : scheduler_(scheduler), pi_(pi), cycles_per_tick_(cpu_cycles_per_timebase_tick) {}

// The A/V encoder's I2C pins: a pin set as an output is driven to its output
// value; one set as an input is released and the bus pull-up holds it high.
void HollywoodIpc::drive_ave_pins() {
    const bool scl = (gpio_dir_ & GPIO_AVE_SCL) == 0U || (gpio_out_ & GPIO_AVE_SCL) != 0U;
    const bool sda = (gpio_dir_ & GPIO_AVE_SDA) == 0U || (gpio_out_ & GPIO_AVE_SDA) != 0U;
    ave_.update(scl, sda);
}

// Input levels: the disc slot sensor and the two I2C lines (SDA low while the
// encoder pulls it down to acknowledge or to send a 0 bit).
u32 HollywoodIpc::gpio_in() const {
    u32 in = 0;
    if (disc_present_ && disc_present_()) in |= GPIO_SLOT_IN;
    const bool scl = (gpio_dir_ & GPIO_AVE_SCL) == 0U || (gpio_out_ & GPIO_AVE_SCL) != 0U;
    const bool sda = (gpio_dir_ & GPIO_AVE_SDA) == 0U || (gpio_out_ & GPIO_AVE_SDA) != 0U;
    if (scl) in |= GPIO_AVE_SCL;
    if (ave_.sda_line(sda)) in |= GPIO_AVE_SDA;
    return in;
}

bool HollywoodIpc::ready() const noexcept {
    return (ctrl_ & (CTRL_Y1 | CTRL_Y2)) == 0U && (flags_ & IRQ_IPC_BROADWAY) == 0U;
}

void HollywoodIpc::update_interrupt() {
    // Y1/Y2 take part only once they have become visible to the interrupt logic
    const u32 y = ctrl_ & visible_;
    if (((y & CTRL_Y1) != 0U && (ctrl_ & CTRL_IY1) != 0U) || ((y & CTRL_Y2) != 0U && (ctrl_ & CTRL_IY2) != 0U))
        flags_ |= IRQ_IPC_BROADWAY;
    if ((flags_ & mask_) != 0U) pi_.raise(pi::CAUSE_HOLLYWOOD);
    else pi_.lower(pi::CAUSE_HOLLYWOOD);
}

void HollywoodIpc::schedule_visible(u32 bit) {
    visible_ &= ~bit;
    const u64 generation = ++generation_[bit == CTRL_Y1 ? 0 : 1];
    scheduler_.schedule_in(kInterruptDelayTicks * cycles_per_tick_, "Hollywood IPC interrupt",
                           [this, bit, generation](u64, u64) {
                               if (generation_[bit == CTRL_Y1 ? 0 : 1] != generation) return;
                               if ((ctrl_ & bit) != 0U) visible_ |= bit;
                               update_interrupt();
                           });
}

void HollywoodIpc::acknowledge() {
    ctrl_ |= CTRL_Y2;
    schedule_visible(CTRL_Y2);
}

void HollywoodIpc::reply(u32 address) {
    arm_msg_ = address;
    ctrl_ |= CTRL_Y1;
    schedule_visible(CTRL_Y1);
}

bool HollywoodIpc::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size != 4U) return fail("Hollywood registers are 32-bit (access of " + std::to_string(size) + " bytes)");
    switch (offset) {
    case PPCMSG: value = ppc_msg_; return true;
    case PPCCTRL: {
        // Broadway's view: IY2 IY1 X2 Y1 Y2 X1 (bits 5..0)
        value = ctrl_ & 0x3FU;
        return true;
    }
    case ARMMSG: value = arm_msg_; return true;
    case VISOLID: value = visolid_; return true;
    case PPC_IRQFLAG: value = flags_; return true;
    case PPC_IRQMASK: value = mask_; return true;
    case GPIOB_OUT: value = gpio_out_ & kBroadwayPins; return true;
    case GPIOB_DIR: value = gpio_dir_ & kBroadwayPins; return true;
    case GPIOB_IN: value = gpio_in() & kBroadwayPins; return true;
    case RESETS: value = resets_; return true;
    // AHBPROT: which bus masters may reach which devices. Orca does not restrict
    // Broadway's accesses, so every bit reads as "allowed" (Dolphin: 0xFFFFFFFF).
    case AHBPROT: value = 0xFFFFFFFFU; return true;
    case PPCSPEED: value = ppcspeed_; return true;
    // Starlet's GPIO registers: one set of pin data seen through both banks; the
    // owner register (IOS: kBroadwayPins) only decides which bank may change a pin.
    case GPIO_OUT: value = gpio_out_; return true;
    case GPIO_DIR: value = gpio_dir_; return true;
    case GPIO_IN: value = gpio_in(); return true;
    // Undocumented registers that Wii games read: no known function; they read 0 and
    // take writes without effect (the behaviour Dolphin settled on for them).
    case UNK_180: case UNK_1CC: case UNK_1D0: value = 0; return true;
    default:
        return fail("Hollywood register 0x" + hex(kBase + offset) + " is not emulated");
    }
}

bool HollywoodIpc::mmio_write(u32 offset, unsigned size, u64 value64) {
    if (size != 4U) return fail("Hollywood registers are 32-bit (access of " + std::to_string(size) + " bytes)");
    const auto v = static_cast<u32>(value64);
    switch (offset) {
    case PPCMSG: ppc_msg_ = v; return true;
    case PPCCTRL: {
        const u32 old = ctrl_;
        u32 c = old & (CTRL_Y1 | CTRL_Y2);
        if ((v & CTRL_Y1) != 0U) c &= ~CTRL_Y1; // write 1 to clear
        if ((v & CTRL_Y2) != 0U) c &= ~CTRL_Y2;
        visible_ &= c;
        c |= v & (CTRL_X1 | CTRL_X2 | CTRL_IY1 | CTRL_IY2);
        ctrl_ = c;
        if (((v & CTRL_Y1) != 0U && (c & CTRL_IY1) != 0U) || ((v & CTRL_Y2) != 0U && (c & CTRL_IY2) != 0U))
            flags_ |= IRQ_IPC_BROADWAY;
        // X2: Broadway tells Starlet it has taken the reply (libogc and the SDK write it
        // after every reply, and 0x38 when initialising). Starlet's IOS does not wait
        // for it, so, as in Dolphin, it is only kept in the register.
        if ((c & CTRL_X1) != 0U && execute_listener_) execute_listener_(ppc_msg_);
        if (ready_listener_) ready_listener_();
        update_interrupt();
        return true;
    }
    case VISOLID:
        if ((v & 1U) != 0U) return fail("VISOLID: solid-colour video output is not emulated (write " + hex(v) + ")");
        visolid_ = v;
        return true;
    case PPC_IRQFLAG:
        flags_ &= ~v;
        if (ready_listener_) ready_listener_();
        update_interrupt();
        return true;
    case PPC_IRQMASK:
        mask_ = v;
        if (ready_listener_) ready_listener_();
        update_interrupt();
        return true;
    case GPIOB_OUT: {
        const u32 out = (v & kBroadwayPins) | (gpio_out_ & ~kBroadwayPins);
        if ((out & GPIO_DO_EJECT) != 0U) return fail("GPIOB_OUT: disc eject through the GPIO pin is not emulated");
        gpio_out_ = out;
        drive_ave_pins();
        return true;
    }
    case GPIOB_DIR:
        gpio_dir_ = (v & kBroadwayPins) | (gpio_dir_ & ~kBroadwayPins);
        drive_ave_pins();
        return true;
    case GPIOB_IN:
        return true; // input pins: writes have no effect
    case RESETS: {
        const u32 asserted = resets_ & ~v; // active low: 1 -> 0 starts a reset
        if (asserted != 0U)
            return fail("RESETS: asserting reset lines " + hex(asserted) + " is not emulated");
        resets_ = v;
        return true;
    }
    case AHBPROT: return fail("AHBPROT: changing bus access permissions is not emulated (write " + hex(v) + ")");
    case PPCSPEED: ppcspeed_ = v; return true; // bus clock ratio setting: no timing effect modelled
    case GPIO_OUT: {
        const u32 out = (gpio_out_ & kBroadwayPins) | (v & ~kBroadwayPins);
        if ((out & GPIO_DO_EJECT) != 0U) return fail("GPIO_OUT: disc eject through the GPIO pin is not emulated");
        gpio_out_ = out;
        drive_ave_pins();
        return true;
    }
    case GPIO_DIR:
        gpio_dir_ = (gpio_dir_ & kBroadwayPins) | (v & ~kBroadwayPins);
        drive_ave_pins();
        return true;
    case GPIO_IN: return true; // input pins: writes have no effect
    case UNK_180: case UNK_1CC: case UNK_1D0: return true;
    default:
        return fail("Hollywood register 0x" + hex(kBase + offset) + " is not emulated");
    }
}

} // namespace orca::hw
