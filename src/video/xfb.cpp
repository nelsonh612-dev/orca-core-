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
    auto clamp8 = [](float v) { return static_cast<u8>(std::clamp(static_cast<int>(v), 0, 255)); };
    std::size_t o = 0;
    for (u32 y = 0; y < height; ++y) {
        const u8* p = src.data() + static_cast<std::size_t>(y) * stride;
        for (u32 x = 0; x < width; x += 2, p += 4) {
            const float y1 = static_cast<float>(static_cast<int>(p[0]) - 16), u = static_cast<float>(static_cast<int>(p[1]) - 128);
            const float y2 = static_cast<float>(static_cast<int>(p[2]) - 16), v = static_cast<float>(static_cast<int>(p[3]) - 128);
            for (const float yy : {y1, y2}) {
                rgba[o++] = clamp8(1.164f * yy + 1.596f * v);
                rgba[o++] = clamp8(1.164f * yy - 0.392f * u - 0.813f * v);
                rgba[o++] = clamp8(1.164f * yy + 2.017f * u);
                rgba[o++] = 255;
            }
        }
    }
    return true;
}

} // namespace orca::video
