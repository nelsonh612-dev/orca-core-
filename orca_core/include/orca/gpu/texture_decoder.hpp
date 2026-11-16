// Orca Core — GameCube/Wii texture formats decoded to RGBA8.
//
// Source of the block layouts and conversions: Dolphin's
// VideoCommon/TextureDecoder_Generic.cpp and TextureDecoder_Util.h (read as
// information only). Bit expansion to 8 bits (3->8 = v<<5|v<<2|v>>1,
// 4->8 = v<<4|v, 5->8 = v<<3|v>>2, 6->8 = v<<2|v>>4) from Metaforce's GX
// texture converter; Dolphin's colour code agrees for 4, 5 and 6 bits.
// Texels are stored in tiles, row by row, tiles left to right:
//   I4, C4, CMPR 8x8 (4 bits)   I8, IA4, C8 8x4 (8 bits)
//   IA8, RGB565, RGB5A3, C14X2 4x4 (16 bits, big-endian)
//   RGBA8 4x4 in 64 bytes: 16 A,R pairs then 16 G,B pairs
// I4/I8 give R=G=B=A=I. IA4: high nibble alpha. IA8: first byte alpha.
// RGB5A3: bit 15 set = RGB555 (alpha 255), clear = A3 R4 G4 B4.
// CMPR: 8x8 tiles of four DXT1 blocks (top-left, top-right, bottom-left,
// bottom-right), colours big-endian, 2 bits per texel from the high bits;
// c1 > c2: c2' = (c2*3 + c1*5) >> 3, c3' = (c1*3 + c2*5) >> 3 per channel;
// otherwise c2' = c3' = midpoint (rounded towards c1) and c3' has alpha 0.
// Palettes (TLUT, big-endian 16-bit entries): IA8 (first byte alpha), RGB565,
// RGB5A3. C14X2 uses the low 14 bits of each texel as the index.
// decode_texel is Orca's own per-texel addressing of the same layouts; tests
// check it against the tile decoder for every format.
#pragma once
#include "orca/core/bits.hpp"

#include <span>
#include <string>
#include <vector>

namespace orca::gpu {

enum class TexFormat : u8 { I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA8 = 6, C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14 };
enum class TlutFormat : u8 { IA8 = 0, RGB565 = 1, RGB5A3 = 2 };

bool tex_format_valid(u32 format);
bool tex_format_paletted(TexFormat f);
u32 tex_block_width(TexFormat f);
u32 tex_block_height(TexFormat f);
// Bytes taken by a width x height image (whole tiles).
u32 tex_size_bytes(TexFormat f, u32 width, u32 height);

// Decodes a whole image into width*height RGBA8 texels (bytes R, G, B, A).
// `src` must hold tex_size_bytes(); `tlut` the palette for C4/C8/C14X2.
bool decode_texture(TexFormat f, std::span<const u8> src, u32 width, u32 height, std::span<const u8> tlut,
                    TlutFormat tlut_format, std::vector<u8>& out, std::string& error);

// Decodes one texel (s, t) of an image `width` texels wide.
bool decode_texel(TexFormat f, std::span<const u8> src, u32 s, u32 t, u32 width, std::span<const u8> tlut,
                  TlutFormat tlut_format, u8 rgba[4], std::string& error);

} // namespace orca::gpu
