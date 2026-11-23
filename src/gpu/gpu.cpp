// Orca Core — GPU command execution. See gpu.hpp.
#include "orca/gpu/gpu.hpp"

#include <cstdio>

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
    case bp::PE_TOKEN: pe_.set_token(static_cast<u16>(v & 0xFFFFU), false, into); return true;
    case bp::PE_TOKEN_INT: pe_.set_token(static_cast<u16>(v & 0xFFFFU), true, into); return true;
    case bp::TRIGGER_EFB_COPY: error = "EFB copy (BP 52) is not implemented yet"; return false;
    case bp::LOAD_TLUT1: error = "TLUT load (BP 65) is not implemented yet"; return false;
    case bp::PRELOAD_MODE: error = "TMEM preload (BP 63) is not implemented yet"; return false;
    default: return true;
    }
}

bool Gpu::on_primitive(Primitive prim, u8 vat, u16 count, u32 vertex_size, std::span<const u8> data,
                       std::string& error) {
    (void)data;
    cycles_ += static_cast<u64>(count) * 12U + 6U;
    char b[112];
    std::snprintf(b, sizeof b, "drawing is not implemented yet (primitive %u, VAT %u, %u vertices of %u bytes)",
                  static_cast<unsigned>(prim), vat, count, vertex_size);
    error = b;
    return false;
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
