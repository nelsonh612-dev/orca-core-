// Orca Core — GPU command execution. See gpu.hpp.
#include "orca/gpu/gpu.hpp"
#include "orca/core/host_timer.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace orca::gpu {

bool Gpu::feed(std::span<const u8> bytes) {
    if (failed_) return false;
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    batch_start_ = cycles_;
    const ParseResult r = parse_commands(pending_, *this, false);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(r.consumed));
    if (r.error) {
        failed_ = true;
        error_ = "GPU: " + r.message;
        return false;
    }
    return true;
}

void Gpu::set_gpu_copies(GpuCopies copies) {
    // ranges encoded by the previous ones are read back while those still exist
    bus_.materialize_all();
    *gpu_copies_alive_ = false;
    gpu_copies_alive_ = std::make_shared<bool>(true);
    gpu_copies_ = std::move(copies);
}

namespace {
// Owns one encoded XFB until it is read back; dropped unread (a newer copy
// replaced the range) it releases the GPU buffer.
struct PendingXfb {
    std::function<void(u32, u8*)> read;
    std::function<void(u32)> drop;
    std::shared_ptr<bool> alive;
    u32 handle{0};
    bool done{false};
    PendingXfb() = default;
    PendingXfb(const PendingXfb&) = delete;
    PendingXfb& operator=(const PendingXfb&) = delete;
    ~PendingXfb() {
        if (!done && *alive) drop(handle);
    }
};
} // namespace

// The copy on the GPU backend, when it can do all of it: `done` false means the
// CPU path must run (nothing was changed). Only display copies (and their
// clear) go here; a texture copy is read by the GPU's own texture unit later
// and keeps the CPU path for now.
bool Gpu::gpu_copy(u32 trigger, std::string& error, bool& done) {
    done = false;
    sw::CopyArea area;
    if (!sw::copy_area(bp_, area, error)) return false;
    if (area.ignored || (trigger & 0x4000U) == 0U) return true;
    const bool clears = (trigger & 0x800U) != 0U;
    const sw::ClearPlan cp = clears ? sw::clear_plan(bp_, area) : sw::ClearPlan{};
    auto fallback = [&](std::string why) {
        ++cpu_fallback_copies_;
        last_fallback_ = std::move(why);
        return true;
    };
    if (clears && !cp.whole) return fallback("RGBA6 clear of colour or alpha only");
    sw::CopyResult res;
    if ((bp_.reg(bp::EFB_STRIDE) << 5) != 0U) {
        sw::XfbPlan plan;
        if (!sw::xfb_plan(trigger, bp_, area, phys_mask_, plan, error)) return false;
        const u32 row = plan.width * 2U;
        const u64 span = static_cast<u64>(plan.lines - 1U) * plan.stride + row;
        if (plan.stride < row || static_cast<u64>(plan.dest) + span > static_cast<u64>(phys_mask_) + 1U)
            return fallback("XFB rows that overlap or wrap around RAM");
        u32 handle = 0;
        std::string why;
        {
            const core::HostTimer t(copy_ns_.encode);
            if (!gpu_copies_.encode_xfb(plan, handle, why)) return fallback(why);
        }
        auto p = std::make_shared<PendingXfb>();
        p->read = gpu_copies_.read_xfb;
        p->drop = gpu_copies_.drop_xfb;
        p->alive = gpu_copies_alive_;
        p->handle = handle;
        const u32 stride = plan.stride;
        const bool dense = stride == row;
        auto fill = [p, this](u8* dst) {
            if (p->done || !*p->alive) return;
            p->done = true;
            const core::HostTimer t(copy_ns_.readback);
            p->read(p->handle, dst);
        };
        if (!bus_.set_lazy(plan.dest, static_cast<u32>(span), fill, dense)) {
            // not inside one RAM region: the rows go through the bus now
            std::vector<u8> bytes(static_cast<std::size_t>(span));
            fill(bytes.data());
            for (u32 y = 0; y < plan.lines; ++y) {
                const u32 addr = (plan.dest + y * stride) & phys_mask_;
                const auto rowspan = std::span<const u8>(bytes).subspan(static_cast<std::size_t>(y) * stride, row);
                if (bus_.dma_write(addr, rowspan) != mem::BusStatus::Ok) {
                    char b[96];
                    std::snprintf(b, sizeof b, "XFB copy writes %u bytes at %08X outside RAM", row, addr);
                    error = b;
                    return false;
                }
            }
        }
        res.copied = true;
        res.xfb = true;
        res.address = plan.dest;
        res.stride = stride;
        res.width = plan.width;
        res.height = plan.lines;
        res.bytes = row * plan.lines;
        ++gpu_xfb_copies_;
    }
    if (clears) {
        std::string why;
        if (!gpu_copies_.clear(area, cp, why)) {
            error = "GPU EFB clear after a display copy failed: " + why;
            return false;
        }
        res.cleared = true;
    }
    last_copy_ = res;
    done = true;
    return true;
}

bool Gpu::on_cp(u8 reg, u32 value, std::string& error) {
    cycles_ += 12;
    return cp_.load(reg, value, error);
}

bool Gpu::on_xf(u16 address, std::span<const u8> words_be, std::string& error) {
    cycles_ += 18U + 6U * (words_be.size() / 4U);
    return xf_.load(address, words_be, error);
}

bool Gpu::on_indexed_xf(u8 array, u16 index, u16 address, u8 size, std::string& error) {
    cycles_ += 6;
    std::vector<u8> words(static_cast<std::size_t>(size) * 4U);
    const u32 src = (cp_.array_base[array] + cp_.array_stride[array] * index) & phys_mask_;
    if (bus_.dma_read(src, words) != mem::BusStatus::Ok) {
        char b[96];
        std::snprintf(b, sizeof b, "indexed XF load reads %u bytes at %08X outside RAM", size * 4U, src);
        error = b;
        return false;
    }
    return xf_.load(address, words, error);
}

bool Gpu::on_bp(u8 reg, u32 value, std::string& error) {
    cycles_ += 12;
    const u32 v = bp_.load(reg, value);
    const u64 into = cycles_ - batch_start_;
    char b[96];
    switch (reg) {
    case bp::SET_DRAW_DONE:
        if ((v & 0xFFU) != 0x02U) {
            std::snprintf(b, sizeof b, "BP 45 (draw done) with value %06X is not handled", v);
            error = b;
            return false;
        }
        pe_.set_finish(into);
        return true;
    case bp::CLEAR_BBOX1: case bp::CLEAR_BBOX2: {
        if (!sync_stats_before_cpu_write(true, error)) return false;
        PixelStats& s = pe_.stats();
        s.set_bbox_bp(reg == bp::CLEAR_BBOX2, v);
        s.bbox_armed = true;
        ++s.epoch;
        return true;
    }
    case bp::CLEAR_PIXEL_PERF: {
        if (!sync_stats_before_cpu_write(false, error)) return false;
        PixelStats& s = pe_.stats();
        s.quads = {};
        s.quads_armed = true;
        ++s.epoch;
        return true;
    }
    case bp::PE_TOKEN: pe_.set_token(static_cast<u16>(v & 0xFFFFU), false, into); return true;
    case bp::PE_TOKEN_INT: pe_.set_token(static_cast<u16>(v & 0xFFFFU), true, into); return true;
    case bp::TRIGGER_EFB_COPY: {
        if (gpu_copies_.encode_xfb && renderer_.sink() != nullptr) {
            bool done = false;
            if (!gpu_copy(v, error, done)) return false;
            if (done) {
                ++copies_;
                return true;
            }
        }
        sw::CopyResult res;
        const sw::EfbRect area = sw::copy_source_area(bp_);
        if (renderer_.sink() != nullptr && download_) {
            // only the plane the copy reads, unless it also clears: the cleared
            // area goes back to the backend whole (colour and depth)
            const bool clears = (v & 0x800U) != 0U;
            const u32 fmt = bp_.reg(bp::ZCOMPARE) & 7U;
            const bool depth_copy = fmt == 3U;
            const bool all = clears || (fmt != 0U && fmt != 3U && fmt != 1U);
            const core::HostTimer t(copy_ns_.readback);
            download_(renderer_.efb(), area, all || !depth_copy, all || depth_copy);
        }
        {
            const core::HostTimer t(copy_ns_.encode);
            if (!sw::efb_copy(v, bp_, renderer_.efb(), bus_, phys_mask_, res, error)) return false;
        }
        if (res.cleared && renderer_.sink() != nullptr && upload_) {
            const core::HostTimer t(copy_ns_.upload);
            upload_(renderer_.efb(), area);
        }
        last_copy_ = res;
        ++copies_;
        return true;
    }
    case bp::LOAD_TLUT1: {
        const u32 dst = (v & 0x3FFU) << 9, len = ((v >> 10) & 0x7FFU) * 32U;
        u32 src = bp_.reg(bp::LOAD_TLUT0) << 5;
        if (phys_mask_ == 0x03FFFFFFU) src &= 0x01FFFFFFU;
        if (bus_.dma_read(src, std::span<u8>(tmem_).subspan(dst, len)) != mem::BusStatus::Ok) {
            std::snprintf(b, sizeof b, "TLUT load of %u bytes from %08X is outside RAM", len, src);
            error = b;
            return false;
        }
        return true;
    }
    case bp::PRELOAD_MODE: {
        if (v == 0U) return true; // GX_TexModeSync
        constexpr u32 kLine = 32U, kTmem = 1U << 20;
        const u32 src = (bp_.reg(bp::PRELOAD_ADDR) << 5) & phys_mask_;
        u32 even = bp_.reg(bp::PRELOAD_TMEM_EVEN) * kLine;
        const u32 count = v & 0x7FFFU, type = (v >> 15) & 3U;
        auto fail = [&](u32 at, u32 len) {
            std::snprintf(b, sizeof b, "TMEM preload reads %u bytes at %08X outside RAM", len, at);
            error = b;
            return false;
        };
        if (type != 3U) {
            if (even >= kTmem) return true;
            const u32 len = std::min(count * kLine, kTmem - even);
            if (bus_.dma_read(src, std::span<u8>(tmem_).subspan(even, len)) != mem::BusStatus::Ok) return fail(src, len);
            return true;
        }
        // RGBA8: AR lines to the even bank, GB lines to the odd bank, alternating in RAM
        u32 odd = bp_.reg(bp::PRELOAD_TMEM_ODD) * kLine, read = 0;
        for (u32 i = 0; i < count; ++i) {
            if (even + kLine > kTmem || odd + kLine > kTmem) break;
            if (bus_.dma_read(src + read, std::span<u8>(tmem_).subspan(even, kLine)) != mem::BusStatus::Ok) return fail(src + read, kLine);
            if (bus_.dma_read(src + read + kLine, std::span<u8>(tmem_).subspan(odd, kLine)) != mem::BusStatus::Ok) return fail(src + read + kLine, kLine);
            even += kLine;
            odd += kLine;
            read += 2U * kLine;
        }
        return true;
    }
    case 0xE0: case 0xE2: case 0xE4: case 0xE6: case 0xE1: case 0xE3: case 0xE5: case 0xE7: {
        const unsigned num = (reg >> 1) & 3U;
        auto& dst = ((v >> 23) & 1U) != 0U ? tev_.konst[num] : tev_.reg[num];
        auto s11 = [](u32 x) { return static_cast<s16>(static_cast<s32>((x & 0x7FFU) ^ 0x400U) - 0x400); };
        if ((reg & 1U) == 0U) { dst[0] = s11(v); dst[3] = s11(v >> 12); }       // RA: red bits 0-10, alpha 12-22
        else { dst[2] = s11(v); dst[1] = s11(v >> 12); }                          // BG: blue bits 0-10, green 12-22
        return true;
    }
    default: return true;
    }
}

bool Gpu::on_primitive(Primitive prim, u8 vat, u16 count, u32 vertex_size, std::span<const u8> data,
                       std::string& error) {
    cycles_ += static_cast<u64>(count) * 12U + 6U;
    sw::TexScale scale{};
    for (unsigned i = 0; i < 8; ++i)
        scale[i] = {static_cast<float>((bp_.reg(static_cast<u8>(bp::SU_SSIZE + 2U * i)) & 0xFFFFU) + 1U),
                    static_cast<float>((bp_.reg(static_cast<u8>(bp::SU_TSIZE + 2U * i)) & 0xFFFFU) + 1U)};
    std::vector<sw::OutputVertex> out;
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        sw::InputVertex in;
        if (!decoder_.decode(cp_, xf_, bus_, phys_mask_, vat, data.subspan(static_cast<std::size_t>(i) * vertex_size, vertex_size), in, error))
            return false;
        // an indexed position of 0xFF / 0xFFFF drops the vertex: the primitive is assembled from
        // the remaining ones (VertexLoader.cpp SkipVertex / RunVertices)
        if (in.skip) continue;
        out.emplace_back();
        sw::transform_vertex(xf_, in, scale, out.back());
    }
    const sw::TexSource src{&bus_, phys_mask_, tmem_};
    return renderer_.draw(prim, out, bp_, xf_, tev_, src, error);
}

bool Gpu::on_display_list(u32 address, u32 size, std::string& error) {
    cycles_ += 6;
    std::vector<u8> list(size);
    if (bus_.dma_read(address & phys_mask_, list) != mem::BusStatus::Ok) {
        char b[96];
        std::snprintf(b, sizeof b, "display list of %u bytes at %08X is outside RAM", size, address);
        error = b;
        return false;
    }
    const ParseResult r = parse_commands(list, *this, true);
    if (r.error) { error = r.message; return false; }
    if (r.consumed != size) {
        char b[96];
        std::snprintf(b, sizeof b, "display list at %08X ends inside a command (%u of %u bytes used)", address,
                      r.consumed, size);
        error = b;
        return false;
    }
    return true;
}

} // namespace orca::gpu
