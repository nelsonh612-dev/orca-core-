// Orca Core — XFB (external frame buffer) to RGBA conversion for video output.
//
// The XFB is YUV 4:2:2 in "Y U Y V" byte order, two pixels per 4 bytes. The conversion is the
// inverse BT.601 used by Dolphin's TexDecoder_DecodeXFB (TextureDecoder_Common.cpp, read as
// information only): R = 1.164 (Y - 16) + 1.596 (V - 128), G = 1.164 (Y - 16) - 0.392 (U - 128)
// - 0.813 (V - 128), B = 1.164 (Y - 16) + 2.017 (U - 128), computed in float, truncated toward
// zero and clamped to 0..255. Dolphin notes that the real VI does not necessarily produce RGB
// this way (pending hardware verification); it is only used to show the image.
#pragma once
#include "orca/core/bits.hpp"

#include <span>
#include <vector>

namespace orca::video {

// Converts `height` lines of `width` pixels (width even), `stride` bytes apart, into RGBA8
// (R, G, B, 255 per pixel). Returns false if `src` is too short.
bool decode_xfb(std::span<const u8> src, u32 width, u32 height, u32 stride, std::vector<u8>& rgba);

} // namespace orca::video
