// Orca Core — physical memory bus.
//
// The bus models the *physical* address space seen after PowerPC address
// translation. It owns RAM regions, routes MMIO accesses to devices, offers
// side-effect-free mapping probes, DMA block transfers, write observers and
// code-page tracking used to invalidate translated (JIT) code.
//
// All multi-byte accessors use big-endian byte order, matching the
// GameCube/Wii memory system.
#pragma once
#include "orca/core/bits.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace orca::mem {

enum class BusStatus : u8 {
    Ok,
    Unmapped,      // no region covers (part of) the access
    DeviceError,   // an MMIO device rejected the access
    CrossesRegion, // MMIO accesses must stay inside one device region
};

enum class AccessSource : u8 { Cpu, Dma };

// Interface for memory-mapped devices. Offsets are relative to the start of
// the device's mapped window. `size` is 1, 2, 4 or 8 bytes. Values are passed
// as host integers already assembled in big-endian significance order.
class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    virtual bool mmio_read(u32 offset, unsigned size, u64& value) = 0;
    virtual bool mmio_write(u32 offset, unsigned size, u64 value) = 0;
    virtual const char* mmio_name() const noexcept = 0;
    // Optional explanation of the last rejected access (shown in CPU stop reasons).
    virtual std::string mmio_error() const { return {}; }
};

struct WriteEvent {
    u32 address;
    u32 size;
    AccessSource source;
};

class Bus {
public:
    static constexpr u32 kPageShift = 12U; // 4 KiB tracking granularity
    static constexpr u32 kPageSize = 1U << kPageShift;
    static constexpr u32 kPageCount = 1U << (32U - kPageShift);

    Bus();
    ~Bus();
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    // Maps zero-initialised RAM. Base and size must be page aligned, size > 0,
    // and the window must not overlap an existing mapping. Returns false on error.
    bool map_ram(std::string name, u32 base, u32 size);
    // Maps an MMIO device (not owned). Same alignment/overlap rules as RAM.
    bool map_mmio(std::string name, u32 base, u32 size, MmioDevice& device);

    // Single accesses (size 1, 2, 4 or 8). Unaligned and region-crossing RAM
    // accesses are supported. Writes are validated *before* any byte is
    // modified, without performing device reads.
    BusStatus read(u32 address, unsigned size, u64& value, AccessSource src = AccessSource::Cpu);
    BusStatus write(u32 address, unsigned size, u64 value, AccessSource src = AccessSource::Cpu);

    BusStatus read8(u32 a, u8& v);
    BusStatus read16(u32 a, u16& v);
    BusStatus read32(u32 a, u32& v);
    BusStatus read64(u32 a, u64& v);
    BusStatus write8(u32 a, u8 v);
    BusStatus write16(u32 a, u16 v);
    BusStatus write32(u32 a, u32 v);
    BusStatus write64(u32 a, u64 v);

    // Returns true when every byte in [address, address+size) is mapped, with
    // no side effects. `size` may be any value up to 4 GiB - address.
    bool is_mapped(u32 address, u32 size) const noexcept;
    bool is_ram(u32 address, u32 size) const noexcept;

    // Block transfers between RAM and a host buffer, as used by DMA engines.
    // Only RAM is a valid DMA target. Fails without modifying anything if any
    // byte is not RAM.
    BusStatus dma_read(u32 address, std::span<u8> out);
    BusStatus dma_write(u32 address, std::span<const u8> in);

    // Direct host pointer to RAM, or nullptr if [address, address+size) is not
    // fully inside one RAM region. Intended for fast paths (JIT, GPU uploads).
    // Writes through this pointer bypass observers: callers must call
    // notify_external_write() afterwards.
    u8* ram_pointer(u32 address, u32 size) noexcept;
    void notify_external_write(u32 address, u32 size, AccessSource src);

    // Write epochs for caches of memory contents (e.g. decoded textures): every write
    // advances a global counter and stamps the 4 KiB pages it touches. A range is
    // unchanged since epoch E when none of its pages was stamped after E. Only the
    // first kEpochPages pages (physical 0 - 0x13FFFFFF: MEM1 and Wii MEM2) are tracked;
    // ranges beyond always count as changed.
    static constexpr u32 kEpochPages = 0x14000000U >> kPageShift;
    u64 write_epoch() const noexcept { return write_epoch_; }
    bool unchanged_since(u32 address, u32 size, u64 epoch) const noexcept;

    // Write observers are called after every successful write (CPU and DMA).
    using WriteObserver = std::function<void(const WriteEvent&)>;
    std::size_t add_write_observer(WriteObserver observer);
    void remove_write_observer(std::size_t id);

    // Code-page tracking: when a write touches a page marked as containing
    // translated code, the invalidation handler receives the written range.
    // The page stays marked until unmark_code_page() is called (normally by
    // the handler after discarding its translations).
    using CodeInvalidationHandler = std::function<void(u32 address, u32 size)>;
    void set_code_invalidation_handler(CodeInvalidationHandler handler);
    void mark_code_page(u32 address) noexcept;
    void unmark_code_page(u32 address) noexcept;
    bool is_code_page(u32 address) const noexcept;

    struct RegionInfo {
        std::string name;
        u32 base;
        u32 size;
        bool is_ram;
    };
    std::vector<RegionInfo> regions() const;
    // MMIO device mapped at `address`, or nullptr (RAM or unmapped).
    MmioDevice* mmio_device_at(u32 address) noexcept;

private:
    struct Region {
        std::string name;
        u32 base;
        u32 size;
        std::unique_ptr<u8[]> ram; // non-null for RAM
        MmioDevice* device;        // non-null for MMIO
    };

    bool add_region(Region region);
    const Region* region_for(u32 address) const noexcept;
    Region* region_for(u32 address) noexcept;
    void after_write(u32 address, u32 size, AccessSource src);

    std::vector<Region> regions_;          // index 0 unused (means unmapped)
    std::unique_ptr<u16[]> page_region_;   // kPageCount entries
    std::unique_ptr<u64[]> code_pages_;    // bitmap, kPageCount bits
    std::unique_ptr<u64[]> page_epoch_;    // kEpochPages write stamps
    u64 write_epoch_{0};
    std::vector<std::pair<std::size_t, WriteObserver>> observers_;
    std::size_t next_observer_id_{1};
    CodeInvalidationHandler code_handler_;
};

} // namespace orca::mem
