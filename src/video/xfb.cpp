// Orca Core — XFB to RGBA. See xfb.hpp.
#include "orca/video/xfb.hpp"

#include <algorithm>

namespace orca::video {

bool decode_xfb(std::span<const u8> src, u32 width, u32 height, u32 stride, std::vector<u8>& rgba) {
    if ((width & 1U) != 0U) return false;
    if (height != 0U && width != 0U &&
        static_cast<std::size_t>(height - 1U) * stride + static_cast<std::size_t>(width) * 2U > src.size())
        return false;
    rgba.assign(static_cast<std::size_t>(width) * height * 4U, 0);
    // BT.601 limited range to RGB in 10-bit fixed point (1.164, 1.596, 0.392, 0.813,
    // 2.017 times 1024), rounded down: integer arithmetic, so that the GPU
    // presentation (gles::Backend::present_xfb) gives the same pixels.
    auto clamp8 = [](int v) { return static_cast<u8>(std::clamp(v >> 10, 0, 255)); };
    std::size_t o = 0;
    for (u32 y = 0; y < height; ++y) {
        const u8* p = src.data() + static_cast<std::size_t>(y) * stride;
        for (u32 x = 0; x < width; x += 2, p += 4) {
            const int u = static_cast<int>(p[1]) - 128, v = static_cast<int>(p[3]) - 128;
            for (const int yy : {static_cast<int>(p[0]) - 16, static_cast<int>(p[2]) - 16}) {
                rgba[o++] = clamp8(1192 * yy + 1634 * v);
                rgba[o++] = clamp8(1192 * yy - 401 * u - 833 * v);
                rgba[o++] = clamp8(1192 * yy + 2065 * u);
                rgba[o++] = 255;
            }
        }
    }
    return true;
}

} // namespace orca::video
