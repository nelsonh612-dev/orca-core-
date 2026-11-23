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
