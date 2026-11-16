#include "orca/memory/bus.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace orca::mem {

Bus::Bus()
    : regions_(1),
      page_region_(std::make_unique<u16[]>(kPageCount)),
      code_pages_(std::make_unique<u64[]>(kPageCount / 64U)) {}

Bus::~Bus() = default;

bool Bus::add_region(Region region) {
    if (region.size == 0U) return false;
    if ((region.base & (kPageSize - 1U)) != 0U || (region.size & (kPageSize - 1U)) != 0U) return false;
    if (static_cast<u64>(region.base) + region.size > (1ULL << 32U)) return false;
    if (regions_.size() >= 0xFFFFU) return false;
    const u32 first = region.base >> kPageShift;
    const u32 count = region.size >> kPageShift;
    for (u32 i = 0; i < count; ++i)
        if (page_region_[first + i] != 0U) return false;
    const auto index = static_cast<u16>(regions_.size());
    regions_.push_back(std::move(region));
    for (u32 i = 0; i < count; ++i) page_region_[first + i] = index;
    return true;
}

bool Bus::map_ram(std::string name, u32 base, u32 size) {
    Region r{std::move(name), base, size, nullptr, nullptr};
    if (size == 0U) return false;
    r.ram = std::make_unique<u8[]>(size); // value-initialised -> zero
    return add_region(std::move(r));
}

bool Bus::map_mmio(std::string name, u32 base, u32 size, MmioDevice& device) {
    return add_region(Region{std::move(name), base, size, nullptr, &device});
}

const Bus::Region* Bus::region_for(u32 address) const noexcept {
    const u16 idx = page_region_[address >> kPageShift];
    return idx == 0U ? nullptr : &regions_[idx];
}

Bus::Region* Bus::region_for(u32 address) noexcept {
    const u16 idx = page_region_[address >> kPageShift];
    return idx == 0U ? nullptr : &regions_[idx];
}

bool Bus::is_mapped(u32 address, u32 size) const noexcept {
    if (size == 0U) return true;
    if (static_cast<u64>(address) + size > (1ULL << 32U)) return false;
    const u32 first = address >> kPageShift;
    const u32 last = static_cast<u32>((static_cast<u64>(address) + size - 1U) >> kPageShift);
    for (u32 p = first; p <= last; ++p) {
        if (page_region_[p] == 0U) return false;
        if (p == 0xFFFFFFFFU >> kPageShift) break;
    }
    return true;
}

bool Bus::is_ram(u32 address, u32 size) const noexcept {
    if (!is_mapped(address, size)) return false;
    if (size == 0U) return true;
    const u32 first = address >> kPageShift;
    const u32 last = static_cast<u32>((static_cast<u64>(address) + size - 1U) >> kPageShift);
    for (u32 p = first; p <= last; ++p) {
        if (!regions_[page_region_[p]].ram) return false;
        if (p == 0xFFFFFFFFU >> kPageShift) break;
    }
    return true;
}

BusStatus Bus::read(u32 address, unsigned size, u64& value, AccessSource) {
    if (size != 1U && size != 2U && size != 4U && size != 8U) return BusStatus::DeviceError;
    Region* r = region_for(address);
    if (r != nullptr && r->ram && static_cast<u64>(address) + size <= static_cast<u64>(r->base) + r->size) {
        const u8* p = r->ram.get() + (address - r->base); // fast path: one RAM region
        u64 v = 0;
        for (unsigned i = 0; i < size; ++i) v = (v << 8U) | p[i];
        value = v;
        return BusStatus::Ok;
    }
    if (!is_mapped(address, size)) return BusStatus::Unmapped;
    if (r->device != nullptr) {
        if (static_cast<u64>(address) + size > static_cast<u64>(r->base) + r->size) return BusStatus::CrossesRegion;
        u64 v = 0;
        if (!r->device->mmio_read(address - r->base, size, v)) return BusStatus::DeviceError;
        value = size == 8U ? v : (v & ((1ULL << (size * 8U)) - 1U));
        return BusStatus::Ok;
    }
    if (!is_ram(address, size)) return BusStatus::CrossesRegion; // RAM that runs into MMIO
    u64 v = 0;
    for (unsigned i = 0; i < size; ++i) {
        const u32 a = address + i;
        const Region* rr = region_for(a);
        v = (v << 8U) | rr->ram[a - rr->base];
    }
    value = v;
    return BusStatus::Ok;
}

BusStatus Bus::write(u32 address, unsigned size, u64 value, AccessSource src) {
    if (size != 1U && size != 2U && size != 4U && size != 8U) return BusStatus::DeviceError;
    Region* r = region_for(address);
    if (r != nullptr && r->ram && static_cast<u64>(address) + size <= static_cast<u64>(r->base) + r->size) {
        u8* p = r->ram.get() + (address - r->base); // fast path: one RAM region
        for (unsigned i = 0; i < size; ++i) p[i] = static_cast<u8>(value >> ((size - 1U - i) * 8U));
        after_write(address, size, src);
        return BusStatus::Ok;
    }
    if (!is_mapped(address, size)) return BusStatus::Unmapped;
    if (r->device != nullptr) {
        if (static_cast<u64>(address) + size > static_cast<u64>(r->base) + r->size) return BusStatus::CrossesRegion;
        const u64 v = size == 8U ? value : (value & ((1ULL << (size * 8U)) - 1U));
        if (!r->device->mmio_write(address - r->base, size, v)) return BusStatus::DeviceError;
        after_write(address, size, src);
        return BusStatus::Ok;
    }
    if (!is_ram(address, size)) return BusStatus::CrossesRegion;
    for (unsigned i = 0; i < size; ++i) {
        const u32 a = address + i;
        Region* rr = region_for(a);
        rr->ram[a - rr->base] = static_cast<u8>(value >> ((size - 1U - i) * 8U));
    }
    after_write(address, size, src);
    return BusStatus::Ok;
}

BusStatus Bus::read8(u32 a, u8& v) { u64 t = 0; auto s = read(a, 1, t); if (s == BusStatus::Ok) v = static_cast<u8>(t); return s; }
BusStatus Bus::read16(u32 a, u16& v) { u64 t = 0; auto s = read(a, 2, t); if (s == BusStatus::Ok) v = static_cast<u16>(t); return s; }
BusStatus Bus::read32(u32 a, u32& v) { u64 t = 0; auto s = read(a, 4, t); if (s == BusStatus::Ok) v = static_cast<u32>(t); return s; }
BusStatus Bus::read64(u32 a, u64& v) { return read(a, 8, v); }
BusStatus Bus::write8(u32 a, u8 v) { return write(a, 1, v); }
BusStatus Bus::write16(u32 a, u16 v) { return write(a, 2, v); }
BusStatus Bus::write32(u32 a, u32 v) { return write(a, 4, v); }
BusStatus Bus::write64(u32 a, u64 v) { return write(a, 8, v); }

BusStatus Bus::dma_read(u32 address, std::span<u8> out) {
    const auto size = static_cast<u32>(out.size());
    if (out.size() > 0xFFFFFFFFULL || !is_ram(address, size)) return BusStatus::Unmapped;
    for (u32 done = 0; done < size;) {
        const u32 a = address + done;
        const Region* r = region_for(a);
        const u32 chunk = std::min(size - done, r->base + (r->size - 1U) - a + 1U);
        std::memcpy(out.data() + done, r->ram.get() + (a - r->base), chunk);
        done += chunk;
    }
    return BusStatus::Ok;
}

BusStatus Bus::dma_write(u32 address, std::span<const u8> in) {
    const auto size = static_cast<u32>(in.size());
    if (in.size() > 0xFFFFFFFFULL || !is_ram(address, size)) return BusStatus::Unmapped;
    for (u32 done = 0; done < size;) {
        const u32 a = address + done;
        Region* r = region_for(a);
        const u32 chunk = std::min(size - done, r->base + (r->size - 1U) - a + 1U);
        std::memcpy(r->ram.get() + (a - r->base), in.data() + done, chunk);
        done += chunk;
    }
    if (size != 0U) after_write(address, size, AccessSource::Dma);
    return BusStatus::Ok;
}

u8* Bus::ram_pointer(u32 address, u32 size) noexcept {
    Region* r = region_for(address);
    if (r == nullptr || !r->ram) return nullptr;
    if (static_cast<u64>(address) + size > static_cast<u64>(r->base) + r->size) return nullptr;
    return r->ram.get() + (address - r->base);
}

void Bus::notify_external_write(u32 address, u32 size, AccessSource src) {
    if (size != 0U) after_write(address, size, src);
}

void Bus::after_write(u32 address, u32 size, AccessSource src) {
    if (code_handler_) {
        const u32 first = address >> kPageShift;
        const u32 last = static_cast<u32>((static_cast<u64>(address) + size - 1U) >> kPageShift);
        for (u32 p = first; p <= last; ++p) {
            if ((code_pages_[p >> 6U] >> (p & 63U)) & 1U) {
                code_handler_(address, size);
                break;
            }
            if (p == 0xFFFFFFFFU >> kPageShift) break;
        }
    }
    if (!observers_.empty()) {
        const WriteEvent ev{address, size, src};
        for (auto& [id, fn] : observers_) fn(ev);
    }
}

std::size_t Bus::add_write_observer(WriteObserver observer) {
    const std::size_t id = next_observer_id_++;
    observers_.emplace_back(id, std::move(observer));
    return id;
}

void Bus::remove_write_observer(std::size_t id) {
    std::erase_if(observers_, [id](const auto& p) { return p.first == id; });
}

void Bus::set_code_invalidation_handler(CodeInvalidationHandler handler) { code_handler_ = std::move(handler); }

void Bus::mark_code_page(u32 address) noexcept {
    const u32 p = address >> kPageShift;
    code_pages_[p >> 6U] |= 1ULL << (p & 63U);
}

void Bus::unmark_code_page(u32 address) noexcept {
    const u32 p = address >> kPageShift;
    code_pages_[p >> 6U] &= ~(1ULL << (p & 63U));
}

bool Bus::is_code_page(u32 address) const noexcept {
    const u32 p = address >> kPageShift;
    return ((code_pages_[p >> 6U] >> (p & 63U)) & 1U) != 0U;
}

MmioDevice* Bus::mmio_device_at(u32 address) noexcept {
    Region* r = region_for(address);
    return r == nullptr ? nullptr : r->device;
}

std::vector<Bus::RegionInfo> Bus::regions() const {
    std::vector<RegionInfo> out;
    for (std::size_t i = 1; i < regions_.size(); ++i)
        out.push_back({regions_[i].name, regions_[i].base, regions_[i].size, regions_[i].ram != nullptr});
    return out;
}

} // namespace orca::mem
