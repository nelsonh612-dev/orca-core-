// Orca Core — Hollywood registers seen by Broadway (Wii): the IPC mailbox to
// Starlet (where IOS runs), the Broadway interrupt flags/mask, the Broadway
// GPIO bank and the reset register.
//
// Register map (physical 0x0D000000, 32-bit registers; public Wii hardware
// documentation, e.g. WiiBrew "Hardware/Hollywood"):
//   0x000 PPCMSG     request address for Starlet (physical)
//   0x004 PPCCTRL    control: bit0 X1 (execute, set by Broadway, cleared by
//                    Starlet when it takes the request), bit1 Y2 (Starlet
//                    acknowledged; write 1 to clear), bit2 Y1 (Starlet replied;
//                    write 1 to clear), bit3 X2 (reply taken; kept, as in Dolphin), bit4 IY1, bit5 IY2
//                    (interrupt enables for Y1 / Y2)
//   0x008 ARMMSG     reply address from Starlet
//   0x024 VISOLID    solid-colour video output (bit0 enable)
//   0x030 PPCIRQFLAG Broadway interrupt flags (write 1 to clear)
//   0x034 PPCIRQMASK Broadway interrupt mask
//   0x0C0 GPIOB_OUT, 0x0C4 GPIOB_DIR, 0x0C8 GPIOB_IN: the GPIO pins owned by
//                    Broadway (slot LED, slot sensor, sensor bar, eject, A/V
//                    encoder I2C); writes to pins Broadway does not own have no effect
//   0x194 RESETS     reset lines (active low)
// Any pending flag that is also enabled in the mask raises the PI cause
// CAUSE_HOLLYWOOD. Flag bit 30 is the IPC interrupt: it is set when Y1 or Y2
// is set while its IY bit is enabled, and also when Broadway writes 1 to Y1/Y2
// while the matching IY bit is set (WiiBrew; matched by hardware tests
// reported in Dolphin's notes). Starlet's acknowledge and reply interrupts
// become visible about 100 timebase ticks after Starlet sets Y2/Y1 (same source).
//
// Not emulated, each reported as an explicit device error: the other
// registers of the page, a solid-colour video request, the disc eject pin,
// reading the A/V encoder I2C data pin, and asserting any reset line.
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"
#include "orca/memory/bus.hpp"

#include <array>
#include <functional>
#include <string>

namespace orca::hw {

namespace hollywood {
inline constexpr u32 kBase = 0x0D000000U;
inline constexpr u32 PPCMSG = 0x000U;
inline constexpr u32 PPCCTRL = 0x004U;
inline constexpr u32 ARMMSG = 0x008U;
inline constexpr u32 VISOLID = 0x024U;
inline constexpr u32 PPC_IRQFLAG = 0x030U;
inline constexpr u32 PPC_IRQMASK = 0x034U;
inline constexpr u32 GPIOB_OUT = 0x0C0U;
inline constexpr u32 GPIOB_DIR = 0x0C4U;
inline constexpr u32 GPIOB_IN = 0x0C8U;
inline constexpr u32 RESETS = 0x194U;

inline constexpr u32 CTRL_X1 = 0x01U;
inline constexpr u32 CTRL_Y2 = 0x02U;
inline constexpr u32 CTRL_Y1 = 0x04U;
inline constexpr u32 CTRL_X2 = 0x08U;
inline constexpr u32 CTRL_IY1 = 0x10U;
inline constexpr u32 CTRL_IY2 = 0x20U;

inline constexpr u32 IRQ_IPC_BROADWAY = 0x40000000U;

// GPIO pins
inline constexpr u32 GPIO_SLOT_LED = 0x00000020U;
inline constexpr u32 GPIO_SLOT_IN = 0x00000080U;
inline constexpr u32 GPIO_SENSOR_BAR = 0x00000100U;
inline constexpr u32 GPIO_DO_EJECT = 0x00000200U;
inline constexpr u32 GPIO_AVE_SCL = 0x00004000U;
inline constexpr u32 GPIO_AVE_SDA = 0x00008000U;
// pins handed to Broadway by IOS (the GPIO owner register as IOS sets it up)
inline constexpr u32 kBroadwayPins =
    GPIO_SLOT_LED | GPIO_SLOT_IN | GPIO_SENSOR_BAR | GPIO_DO_EJECT | GPIO_AVE_SCL | GPIO_AVE_SDA;
// output pins after IOS start-up (direction register): every output pin of the
// Hollywood GPIO bank (power, fan, DC/DC, drive spin, LED, sensor bar, eject,
// EEPROM chip select/clock/data out, A/V encoder clock/data, debug 0-7)
inline constexpr u32 kGpioOutputsAfterIos = 0x00FFDF3FU;
} // namespace hollywood

class HollywoodIpc final : public mem::MmioDevice {
public:
    HollywoodIpc(core::Scheduler& scheduler, ProcessorInterface& pi, u32 cpu_cycles_per_timebase_tick);

    // --- Starlet (IOS) side -------------------------------------------------
    // Called when Broadway sets X1: `address` is PPCMSG.
    void set_execute_listener(std::function<void(u32 address)> f) { execute_listener_ = std::move(f); }
    // Called after every Broadway write that may let Starlet send the next
    // acknowledge/reply (PPCCTRL, PPCIRQFLAG, PPCIRQMASK).
    void set_ready_listener(std::function<void()> f) { ready_listener_ = std::move(f); }
    // Starlet can signal: neither Y1 nor Y2 is pending and the IPC flag is clear.
    bool ready() const noexcept;
    void clear_execute() noexcept { ctrl_ &= ~hollywood::CTRL_X1; }
    void acknowledge();               // Y2
    void reply(u32 address);          // ARMMSG = address, Y1

    // Disc presence for the slot sensor pin.
    void set_disc_present(std::function<bool()> f) { disc_present_ = std::move(f); }

    u32 ctrl() const noexcept { return ctrl_; }
    u32 irq_flags() const noexcept { return flags_; }
    u32 irq_mask() const noexcept { return mask_; }
    u32 gpio_out() const noexcept { return gpio_out_; }
    u32 ppc_message() const noexcept { return ppc_msg_; }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "Hollywood"; }
    std::string mmio_error() const override { return last_error_; }

private:
    void update_interrupt();
    void schedule_visible(u32 bit);
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    u32 cycles_per_tick_;
    std::function<void(u32)> execute_listener_;
    std::function<void()> ready_listener_;
    std::function<bool()> disc_present_;
    u32 ppc_msg_{0}, arm_msg_{0}, ctrl_{0};
    u32 flags_{0}, mask_{0};
    u32 gpio_out_{0}, gpio_dir_{hollywood::kGpioOutputsAfterIos};
    u32 resets_{0xFFFFFFFFU};
    u32 visolid_{0};
    u32 visible_{0};                 // Y1/Y2 bits the interrupt logic already sees
    std::array<u64, 2> generation_{}; // Y1, Y2: invalidates stale visibility events
    std::string last_error_;
};

} // namespace orca::hw
