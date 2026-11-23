// Orca Core — GPU command execution (state side).
//
// Executes decoded commands: CP/XF/BP loads, indexed XF loads (from the CP
// arrays 12-15 in RAM), display lists (read from RAM) and the BP registers
// that signal the CPU (0x45 draw done with low byte 2, 0x47 token, 0x48 token
// with interrupt; token = value & 0xFFFF). Sources: Dolphin's OpcodeDecoding.cpp,
// BPStructs.cpp and XFStructs.cpp, read as information only.
// GPU time follows Dolphin's estimate, in CPU cycles: NOP / 0x44 / 0x48 = 6,
// 0x01-0x07 and 0x3F = 1, CP = 12, BP = 12, XF = 18 + 6 per word, indexed = 6,
// display list call = 6 (plus its contents), primitive = 12 per vertex + 6.
// Not implemented yet (each stops with a message): drawing primitives, EFB
// copies (BP 0x52), TLUT loads (BP 0x65), TMEM preloads (BP 0x63), and draw-done
// values other than 2.
#pragma once
#include "orca/gpu/bp_memory.hpp"
#include "orca/gpu/command_stream.hpp"
#include "orca/gpu/cp_state.hpp"
#include "orca/gpu/xf_memory.hpp"
#include "orca/hw/pixel_engine.hpp"
#include "orca/memory/bus.hpp"

#include <string>
#include <vector>

namespace orca::gpu {

class Gpu final : public CommandSink {
public:
    // `phys_mask`: 0x03FFFFFF on GameCube, 0x1FFFFFFF on Wii (applied to display
    // list and array addresses, as Dolphin's GetPhysicalAddressMask).
    Gpu(mem::Bus& bus, hw::PixelEngine& pe, u32 phys_mask = 0x03FFFFFFU) : bus_(bus), pe_(pe), phys_mask_(phys_mask) {}

    // Feeds FIFO bytes; incomplete trailing commands are kept for the next call.
    // Returns false once the GPU has stopped (see error()).
    bool feed(std::span<const u8> bytes);
    bool failed() const noexcept { return failed_; }
    const std::string& error() const noexcept { return error_; }
    std::size_t pending_bytes() const noexcept { return pending_.size(); }
    u64 cycles() const noexcept { return cycles_; }

    const CpState& cp() const noexcept { return cp_; }
    const XfMemory& xf() const noexcept { return xf_; }
    const BpMemory& bp() const noexcept { return bp_; }

    // CommandSink
    bool on_cp(u8 reg, u32 value, std::string& error) override;
    bool on_xf(u16 address, std::span<const u8> words_be, std::string& error) override;
    bool on_indexed_xf(u8 array, u16 index, u16 address, u8 size, std::string& error) override;
    bool on_bp(u8 reg, u32 value, std::string& error) override;
    bool on_primitive(Primitive prim, u8 vat, u16 count, u32 vertex_size, std::span<const u8> data,
                      std::string& error) override;
    bool on_display_list(u32 address, u32 size, std::string& error) override;
    bool vertex_size(u8 vat, u32& size, std::string& error) override { return cp_.vertex_size(vat, size, error); }
    void on_nop(u8 opcode) override { cycles_ += (opcode == 0x00U || opcode == 0x44U || opcode == 0x48U) ? 6U : 1U; }

private:
    mem::Bus& bus_;
    hw::PixelEngine& pe_;
    u32 phys_mask_;
    CpState cp_;
    XfMemory xf_;
    BpMemory bp_;
    std::vector<u8> pending_;
    u64 cycles_{0};       // total GPU time
    u64 batch_start_{0};  // cycles_ at the start of the current feed()
    bool failed_{false};
    std::string error_;
};

} // namespace orca::gpu
