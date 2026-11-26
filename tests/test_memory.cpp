#include <cstring>
#include "orca/memory/console_maps.hpp"
#include "test_framework.hpp"

#include <array>
#include <vector>

using namespace orca;
using namespace orca::mem;

namespace {
struct CountingDevice final : MmioDevice {
    std::array<u8, 0x1000> regs{};
    int reads = 0, writes = 0;
    bool reject = false;
    bool mmio_read(u32 off, unsigned size, u64& v) override {
        ++reads;
        if (reject) return false;
        v = 0;
        for (unsigned i = 0; i < size; ++i) v = (v << 8U) | regs[off + i];
        return true;
    }
    bool mmio_write(u32 off, unsigned size, u64 v) override {
        ++writes;
        if (reject) return false;
        for (unsigned i = 0; i < size; ++i) regs[off + i] = static_cast<u8>(v >> ((size - 1U - i) * 8U));
        return true;
    }
    const char* mmio_name() const noexcept override { return "counting"; }
};
} // namespace

TEST_CASE(mapping_rules) {
    Bus bus;
    CHECK(bus.map_ram("a", 0x0, 0x2000));
    CHECK(!bus.map_ram("overlap", 0x1000, 0x1000));
    CHECK(!bus.map_ram("unaligned", 0x3001, 0x1000));
    CHECK(!bus.map_ram("zero", 0x4000, 0));
    CHECK(!bus.map_ram("wraps", 0xFFFFF000U, 0x2000));
    CHECK(bus.map_ram("top", 0xFFFFF000U, 0x1000));
    CHECK_EQ(bus.regions().size(), 2U);
}

// Deferred RAM contents (a GPU copy still on the GPU): the range counts as
// written at once and gets its bytes when anything reaches it through the bus.
TEST_CASE(deferred_ranges_with_gaps_keep_older_ones) {
    Bus bus;
    bus.map_ram("ram", 0, 0x10000);
    {   // a newer range with gaps (not dense) does not replace an older one it covers
        unsigned fills = 0;
        const unsigned f0 = fills;
        CHECK(bus.set_lazy(0x6010, 0x100 - 0x20, [&fills](u8* dst) { ++fills; std::memset(dst, 0x77, 0x10); }));
        CHECK(bus.set_lazy(0x6000, 0x100, [&fills](u8* dst) { ++fills; std::memset(dst, 0x88, 0x10); }, false));
        CHECK_EQ(fills, f0 + 1U);
        u8 g = 0;
        CHECK(bus.read8(0x6015, g) == BusStatus::Ok); // the older range's bytes, outside the newer one's rows
        CHECK_EQ(g, 0x77U);
        CHECK(bus.read8(0x6005, g) == BusStatus::Ok);
        CHECK_EQ(g, 0x88U);
    }
}

TEST_CASE(deferred_ranges_fill_on_first_access) {
    Bus bus;
    bus.map_ram("ram", 0, 0x10000);
    bus.map_ram("ram2", 0x20000, 0x1000);
    unsigned fills = 0, flushes = 0;
    const std::size_t id = bus.add_lazy_listener([&] { ++flushes; });
    auto fill_with = [&](u8 v) { return [&fills, v](u8* dst) { ++fills; std::memset(dst, v, 0x100); }; };
    const u64 e0 = bus.write_epoch();
    CHECK(bus.set_lazy(0x1000, 0x100, fill_with(0xAB)));
    CHECK_EQ(flushes, 1U);
    CHECK(!bus.unchanged_since(0x1000, 0x100, e0)); // counts as written now
    CHECK_EQ(fills, 0U);
    u32 w = 0;
    CHECK(bus.read32(0x0FFC, w) == BusStatus::Ok);  // outside: nothing happens
    CHECK_EQ(fills, 0U);
    CHECK(bus.read32(0x1080, w) == BusStatus::Ok);  // inside: filled first
    CHECK_EQ(w, 0xABABABABU);
    CHECK_EQ(fills, 1U);
    CHECK(bus.read32(0x1084, w) == BusStatus::Ok);
    CHECK_EQ(fills, 1U);                             // once
    // a write into a pending range fills it first, then writes
    CHECK(bus.set_lazy(0x2000, 0x100, fill_with(0x11)));
    CHECK(bus.write8(0x2010, 0x99) == BusStatus::Ok);
    u8 b = 0;
    CHECK(bus.read8(0x2010, b) == BusStatus::Ok);
    CHECK_EQ(b, 0x99U);
    CHECK(bus.read8(0x2011, b) == BusStatus::Ok);
    CHECK_EQ(b, 0x11U);
    // DMA and host pointers fill too
    CHECK(bus.set_lazy(0x3000, 0x100, fill_with(0x22)));
    std::array<u8, 4> d{};
    CHECK(bus.dma_read(0x30F0, d) == BusStatus::Ok);
    CHECK_EQ(d[0], 0x22U);
    CHECK(bus.set_lazy(0x4000, 0x100, fill_with(0x33)));
    CHECK_EQ(bus.ram_pointer(0x4000, 4)[3], 0x33U);
    // a newer range covering an older one replaces it; a partial overlap fills the older first
    const unsigned before = fills;
    CHECK(bus.set_lazy(0x5000, 0x100, fill_with(0x44)));
    CHECK(bus.set_lazy(0x5000, 0x100, fill_with(0x55)));
    CHECK_EQ(fills, before);
    CHECK(bus.set_lazy(0x5080, 0x100, fill_with(0x66)));
    CHECK_EQ(fills, before + 1U);                    // 0x55 written, then 0x66 pending
    CHECK(bus.read8(0x5000, b) == BusStatus::Ok);
    CHECK_EQ(b, 0x55U);
    CHECK(bus.read8(0x5080, b) == BusStatus::Ok);
    CHECK_EQ(b, 0x66U);
    // not one RAM region: refused
    CHECK(!bus.set_lazy(0xFF80, 0x100, fill_with(0x77)));
    CHECK(!bus.set_lazy(0x10000, 0x10, fill_with(0x77)));
    // everything filled on request
    CHECK(bus.set_lazy(0x20000, 0x100, fill_with(0x88)));
    CHECK_EQ(bus.lazy_ranges(), 1U);
    bus.materialize_all();
    CHECK_EQ(bus.lazy_ranges(), 0U);
    CHECK(bus.read8(0x200FF, b) == BusStatus::Ok);
    CHECK_EQ(b, 0x88U);
    bus.remove_lazy_listener(id);
    CHECK(bus.set_lazy(0x6000, 0x10, fill_with(0x99)));
    CHECK_EQ(flushes, 8U);                           // not called after removal
}

TEST_CASE(big_endian_all_sizes) {
    Bus bus;
    bus.map_ram("ram", 0, 0x1000);
    CHECK(bus.write32(0x10, 0x11223344U) == BusStatus::Ok);
    u8 b = 0; u16 h = 0; u32 w = 0; u64 d = 0;
    CHECK(bus.read8(0x10, b) == BusStatus::Ok); CHECK_EQ(b, 0x11U);
    CHECK(bus.read8(0x13, b) == BusStatus::Ok); CHECK_EQ(b, 0x44U);
    CHECK(bus.read16(0x12, h) == BusStatus::Ok); CHECK_EQ(h, 0x3344U);
    CHECK(bus.write64(0x20, 0x0102030405060708ULL) == BusStatus::Ok);
    CHECK(bus.read64(0x20, d) == BusStatus::Ok); CHECK_EQ(d, 0x0102030405060708ULL);
    CHECK(bus.read32(0x24, w) == BusStatus::Ok); CHECK_EQ(w, 0x05060708U);
    CHECK(bus.read32(0x11, w) == BusStatus::Ok); CHECK_EQ(w, 0x22334400U); // unaligned
}

TEST_CASE(unmapped_and_partial_accesses_do_not_write) {
    Bus bus;
    bus.map_ram("ram", 0, 0x1000);
    u32 w = 0;
    CHECK(bus.read32(0x2000, w) == BusStatus::Unmapped);
    CHECK(bus.write32(0xFFE, 0xAABBCCDDU) == BusStatus::Unmapped); // last two bytes unmapped
    u16 h = 0;
    CHECK(bus.read16(0xFFE, h) == BusStatus::Ok);
    CHECK_EQ(h, 0U); // nothing was written
    CHECK(bus.read32(0xFFFFFFFEU, w) == BusStatus::Unmapped); // would wrap
}

TEST_CASE(access_spanning_two_ram_regions) {
    Bus bus;
    bus.map_ram("a", 0x0000, 0x1000);
    bus.map_ram("b", 0x1000, 0x1000);
    CHECK(bus.write32(0xFFE, 0xCAFEBABEU) == BusStatus::Ok);
    u32 w = 0;
    CHECK(bus.read32(0xFFE, w) == BusStatus::Ok);
    CHECK_EQ(w, 0xCAFEBABEU);
}

TEST_CASE(mmio_routing_and_no_read_before_write) {
    Bus bus;
    CountingDevice dev;
    bus.map_ram("ram", 0, 0x1000);
    CHECK(bus.map_mmio("dev", 0x0C003000U, 0x1000, dev));
    CHECK(bus.write32(0x0C003004U, 0xDEADBEEFU) == BusStatus::Ok);
    CHECK_EQ(dev.reads, 0); // regression for M002: stores must not read the target first
    CHECK_EQ(dev.writes, 1);
    CHECK_EQ(dev.regs[4], 0xDEU);
    u32 w = 0;
    CHECK(bus.read32(0x0C003004U, w) == BusStatus::Ok);
    CHECK_EQ(w, 0xDEADBEEFU);
    dev.reject = true;
    CHECK(bus.write16(0x0C003000U, 1) == BusStatus::DeviceError);
    CHECK(bus.read32(0x0C003FFEU, w) == BusStatus::Unmapped); // runs past the device into unmapped space
}

TEST_CASE(mmio_access_may_not_cross_device_window) {
    Bus bus;
    CountingDevice d1, d2;
    bus.map_mmio("d1", 0x0C000000U, 0x1000, d1);
    bus.map_mmio("d2", 0x0C001000U, 0x1000, d2);
    u32 w = 0;
    CHECK(bus.read32(0x0C000FFEU, w) == BusStatus::CrossesRegion);
    CHECK_EQ(d1.reads + d2.reads, 0);
}

TEST_CASE(dma_transfers_and_observers) {
    Bus bus;
    bus.map_ram("ram", 0, 0x4000);
    std::vector<WriteEvent> seen;
    const auto id = bus.add_write_observer([&](const WriteEvent& e) { seen.push_back(e); });
    const std::array<u8, 6> src{1, 2, 3, 4, 5, 6};
    CHECK(bus.dma_write(0x0FFD, src) == BusStatus::Ok); // spans a page boundary
    std::array<u8, 6> dst{};
    CHECK(bus.dma_read(0x0FFD, dst) == BusStatus::Ok);
    CHECK(dst == src);
    CHECK_EQ(seen.size(), 1U);
    CHECK(seen[0].source == AccessSource::Dma);
    CHECK_EQ(seen[0].address, 0x0FFDU);
    CHECK_EQ(seen[0].size, 6U);
    CHECK(bus.dma_write(0x3FFE, src) == BusStatus::Unmapped);
    CHECK_EQ(seen.size(), 1U);
    bus.remove_write_observer(id);
    bus.write8(0, 1);
    CHECK_EQ(seen.size(), 1U);
}

TEST_CASE(code_page_invalidation) {
    Bus bus;
    bus.map_ram("ram", 0, 0x10000);
    std::vector<std::pair<u32, u32>> hits;
    bus.set_code_invalidation_handler([&](u32 a, u32 s) { hits.emplace_back(a, s); });
    bus.mark_code_page(0x3000);
    CHECK(bus.is_code_page(0x3FFF));
    bus.write32(0x2000, 1);           // not a code page
    CHECK(hits.empty());
    bus.write32(0x2FFE, 0x12345678U); // spans into the code page
    CHECK_EQ(hits.size(), 1U);
    const std::array<u8, 4> blob{9, 9, 9, 9};
    bus.dma_write(0x3010, blob);      // DMA also invalidates
    CHECK_EQ(hits.size(), 2U);
    CHECK_EQ(hits[1].first, 0x3010U);
    bus.unmark_code_page(0x3000);
    bus.write32(0x3000, 0);
    CHECK_EQ(hits.size(), 2U);
}

TEST_CASE(ram_pointer_bounds) {
    Bus bus;
    bus.map_ram("ram", 0x1000, 0x1000);
    CHECK(bus.ram_pointer(0x1000, 0x1000) != nullptr);
    CHECK(bus.ram_pointer(0x1800, 0x801) == nullptr);
    CHECK(bus.ram_pointer(0x0, 4) == nullptr);
}

TEST_CASE(console_ram_maps) {
    Bus gc;
    CHECK(map_gamecube_ram(gc));
    CHECK(gc.is_ram(0, kMem1Size));
    CHECK(!gc.is_mapped(kMem1Size, 1));
    Bus wii;
    CHECK(map_wii_ram(wii));
    CHECK(wii.is_ram(kMem2Base, kMem2Size));
    CHECK(!wii.is_mapped(kMem2Base + kMem2Size, 1));
}

TEST_MAIN("memory")

// Write epochs (used by the GLES texture cache to skip hashing unchanged memory)
TEST_CASE(write_epochs_track_pages) {
    Bus bus;
    map_wii_ram(bus);
    const u64 e0 = bus.write_epoch();
    CHECK(bus.unchanged_since(0x00001000U, 0x2000U, e0));
    CHECK(bus.write32(0x00002FFCU, 1U) == BusStatus::Ok);   // last word of page 2
    const u64 e1 = bus.write_epoch();
    CHECK(e1 > e0);
    CHECK(!bus.unchanged_since(0x00001000U, 0x2000U, e0));  // pages 1-2 contain it
    CHECK(bus.unchanged_since(0x00001000U, 0x1000U, e0));   // page 1 alone does not
    CHECK(bus.unchanged_since(0x00003000U, 0x1000U, e0));   // nor page 3
    CHECK(bus.unchanged_since(0x00001000U, 0x2000U, e1));   // nothing since e1
    // a 64-bit write across a page boundary stamps both pages
    CHECK(bus.write64(0x00004FFCU, 0x0102030405060708ULL) == BusStatus::Ok);
    CHECK(!bus.unchanged_since(0x00004000U, 0x1000U, e1));
    CHECK(!bus.unchanged_since(0x00005000U, 0x1000U, e1));
    // DMA and external writes
    const u64 e2 = bus.write_epoch();
    const std::array<u8, 8> blk{};
    CHECK(bus.dma_write(0x10000000U, blk) == BusStatus::Ok);  // Wii MEM2
    CHECK(!bus.unchanged_since(0x10000000U, 8U, e2));
    const u64 e3 = bus.write_epoch();
    bus.notify_external_write(0x00800000U, 4U, AccessSource::Dma);
    CHECK(!bus.unchanged_since(0x00800000U, 4U, e3));
    CHECK(bus.unchanged_since(0x00801000U, 4U, e3));
    // beyond the tracked range (>= 0x14000000): always "changed"
    CHECK(!bus.unchanged_since(0x14000000U, 4U, bus.write_epoch()));
    CHECK(!bus.unchanged_since(0x13FFFFFCU, 8U, bus.write_epoch()));
    CHECK(bus.unchanged_since(0x13FFF000U, 0x1000U, bus.write_epoch()));
}
