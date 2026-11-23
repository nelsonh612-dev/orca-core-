// Orca Core — GPU command stream parser. See command_stream.hpp for sources.
#include "orca/gpu/command_stream.hpp"

#include <cstdio>

namespace orca::gpu {
namespace {

u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }
u32 be24(const u8* p) { return (static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | p[2]; }
u32 be32(const u8* p) { return (be16(p) << 16) | be16(p + 2); }

std::string hex8(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

} // namespace

ParseResult parse_commands(std::span<const u8> bytes, CommandSink& sink, bool in_display_list) {
    ParseResult r;
    const u8* const base = bytes.data();
    const std::size_t n = bytes.size();
    std::size_t pos = 0;
    auto fail = [&](std::string msg) {
        r.consumed = static_cast<u32>(pos);
        r.error = true;
        r.message = std::move(msg);
        return r;
    };
    while (pos < n) {
        const u8* p = base + pos;
        const std::size_t avail = n - pos;
        const u8 opc = p[0];
        std::size_t len = 0;
        std::string err;
        if (opc == op::NOP || (opc >= 0x01U && opc <= 0x07U) || opc == 0x3FU || opc == op::METRICS ||
            opc == op::INVL_VC) {
            sink.on_nop(opc);
            len = 1;
        } else if (opc == op::LOAD_CP) {
            if (avail < 6) break;
            if (!sink.on_cp(p[1], be32(p + 2), err)) return fail(err);
            len = 6;
        } else if (opc == op::LOAD_XF) {
            if (avail < 5) break;
            const u32 w = be32(p + 1);
            if ((w >> 16) > 0xFU) return fail("XF load with count field " + hex8(w >> 16) + " (above 15) is not handled");
            const u32 words = ((w >> 16) & 0xFU) + 1U;
            if (avail < 5U + words * 4U) break;
            if (!sink.on_xf(static_cast<u16>(w & 0xFFFFU), std::span<const u8>(p + 5, words * 4U), err)) return fail(err);
            len = 5U + words * 4U;
        } else if (opc == op::LOAD_INDX_A || opc == op::LOAD_INDX_B || opc == op::LOAD_INDX_C || opc == op::LOAD_INDX_D) {
            if (avail < 5) break;
            const u32 w = be32(p + 1);
            const u8 array = static_cast<u8>(opc / 8U + 8U);
            if (!sink.on_indexed_xf(array, static_cast<u16>(w >> 16), static_cast<u16>(w & 0xFFFU),
                                    static_cast<u8>(((w >> 12) & 0xFU) + 1U), err))
                return fail(err);
            len = 5;
        } else if (opc == op::CALL_DL) {
            if (avail < 9) break;
            if (in_display_list) return fail("display list called from inside a display list is not handled");
            if (!sink.on_display_list(be32(p + 1) & ~31U, be32(p + 5) & ~31U, err)) return fail(err);
            len = 9;
        } else if (opc == op::LOAD_BP) {
            if (avail < 5) break;
            if (!sink.on_bp(p[1], be24(p + 2), err)) return fail(err);
            len = 5;
        } else if (opc >= op::PRIMITIVE_FIRST && opc <= op::PRIMITIVE_LAST) {
            if (avail < 3) break;
            const u8 vat = opc & 7U;
            u32 vsize = 0;
            if (!sink.vertex_size(vat, vsize, err)) return fail(err);
            const u32 count = be16(p + 1);
            const std::size_t data = static_cast<std::size_t>(count) * vsize;
            if (avail < 3U + data) break;
            if (!sink.on_primitive(static_cast<Primitive>((opc >> 3) & 7U), vat, static_cast<u16>(count), vsize,
                                   std::span<const u8>(p + 3, data), err))
                return fail(err);
            len = 3U + data;
        } else {
            char b[64];
            std::snprintf(b, sizeof b, "unknown GPU command opcode %02X", opc);
            return fail(b);
        }
        pos += len;
    }
    r.consumed = static_cast<u32>(pos);
    return r;
}

} // namespace orca::gpu
