// Orca Core — GPU command stream parser (the FIFO / display-list byte format).
//
// Source of the format: Dolphin's VideoCommon/OpcodeDecoding.h and
// OpcodeDecoding.cpp, read as information only (no code copied). All values
// in the stream are big-endian.
//
//   0x00              NOP                       1 byte
//   0x08 rr vvvvvvvv  load CP register rr       6 bytes
//   0x10 nnnn aaaa .. load XF: n+1 words at a   5 + 4*(n+1) bytes (n = bits 16-19)
//   0x20/28/30/38     indexed XF load A/B/C/D   5 bytes: index<<16 | (size-1)<<12 | addr
//                     (arrays 12-15; address mask 0xFFF as in Dolphin, unverified)
//   0x40 aaaa ssss    call display list         9 bytes; address and size & ~31
//   0x44              GX "metrics"              1 byte, no effect
//   0x48              invalidate vertex cache   1 byte, no effect
//   0x61 rr vvvvvv    load BP register rr       5 bytes (24-bit value)
//   0x80-0xBF nnnn .. primitive (bits 3-6) using VAT (bits 0-2), n vertices
//                     of the size given by the current VCD/VAT
//
// Opcodes 0x01-0x07 and 0x3F are accepted as one-byte no-ops: Dolphin notes
// that hardware testing showed they do nothing. Every other opcode is reported
// as an error (on hardware some of them hang the GPU). Two cases Dolphin only
// asserts on are also reported as errors instead of guessed: an XF load whose
// count field has bits above bit 19 set, and a display list called from
// inside a display list.
#pragma once
#include "orca/core/bits.hpp"

#include <span>
#include <string>

namespace orca::gpu {

namespace op {
inline constexpr u8 NOP = 0x00, LOAD_CP = 0x08, LOAD_XF = 0x10, LOAD_INDX_A = 0x20, LOAD_INDX_B = 0x28,
                    LOAD_INDX_C = 0x30, LOAD_INDX_D = 0x38, CALL_DL = 0x40, METRICS = 0x44, INVL_VC = 0x48,
                    LOAD_BP = 0x61, PRIMITIVE_FIRST = 0x80, PRIMITIVE_LAST = 0xBF;
}

enum class Primitive : u8 {
    Quads = 0,
    Quads2 = 1, // 0x88, non-standard
    Triangles = 2,
    TriangleStrip = 3,
    TriangleFan = 4,
    Lines = 5,
    LineStrip = 6,
    Points = 7,
};

// Receives each decoded command. Returning false stops parsing with an error
// (the callback fills `error`).
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual bool on_cp(u8 reg, u32 value, std::string& error) = 0;
    virtual bool on_xf(u16 address, std::span<const u8> words_be, std::string& error) = 0;
    virtual bool on_indexed_xf(u8 array, u16 index, u16 address, u8 size, std::string& error) = 0;
    virtual bool on_bp(u8 reg, u32 value, std::string& error) = 0;
    virtual bool on_primitive(Primitive prim, u8 vat, u16 count, u32 vertex_size, std::span<const u8> data,
                              std::string& error) = 0;
    virtual bool on_display_list(u32 address, u32 size, std::string& error) = 0;
    // Size in bytes of one vertex for `vat` under the current CP state; false
    // (with `error`) when the format cannot be sized.
    virtual bool vertex_size(u8 vat, u32& size, std::string& error) = 0;
    // One-byte commands with no effect (NOP, 0x01-0x07, 0x3F, 0x44, 0x48).
    virtual void on_nop(u8 opcode) { (void)opcode; }
};

struct ParseResult {
    u32 consumed{0};   // bytes of complete commands handled
    bool error{false}; // true: stopped at a command that could not be handled
    std::string message;
};

// Parses as many complete commands as `bytes` holds. A command cut off at the
// end is left unconsumed so the caller can retry once more bytes arrive.
ParseResult parse_commands(std::span<const u8> bytes, CommandSink& sink, bool in_display_list);

} // namespace orca::gpu
