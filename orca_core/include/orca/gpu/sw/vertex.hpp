// Orca Core — vertex decoding (CP formats -> floats) and the XF transform unit.
//
// Sources (Dolphin, read as information only): VertexLoader_Position/Normal/
// Color/TextCoord.cpp (component formats, scaling, colour packing),
// SWVertexLoader.cpp (default matrix indices from XF 0x1018/0x1019, absent
// normals take the last loaded ones), TransformUnit.cpp (position, normal,
// lighting and texture-coordinate generation), XFMemory.h (register layouts),
// CPMemory.h (ByteDequant: the fractional shift applies to u8/s8 components
// only when VAT bit 30 is set).
//  - Positions/texture coordinates: integers scaled by 1/2^frac, floats as is.
//  - Normals: u8/s8/u16/s16 divided by 128/64/32768/16384, floats as is.
//  - Colours (big-endian in memory): RGB565, RGB888, RGB888x, RGBA4444,
//    RGBA6666, RGBA8888; fields widened by repeating their top bits.
//  - A position index of all ones (0xFF / 0xFFFF) marks the vertex as skipped.
// Orca choices (documented as unverified): a missing colour is opaque white
// (Dolphin uses a configurable value); a matrix index above 63 stops.
#pragma once
#include "orca/core/bits.hpp"
#include "orca/gpu/cp_state.hpp"
#include "orca/gpu/xf_memory.hpp"
#include "orca/memory/bus.hpp"

#include <array>
#include <span>
#include <string>

namespace orca::gpu::sw {

struct Vec3 {
    float x{0}, y{0}, z{0};
};

struct InputVertex {
    Vec3 pos;
    std::array<Vec3, 3> normal{};         // normal, tangent, binormal
    std::array<u32, 2> color{};           // 0xRRGGBBAA
    std::array<std::array<float, 2>, 8> tex{};
    u8 pos_mtx{0};
    std::array<u8, 8> tex_mtx{};
    bool skip{false};
};

struct OutputVertex {
    Vec3 mv_pos;                          // model-view position
    std::array<float, 4> clip{};          // projected x, y, z, w
    std::array<Vec3, 3> normal{};
    std::array<u32, 2> color{};           // lit colours, 0xRRGGBBAA
    std::array<Vec3, 8> tex{};            // generated coordinates (s, t, q)
};

class VertexDecoder {
public:
    // Decodes one vertex of format `vat` from `data` (exactly one vertex).
    bool decode(const CpState& cp, const XfMemory& xf, mem::Bus& bus, u32 phys_mask, unsigned vat,
                std::span<const u8> data, InputVertex& out, std::string& error);
    // Normals used when the vertex format has none (the last ones loaded).
    std::array<Vec3, 3> last_normal{};
};

// BP texture-coordinate scales (scale_minus_1 + 1 of BP 0x30/0x31 + 2n), s and t.
using TexScale = std::array<std::array<float, 2>, 8>;

// XF transform of one vertex (position, normal, colours/lighting, texgen).
void transform_vertex(const XfMemory& xf, const InputVertex& in, const TexScale& tex_scale, OutputVertex& out);

} // namespace orca::gpu::sw
