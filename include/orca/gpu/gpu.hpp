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
// Primitives are decoded, transformed and drawn by the software renderer
// (sw/vertex.hpp, sw/renderer.hpp), which lists what it does not do yet. A vertex whose
// indexed position is 0xFF / 0xFFFF is dropped and the primitive is built from the rest,
// as Dolphin's vertex loaders do (its software renderer keeps it: sources disagree).
// BP 0xE0-0xE7 also set the TEV colour registers or constants (bit 23 of the
// value selects constant), as in BPStructs.cpp.
// TLUT loads (BP 0x65): tlut_src (BP 0x64) << 5, masked to 0x01FFFFFF on
// GameCube, copied to TMEM at (BP 0x65 bits 0-9) << 9, (bits 10-20) * 32 bytes.
// EFB copies and clears (BP 0x52) are done by sw/efb_copy.hpp, which lists
// its sources and limits.
// TMEM preloads (BP 0x63, non-zero value), as in BPStructs.cpp: source
// BP 60 << 5, TMEM line (32 bytes) BP 61; type 3 (RGBA8) alternates 32-byte
// lines between the even (BP 61) and odd (BP 62) banks, stopping at the end of
// TMEM; other types copy count * 32 bytes, cut at the end of TMEM. Orca masks
// the source with the console's physical mask (Dolphin notes this as a TODO).
// Not implemented yet (each stops with a message): draw-done values other than 2.
#pragma once
#include "orca/gpu/bp_memory.hpp"
#include "orca/gpu/command_stream.hpp"
#include "orca/gpu/cp_state.hpp"
#include "orca/gpu/xf_memory.hpp"
#include "orca/gpu/sw/efb_copy.hpp"
#include "orca/gpu/sw/renderer.hpp"
#include "orca/gpu/sw/vertex.hpp"
#include "orca/hw/pixel_engine.hpp"
#include "orca/memory/bus.hpp"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orca::gpu {

class Gpu final : public CommandSink {
public:
    // `phys_mask`: 0x03FFFFFF on GameCube, 0x1FFFFFFF on Wii (applied to display
    // list and array addresses, as Dolphin's GetPhysicalAddressMask).
    Gpu(mem::Bus& bus, hw::PixelEngine& pe, u32 phys_mask = 0x03FFFFFFU) : bus_(bus), pe_(pe), phys_mask_(phys_mask) {
        renderer_.set_stats(&pe_.stats());
    }

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
    const sw::TevState& tev() const noexcept { return tev_; }
    sw::Renderer& renderer() noexcept { return renderer_; }
    const std::vector<u8>& tmem() const noexcept { return tmem_; }
    // Draws with a GPU backend instead of the software rasterizer (nullptr = software).
    // `download` / `upload` move the EFB between the backend and renderer().efb() around
    // EFB copies and clears, which run on the CPU.
    // `download` / `upload` move the given EFB area between the backend and
    // renderer().efb() around an EFB copy (and its clear).
    // `download` gets which planes the copy reads (colour, depth).
    void set_backend(sw::TriangleSink* sink, std::function<void(sw::Efb&, const sw::EfbRect&, bool, bool)> download,
                     std::function<void(const sw::Efb&, const sw::EfbRect&)> upload) {
        renderer_.set_sink(sink);
        PixelStats& s = pe_.stats();
        s.tracked = sink == nullptr || sink->attach_stats(&s);
        if (sink != nullptr && s.tracked)
            pe_.set_stats_sync([sink](bool bbox, std::string& e) { return sink->sync_stats(bbox, e); });
        else
            pe_.set_stats_sync(nullptr);
        if (sink == nullptr) set_gpu_copies({});
        download_ = std::move(download);
        upload_ = std::move(upload);
    }
    // EFB copies done by the GPU backend itself (Dolphin keeps them on the GPU):
    // a display copy is encoded there and its RAM bytes become a deferred range
    // of the bus (mem::Bus::set_lazy), read back only when something touches
    // them; a whole-value clear runs there too. Copies these cannot do keep the
    // download / efb_copy / upload path. Unset = always that path.
    struct GpuCopies {
        std::function<bool(const sw::XfbPlan&, u32&, std::string&)> encode_xfb;
        std::function<void(u32, u8*)> read_xfb;
        std::function<void(u32)> drop_xfb;
        std::function<bool(const sw::CopyArea&, const sw::ClearPlan&, std::string&)> clear;
        std::function<bool(const sw::TexCopyPlan&, u32&, std::string&)> encode_texture; // optional
        // optional: the copy's rows are in RAM (deferred) as of this bus write epoch
        std::function<void(u32, u64)> placed;
    };
    void set_gpu_copies(GpuCopies copies);
    // Copies done by GpuCopies, and the ones with a GpuCopies that took the CPU path.
    u64 gpu_xfb_copies() const noexcept { return gpu_xfb_copies_; }
    u64 gpu_texture_copies() const noexcept { return gpu_texture_copies_; }
    u64 cpu_fallback_copies() const noexcept { return cpu_fallback_copies_; }
    const std::string& last_fallback() const noexcept { return last_fallback_; }
    // Every BP 0x52 by the path it took (cumulative).
    struct CopyPaths {
        u64 gpu_display{0}, gpu_texture{0};  // encoded on the GPU (deferred RAM)
        u64 no_destination{0};               // GPU backend, no RAM written (stride 0): clear only or nothing
        u64 ignored{0};                      // outside the EFB
        u64 cpu_with_gpu{0};                 // GPU backend, CPU path (download / sw encode / upload)
        u64 no_encoder{0};                   // (counted in cpu_with_gpu too) texture copy without a GPU encoder
        u64 no_gpu_copies{0};                // GPU backend without GpuCopies: CPU path
        u64 software{0};                     // software renderer
    };
    const CopyPaths& copy_paths() const noexcept { return paths_; }
    // The display copy on the GPU that a VI field shows, if any: the field's
    // rows (address + y * stride, width pixels, y < height) are lines
    // first_line + y * line_step of copy `handle`, and RAM there has not been
    // written since (the copy may still be unread). A frontend presents it from
    // the GPU instead of reading RAM back.
    struct GpuField { bool valid{false}; u32 handle{0}, first_line{0}, line_step{1}; };
    GpuField gpu_field(u32 address, u32 width, u32 height, u32 stride) const;

    // CPU access to one EFB pixel (24-bit colour / depth values), from the software EFB
    // or the GPU backend that draws.
    bool efb_peek(u32 x, u32 y, u32& color, u32& depth, std::string& error) {
        if (renderer_.sink() != nullptr) return renderer_.sink()->peek_pixel(x, y, color, depth, error);
        color = renderer_.efb().color24(x, y);
        depth = renderer_.efb().depth24(x, y);
        return true;
    }
    bool efb_poke(u32 x, u32 y, bool depth, u32 value, std::string& error) {
        if (renderer_.sink() != nullptr) return renderer_.sink()->poke_pixel(x, y, depth, value, error);
        if (depth) renderer_.efb().set_depth24(x, y, value);
        else renderer_.efb().set_color24(x, y, value);
        return true;
    }
    // Result of the last BP 0x52 (EFB copy / clear) and how many there were.
    const sw::CopyResult& last_copy() const noexcept { return last_copy_; }
    u64 copies() const noexcept { return copies_; }
    // Diagnostics: host time in EFB copies (ns): reading the EFB back from a GPU
    // backend, encoding it into RAM on the CPU, and writing a cleared area back.
    struct CopyTimes { u64 readback{0}, encode{0}, upload{0}; };
    const CopyTimes& copy_times() const noexcept { return copy_ns_; }

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
    // A BP write replaces half of the box (or all counters): the other half must be the
    // backend's current value first.
    bool sync_stats_before_cpu_write(bool bbox, std::string& error) {
        PixelStats& s = pe_.stats();
        if (renderer_.sink() == nullptr || !s.tracked || !(bbox ? s.bbox_armed : s.quads_armed)) return true;
        return renderer_.sink()->sync_stats(bbox, error);
    }
    mem::Bus& bus_;
    hw::PixelEngine& pe_;
    u32 phys_mask_;
    CpState cp_;
    XfMemory xf_;
    BpMemory bp_;
    sw::TevState tev_;
    sw::VertexDecoder decoder_;
    sw::Renderer renderer_;
    std::vector<u8> tmem_ = std::vector<u8>(1U << 20, 0); // 1 MiB texture memory
    std::vector<u8> pending_;
    sw::CopyResult last_copy_;
    std::function<void(sw::Efb&, const sw::EfbRect&, bool, bool)> download_;
    std::function<void(const sw::Efb&, const sw::EfbRect&)> upload_;
    u64 copies_{0};
    CopyTimes copy_ns_;
    GpuCopies gpu_copies_;
    // false once the GpuCopies that encoded a pending range is replaced
    std::shared_ptr<bool> gpu_copies_alive_ = std::make_shared<bool>(true);
    u64 gpu_xfb_copies_{0}, gpu_texture_copies_{0}, cpu_fallback_copies_{0};
    struct GpuXfbRecord { u32 dest{0}, stride{0}, width{0}, lines{0}, handle{0}; u64 epoch{0}; };
    std::array<GpuXfbRecord, 8> gpu_xfbs_{};
    std::size_t gpu_xfb_next_{0};
    std::string last_fallback_;
    CopyPaths paths_;
    bool gpu_copy(u32 trigger, std::string& error, bool& done);
    bool defer_rows(u32 handle, u32 dest, u32 stride, u32 row, u32 lines, std::string& error);
    u64 cycles_{0};       // total GPU time
    u64 batch_start_{0};  // cycles_ at the start of the current feed()
    bool failed_{false};
    std::string error_;
};

} // namespace orca::gpu
