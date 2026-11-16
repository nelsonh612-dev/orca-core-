#include "orca/hw/external_interface.hpp"

#include <cstdio>

namespace orca::hw {

namespace {
unsigned popcount3(u32 v) noexcept { return (v & 1U) + ((v >> 1) & 1U) + ((v >> 2) & 1U); }
unsigned cs_index(u32 cs_bits) noexcept { return (cs_bits & 1U) ? 0U : (cs_bits & 2U) ? 1U : 2U; }
} // namespace

ExternalInterface::ExternalInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi, u64 cpu_hz)
    : bus_(bus), scheduler_(scheduler), pi_(pi), cpu_hz_(cpu_hz) {}

bool ExternalInterface::attach(u32 channel, u32 cs, ExiDevice& device) {
    if (channel >= exi::kChannels || cs >= exi::kDevicesPerChannel) return false;
    if (ch_[channel].devices[cs] != nullptr) return false;
    ch_[channel].devices[cs] = &device;
    return true;
}

u32 ExternalInterface::csr(u32 channel) const noexcept {
    const Channel& c = ch_[channel];
    u32 v = c.csr & ~exi::CSR_EXT;
    if (c.devices[0] != nullptr && c.devices[0]->external()) v |= exi::CSR_EXT;
    return v;
}

ExiDevice* ExternalInterface::selected(u32 channel) const noexcept {
    const u32 cs = (ch_[channel].csr & exi::CSR_CS_MASK) >> 7;
    if (cs == 0U) return nullptr;
    return ch_[channel].devices[cs_index(cs)];
}

void ExternalInterface::update_interrupt() {
    bool any = false;
    for (const Channel& c : ch_) {
        const u32 s = c.csr;
        if (((s & exi::CSR_TCINT) != 0U && (s & exi::CSR_TCINTMASK) != 0U) ||
            ((s & exi::CSR_EXIINT) != 0U && (s & exi::CSR_EXIINTMASK) != 0U) ||
            ((s & exi::CSR_EXTINT) != 0U && (s & exi::CSR_EXTINTMASK) != 0U))
            any = true;
    }
    if (any) pi_.raise(pi::CAUSE_EXI); else pi_.lower(pi::CAUSE_EXI);
}

void ExternalInterface::set_device_interrupt(u32 channel) {
    if (channel >= exi::kChannels) return;
    ch_[channel].csr |= exi::CSR_EXIINT;
    update_interrupt();
}

void ExternalInterface::notify_slot_change(u32 channel) {
    if (channel >= exi::kChannels) return;
    ch_[channel].csr |= exi::CSR_EXTINT;
    update_interrupt();
}

bool ExternalInterface::write_csr(u32 channel, u32 v) {
    Channel& c = ch_[channel];
    char buf[128];
    const u32 clk = (v & exi::CSR_CLK_MASK) >> 4;
    if (clk > 5U) {
        std::snprintf(buf, sizeof buf, "EXI%u: CSR.CLK = %u is a reserved clock setting", channel, clk);
        return fail(buf);
    }
    const u32 new_cs = (v & exi::CSR_CS_MASK) >> 7;
    if (popcount3(new_cs) > 1U) {
        std::snprintf(buf, sizeof buf, "EXI%u: more than one CS bit set (CSR=%08X)", channel, v);
        return fail(buf);
    }
    const u32 old_cs = (c.csr & exi::CSR_CS_MASK) >> 7;
    if (new_cs != old_cs && busy(channel)) {
        std::snprintf(buf, sizeof buf, "EXI%u: chip select changed during a transfer", channel);
        return fail(buf);
    }
    const u32 rw_bits = exi::CSR_EXIINTMASK | exi::CSR_TCINTMASK | exi::CSR_CLK_MASK | exi::CSR_CS_MASK |
                        exi::CSR_EXTINTMASK;
    const u32 w1c = exi::CSR_EXIINT | exi::CSR_TCINT | exi::CSR_EXTINT;
    if (new_cs != old_cs) {
        if (old_cs != 0U) {
            if (ExiDevice* d = c.devices[cs_index(old_cs)]) d->deselect();
        }
        if (new_cs != 0U) {
            if (ExiDevice* d = c.devices[cs_index(new_cs)]) d->select();
        }
    }
    const bool romdis_rising = channel == 0U && (v & exi::CSR_ROMDIS) != 0U && (c.csr & exi::CSR_ROMDIS) == 0U;
    c.csr = (c.csr & ~rw_bits) | (v & rw_bits);
    c.csr &= ~(v & w1c);
    if (romdis_rising) {
        c.csr |= exi::CSR_ROMDIS; // sticky until reset
        for (ExiDevice* d : c.devices)
            if (d != nullptr) d->on_romdis();
    }
    update_interrupt();
    return true;
}

bool ExternalInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size != 4U) return fail("EXI: only 32-bit accesses are implemented");
    const u32 channel = offset / exi::kChannelStride;
    const u32 reg = offset % exi::kChannelStride;
    if (channel >= exi::kChannels) return fail("EXI: register not implemented");
    const Channel& c = ch_[channel];
    switch (reg) {
    case 0x00: value = csr(channel); return true;
    case 0x04: value = c.mar; return true;
    case 0x08: value = c.length; return true;
    case 0x0C: value = c.cr; return true;
    case 0x10: value = c.data; return true;
    default: return fail("EXI: register not implemented");
    }
}

bool ExternalInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size != 4U) return fail("EXI: only 32-bit accesses are implemented");
    const u32 channel = offset / exi::kChannelStride;
    const u32 reg = offset % exi::kChannelStride;
    if (channel >= exi::kChannels) return fail("EXI: register not implemented");
    Channel& c = ch_[channel];
    const auto v = static_cast<u32>(value);
    switch (reg) {
    case 0x00: return write_csr(channel, v);
    case 0x04: c.mar = v & ~0x1FU; return true;
    case 0x08: c.length = v & ~0x1FU; return true;
    case 0x0C:
        if (busy(channel)) return fail("EXI: CR written while a transfer is in progress");
        c.cr = v & (exi::CR_TSTART | exi::CR_DMA | exi::CR_RW_MASK | exi::CR_TLEN_MASK);
        if ((c.cr & exi::CR_TSTART) != 0U && !start(channel)) {
            c.cr &= ~exi::CR_TSTART;
            return false;
        }
        return true;
    case 0x10: c.data = v; return true;
    default: return fail("EXI: register not implemented");
    }
}

bool ExternalInterface::start(u32 channel) {
    Channel& c = ch_[channel];
    char buf[160];
    const u32 cs = (c.csr & exi::CSR_CS_MASK) >> 7;
    if (cs == 0U) {
        std::snprintf(buf, sizeof buf, "EXI%u: transfer started with no device selected", channel);
        return fail(buf);
    }
    ExiDevice* dev = c.devices[cs_index(cs)];
    if (dev == nullptr) {
        std::snprintf(buf, sizeof buf, "EXI%u: transfer to CS%u, where no device is attached", channel, cs_index(cs));
        return fail(buf);
    }
    const u32 rw = (c.cr & exi::CR_RW_MASK) >> 2;
    const bool dma = (c.cr & exi::CR_DMA) != 0U;
    if (rw == exi::RW_READWRITE) {
        std::snprintf(buf, sizeof buf, "EXI%u: %s read+write transfers are not implemented (%s)", channel,
                      dma ? "DMA (invalid per documentation)" : "immediate", dev->exi_name());
        return fail(buf);
    }
    if (rw == 3U) {
        std::snprintf(buf, sizeof buf, "EXI%u: CR.RW = 11 is undefined", channel);
        return fail(buf);
    }
    std::string error;
    u32 bytes = 0;
    c.pending_has_data = false;
    c.pending_dma.clear();
    if (!dma) {
        bytes = ((c.cr & exi::CR_TLEN_MASK) >> 4) + 1U;
        u8 wire[4]{};
        if (rw == exi::RW_WRITE) {
            for (u32 i = 0; i < bytes; ++i) wire[i] = static_cast<u8>(c.data >> (24U - 8U * i));
            if (!dev->write(std::span<const u8>(wire, bytes), error)) return fail(dev->exi_name() + (": " + error));
        } else {
            if (!dev->read(std::span<u8>(wire, bytes), error)) return fail(dev->exi_name() + (": " + error));
            u32 d = c.data;
            for (u32 i = 0; i < bytes; ++i) {
                const u32 shift = 24U - 8U * i;
                d = (d & ~(0xFFU << shift)) | (static_cast<u32>(wire[i]) << shift);
            }
            c.pending_data = d;
            c.pending_has_data = true;
        }
    } else {
        bytes = c.length;
        if (bytes == 0U) {
            std::snprintf(buf, sizeof buf, "EXI%u: DMA with LENGTH = 0 (behaviour undocumented)", channel);
            return fail(buf);
        }
        if (!bus_.is_ram(c.mar, bytes)) {
            std::snprintf(buf, sizeof buf, "EXI%u: DMA range %08X+%X is not RAM", channel, c.mar, bytes);
            return fail(buf);
        }
        std::vector<u8> data(bytes);
        if (rw == exi::RW_WRITE) {
            (void)bus_.dma_read(c.mar, data);
            if (!dev->dma_write(data, error)) return fail(dev->exi_name() + (": " + error));
        } else {
            if (!dev->dma_read(data, error)) return fail(dev->exi_name() + (": " + error));
            c.pending_dma = std::move(data);
        }
    }
    const u64 clk = exi::clock_hz((c.csr & exi::CSR_CLK_MASK) >> 4);
    const u64 bits = static_cast<u64>(bytes) * 8ULL;
    u64 cycles = (bits * cpu_hz_ + clk - 1ULL) / clk;
    if (dma) cycles = dev->dma_cycles(rw == exi::RW_WRITE, bytes, cycles);
    scheduler_.schedule_in(cycles, "EXI transfer", [this, channel](u64, u64) { complete(channel); });
    return true;
}

void ExternalInterface::complete(u32 channel) {
    Channel& c = ch_[channel];
    if (c.pending_has_data) c.data = c.pending_data;
    if (!c.pending_dma.empty()) (void)bus_.dma_write(c.mar, c.pending_dma);
    c.pending_has_data = false;
    c.pending_dma.clear();
    c.cr &= ~exi::CR_TSTART;
    c.csr |= exi::CSR_TCINT;
    update_interrupt();
}

} // namespace orca::hw
