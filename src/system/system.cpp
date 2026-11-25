#include "orca/system/system.hpp"
#include "orca/ios/iosc.hpp"
#include "orca/ios/nand_setup.hpp"
#include "orca/ppc/mmu.hpp"

#include "orca/memory/console_maps.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>
#include <vector>

namespace orca::system {

System::System(SystemConfig config) : config_(config), clocks_(clocks_for(config.console)) {
    if (config_.cycles_per_instruction == 0U) config_.cycles_per_instruction = 1U;
    if (config_.console == Console::GameCube) mem::map_gamecube_ram(bus_);
    else mem::map_wii_ram(bus_);
    cpu_ = std::make_unique<ppc::Cpu>(bus_, ppc::CpuConfig{config_.pvr, config_.console == Console::Wii});
    pi_ = std::make_unique<hw::ProcessorInterface>(bus_, [this](bool level) { cpu_->set_external_interrupt(level); });
    pi_->set_gamecube(config_.console == Console::GameCube);
    gather_pipe_ = std::make_unique<hw::GatherPipe>(*pi_);
    bus_.map_mmio("PI", hw::pi::kBase, 0x1000, *pi_);
    bus_.map_mmio("GatherPipe", hw::pi::kGatherPipeBase, 0x1000, *gather_pipe_);
    const bool wii = config_.console != Console::GameCube;
    pe_ = std::make_unique<hw::PixelEngine>(scheduler_, *pi_);
    gpu_ = std::make_unique<gpu::Gpu>(bus_, *pe_, wii ? 0x1FFFFFFFU : 0x03FFFFFFU);
    cp_ = std::make_unique<hw::CommandProcessor>(bus_, *pi_, *gpu_, wii);
    bus_.map_mmio("CP", hw::cp::kBase, 0x1000, *cp_);
    bus_.map_mmio("PE", hw::pe::kBase, 0x1000, *pe_);
    efb_access_ = std::make_unique<gpu::EfbAccess>(*gpu_, *pe_);
    if (config_.use_jit && jit::host_can_execute()) {
        jit_ = std::make_unique<ppc::Jit>(*cpu_, bus_);
        if (!jit_->can_execute()) jit_.reset();
    }
    bus_.map_mmio("EFB", gpu::kEfbPhysicalBase, gpu::kEfbPhysicalSize, *efb_access_);
    pi_->set_burst_listener([this](u32) { cp_->on_gather_burst(); });
    di_ = std::make_unique<hw::DiscInterface>(bus_, scheduler_, *pi_);
    page_0c006_ = std::make_unique<hw::MmioSplitter>();
    vi_ = std::make_unique<hw::VideoInterface>(scheduler_, *pi_, clocks_.cpu_hz);
    si_ = std::make_unique<hw::SerialInterface>(scheduler_, *pi_, vi_.get());
    ai_ = std::make_unique<hw::AudioInterface>(scheduler_, *pi_, clocks_.cpu_hz, config_.console == Console::Wii);
    vi_->set_field_listener([this](bool bottom) {
        si_->on_video_field();
        if (field_listener_) field_listener_(bottom);
    });
    page_0c006_->attach(0x000, 0x40, *di_);
    page_0c006_->attach(hw::si::kOffsetInPage, 0x100, *si_);
    exi_ = std::make_unique<hw::ExternalInterface>(bus_, scheduler_, *pi_, clocks_.cpu_hz);
    ipl_ = std::make_unique<hw::IplRtcSram>(scheduler_, clocks_.cpu_hz);
    (void)exi_->attach(hw::exi::kIplChannel, hw::exi::kIplDevice, *ipl_);
    page_0c006_->attach(hw::exi::kOffsetInPage, hw::exi::kWindowSize, *exi_);
    page_0c006_->attach(hw::ai::kOffsetInPage, 0x20, *ai_);
    bus_.map_mmio("DI/SI/EXI/AI", hw::di::kBase, 0x1000, *page_0c006_);
    bus_.map_mmio("VI", hw::vi::kBase, 0x1000, *vi_);
    mi_ = std::make_unique<hw::MemoryInterface>(*pi_);
    bus_.map_mmio("MI", hw::mi::kBase, 0x1000, *mi_);
    dsp_ = std::make_unique<hw::DspInterface>(bus_, scheduler_, *pi_, config_.console == Console::GameCube, clocks_.cpu_hz);
    bus_.map_mmio("DSP", hw::dsp::kBase, 0x1000, *dsp_);
    dsp_->set_aid_rate_source([this] { return ai_->dma_rate_32khz(); });
    if (config_.dsp_hle) dsp_->use_hle(true);
    // audio output: AI DMA (DSP) + streaming audio (drive) -> mixer
    mixer_ = std::make_unique<audio::Mixer>(clocks_.cpu_hz);
    dsp_->set_audio_sink([this](std::span<const u8> block) { mixer_->push_dma_block(block, ai_->dma_divisor()); });
    ai_->set_stream_source([this](std::span<s16> out, std::size_t n) { return di_->dtk_pull(out, n); });
    if (wii) {
        // Hollywood: IPC to Starlet/IOS; the legacy DI/SI/EXI/AI page is also seen at 0x0D006000
        hollywood_ = std::make_unique<hw::HollywoodIpc>(scheduler_, *pi_, clocks_.cpu_cycles_per_timebase_tick);
        hollywood_->set_disc_present([this] { return di_->disc() != nullptr; });
        bus_.map_mmio("Hollywood", hw::hollywood::kBase, 0x1000, *hollywood_);
        bus_.map_mmio("DI/SI/EXI/AI (Hollywood)", 0x0D006000U, 0x1000, *page_0c006_);
        // The Hollywood registers are also seen at 0x0D800000 (the mirror Starlet uses;
        // Dolphin's MMIO mapping folds 0x0D80xxxx onto 0x0D00xxxx the same way)
        bus_.map_mmio("Hollywood (0x0D800000 mirror)", 0x0D800000U, 0x1000, *hollywood_);
        bus_.map_mmio("DI/SI/EXI/AI (0x0D806000 mirror)", 0x0D806000U, 0x1000, *page_0c006_);
        ios_ = std::make_unique<ios::Kernel>(bus_, scheduler_, *hollywood_, clocks_.cpu_cycles_per_timebase_tick);
        auto hook = std::make_unique<ios::StmEventHook>();
        stm_hook_ = hook.get();
        ios_->add_device(std::make_unique<ios::StmImmediate>(*hook));
        ios_->add_device(std::make_unique<ios::SdioSlot0>()); // empty SD slot
        ios_->add_device(std::make_unique<ios::NetKdRequest>()); // WiiConnect24 without network
        ios_->add_device(std::make_unique<ios::NetKdTime>([this] {
            // console date: RTC counter + SRAM counter bias (offset 0x0C), seconds since 2000
            const auto& sram = ipl_->sram();
            const auto bias = static_cast<s32>(u32{sram[0x0C]} << 24 | u32{sram[0x0D]} << 16 | u32{sram[0x0E]} << 8 | sram[0x0F]);
            return static_cast<u64>(static_cast<s64>(ipl_->rtc()) + bias + 946684800LL);
        }));
        ios_->add_device(std::move(hook));
        auto es = std::make_unique<ios::Es>();
        es_ = es.get();
        ios_->add_device(std::move(es));
        if (!config_.nand_root.empty()) {
            auto nand = std::make_unique<ios::NandFs>(config_.nand_root);
            if (nand->load(nand_error_)) {
                nand_ = std::move(nand);
                ios_->add_device(std::make_unique<ios::FsDevice>(*nand_));
            }
        }
        // Bluetooth: the remotes the console was paired with (SYSCONF BT.DINF)
        {
            std::vector<ios::BdAddr> remotes;
            if (nand_) {
                const s32 h = nand_->open(0, 0, "/shared2/sys/SYSCONF", ios::kFsRead);
                if (h >= 0) {
                    u32 size = 0, pos = 0;
                    nand_->status(h, size, pos);
                    std::vector<u8> sc(size);
                    if (nand_->read(h, sc) == static_cast<s32>(size)) remotes = ios::paired_remotes_from_sysconf(sc, bt_error_);
                    nand_->close(h);
                } else {
                    bt_error_ = "/shared2/sys/SYSCONF is not on the NAND";
                }
            }
            // controller address: fixed for the emulated module
            auto bt = std::make_unique<ios::BluetoothDevice>(ios::BdAddr{0x4F, 0x52, 0x43, 0x41, 0x00, 0x01}, remotes);
            bluetooth_ = bt.get();
            ios_->add_device(std::move(bt));
        }
        di_->set_wii_mode(); // the drive belongs to Starlet
        // OrcaIOSC's key provision: fill the empty slots automatically
        if (config_.auto_keys) (void)ios::load_default_key_provision(config_.key_dirs, config_.wii_keys, key_report_, key_error_);
        es_->set_nand(nand_.get());
        {
            const auto& c = config_.wii_keys.console;
            es_->set_console(c ? std::optional<u32>(c->ng_id) : std::nullopt,
                             c ? std::optional<u8>(c->boot2_version) : std::nullopt, config_.wii_keys.common[1].has_value());
        }
        auto di_dev = std::make_unique<ios::DiDevice>(*di_, *es_, nand_.get(), config_.wii_keys);
        di_dev_ = di_dev.get();
        ios_->add_device(std::move(di_dev));
    }
    ai_->set_stream_sink([this](std::span<const s16> pcm, u32 divisor, u32 vol) { mixer_->push_stream(pcm, divisor, vol); });
}

System::~System() = default;

void System::hle_boot(u32 entry) {
    ppc::CpuState& s = cpu_->state();
    s.ibat[0] = {0x80001FFFU, 0x00000002U};
    s.dbat[0] = {0x80001FFFU, 0x00000002U};
    s.dbat[1] = {0xC0001FFFU, 0x0000002AU}; // WIMG = 0101 (I, G), PP = 10
    s.msr = ppc::MSR_FP | ppc::MSR_IR | ppc::MSR_DR;
    s.pc = entry;
    (void)bus_.write32(0x20, 0x0D15EA5EU);
    (void)bus_.write32(0x28, mem::kMem1Size);
}

bool System::load_executable(std::span<const u8> file, std::string& error, bool boot) {
    const auto image = loader::parse_executable(file, error);
    if (!image) return false;
    if (!loader::load_into(bus_, *image, error)) return false;
    if (boot) hle_boot(image->entry);
    return true;
}

void System::advance(u64 cycles) {
    timebase_remainder_ += cycles;
    const u64 ticks = timebase_remainder_ / clocks_.cpu_cycles_per_timebase_tick;
    timebase_remainder_ %= clocks_.cpu_cycles_per_timebase_tick;
    if (ticks != 0U) cpu_->advance_timebase(ticks);
    scheduler_.advance_to(scheduler_.now() + cycles);
}

RunResult System::run_until(u32 stop_pc, u64 max_cycles) {
    stop_pc_ = stop_pc;
    const RunResult r = run(max_cycles);
    stop_pc_.reset();
    return r;
}

std::optional<u32> System::call_guest(u32 function, std::initializer_list<u32> args, u64 max_cycles, std::string& error) {
    ppc::CpuState& s = cpu_->state();
    unsigned reg = 3;
    for (u32 a : args) s.gpr[reg++] = a;
    s.lr = kGuestReturn;
    s.pc = function;
    const RunResult r = run_until(kGuestReturn, max_cycles);
    if (!r.reached_stop_pc) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "guest call to %08X did not return: %s at pc=%08X (%s)", function,
                      ppc::to_string(r.stop), s.pc, cpu_->last_stop().reason.c_str());
        error = buf;
        return std::nullopt;
    }
    return s.gpr[3];
}

bool System::boot_disc(std::string& error) {
    disc::DiscReader* d = di_->disc();
    if (d == nullptr) { error = "boot_disc: no disc inserted"; return false; }
    const auto header = disc::parse_header(*d, error);
    if (!header) return false;
    if (header->wii) {
        if (config_.console == Console::Wii) return boot_wii_disc(error);
        error = "boot_disc: " + header->game_id + " is a Wii disc; the system has to be created as a Wii";
        return false;
    }
    if (config_.console != Console::GameCube) {
        error = "boot_disc: " + header->game_id + " is a GameCube disc; running it on a Wii needs MIOS (backward compatibility), "
                "which is not emulated: create the system as a GameCube";
        return false;
    }
    const auto app = disc::parse_apploader_header(*d, error);
    if (!app) return false;
    const u32 app_bytes = app->size + app->trailer_size;
    if (disc::kApploaderLoadAddress + app_bytes > kGuestReturn) { error = "boot_disc: apploader overlaps the HLE stub area"; return false; }

    // Low-memory globals
    std::array<u8, 0x20> id{};
    d->read(0, id);
    (void)bus_.dma_write(0, id);
    const bool gc = config_.console == Console::GameCube;
    (void)bus_.write32(0x20, 0x0D15EA5EU);
    (void)bus_.write32(0x28, mem::kMem1Size);
    (void)bus_.write32(0xF8, gc ? 162000000U : 243000000U);
    (void)bus_.write32(0xFC, static_cast<u32>(clocks_.cpu_hz));
    std::vector<u8> bi2(0x2000);
    if (!d->read(0x440, bi2)) { error = "boot_disc: cannot read bi2.bin"; return false; }
    (void)bus_.dma_write(loader::virtual_to_physical(kBi2Address), bi2);
    (void)bus_.write32(0xF4, kBi2Address);

    hle_boot(0);
    write_default_exception_handlers();
    const auto entry = run_apploader(*d, 0, error);
    if (!entry) return false;
    hle_boot(*entry);
    // the IPL read the disc ID and the apploader and configured streaming audio
    di_->after_ipl_boot(id[8] != 0U, id[9]);
    return true;
}

std::optional<u32> System::run_apploader(disc::DiscReader& d, unsigned offset_shift, std::string& error) {
    const auto app = disc::parse_apploader_header(d, error);
    if (!app) return std::nullopt;
    const u32 app_bytes = app->size + app->trailer_size;
    if (disc::kApploaderLoadAddress + app_bytes > kGuestReturn) { error = "boot: apploader overlaps the HLE stub area"; return std::nullopt; }
    // Apploader image and HLE stubs
    std::vector<u8> code(app_bytes);
    if (!d.read(disc::kApploaderOffset + 0x20U, code)) { error = "boot: cannot read the apploader"; return std::nullopt; }
    (void)bus_.dma_write(loader::virtual_to_physical(disc::kApploaderLoadAddress), code);
    const u32 stub_pa = loader::virtual_to_physical(kGuestReturn);
    (void)bus_.write32(stub_pa, 0x4E800020U);      // blr (never executed: the run loop stops here)
    (void)bus_.write32(stub_pa + 4U, 0x4E800020U); // report stub: blr
    cpu_->state().gpr[1] = kApploaderStack;

    constexpr u64 kBudget = 2000000000ULL;
    const u32 slots = kGuestReturn + 0x10U, args = kGuestReturn + 0x20U;
    if (!call_guest(app->entry, {slots, slots + 4U, slots + 8U}, kBudget, error)) return std::nullopt;
    u32 init = 0, main_fn = 0, close = 0;
    bus_.read32(loader::virtual_to_physical(slots), init);
    bus_.read32(loader::virtual_to_physical(slots + 4U), main_fn);
    bus_.read32(loader::virtual_to_physical(slots + 8U), close);
    if (!call_guest(init, {kReportStub}, kBudget, error)) return std::nullopt;
    for (int i = 0;; ++i) {
        if (i > 100000) { error = "boot: apploader main() never finished"; return std::nullopt; }
        const auto more = call_guest(main_fn, {args, args + 4U, args + 8U}, kBudget, error);
        if (!more) return std::nullopt;
        u32 dst = 0, size = 0, offset = 0;
        bus_.read32(loader::virtual_to_physical(args), dst);
        bus_.read32(loader::virtual_to_physical(args + 4U), size);
        bus_.read32(loader::virtual_to_physical(args + 8U), offset);
        if (size != 0U) {
            const u32 pa = loader::virtual_to_physical(dst);
            std::vector<u8> chunk(size);
            if (!bus_.is_ram(pa, size)) { error = "boot: apploader requested a read outside RAM"; return std::nullopt; }
            if (!d.read(u64{offset} << offset_shift, chunk)) { error = "boot: apploader requested a read outside the disc"; return std::nullopt; }
            (void)bus_.dma_write(pa, chunk);
            (void)bus_.write32(loader::virtual_to_physical(args + 4U), 0); // consumed
        }
        if (*more == 0U) break;
    }
    return call_guest(close, {}, kBudget, error);
}

bool System::boot_wii_disc(std::string& error) {
    if (config_.console != Console::Wii || ios_ == nullptr || di_dev_ == nullptr) { error = "boot_wii_disc: not a Wii system"; return false; }
    disc::DiscReader* d = di_->disc();
    if (d == nullptr) { error = "boot_wii_disc: no disc inserted"; return false; }
    std::array<u8, 0x20> id{};
    if (!d->read(0, id)) { error = "boot_wii_disc: cannot read the disc ID"; return false; }
    const u32 magic = static_cast<u32>(id[0x18]) << 24 | static_cast<u32>(id[0x19]) << 16 | static_cast<u32>(id[0x1A]) << 8 | id[0x1B];
    if (magic != disc::kWiiMagic) { error = "boot_wii_disc: not a Wii disc (no Wii magic at 0x18)"; return false; }
    const auto parts = disc::list_wii_partitions(*d, error);
    if (!parts) return false;
    const disc::WiiPartitionEntry* game = nullptr;
    for (const auto& p : *parts)
        if (p.type == 0U) { game = &p; break; }
    if (game == nullptr) { error = "boot_wii_disc: the disc has no game partition"; return false; }
    // The system menu opens the game partition: ES title, owner IDs
    if (!di_dev_->open_partition(*ios_, game->offset, error)) {
        error = "boot_wii_disc: " + error;
        if (!key_error_.empty()) error += " (a key file in the automatic places was refused: " + key_error_ + ")";
        return false;
    }
    disc::WiiPartition& part = *di_dev_->partition();
    // the game's own header inside the partition (decrypted): Wii magic, DOL, FST
    if (const auto inner = disc::parse_volume_header(part, true, error); !inner) {
        error = "boot_wii_disc: game partition: " + error;
        return false;
    }
    const auto& tmd = part.tmd();
    const u32 ios_hi = static_cast<u32>(tmd[0x184]) << 24 | static_cast<u32>(tmd[0x185]) << 16 | static_cast<u32>(tmd[0x186]) << 8 | tmd[0x187];
    const u32 ios_lo = static_cast<u32>(tmd[0x188]) << 24 | static_cast<u32>(tmd[0x189]) << 16 | static_cast<u32>(tmd[0x18A]) << 8 | tmd[0x18B];
    const auto values = ios_hi == 1U ? ios::ios_memory_values(ios_lo) : std::nullopt;
    if (ios_hi == 1U) ios_->set_version(ios_lo); // which devices this IOS has
    if (!values) {
        char b[96];
        std::snprintf(b, sizeof b, "boot_wii_disc: the title needs IOS %08X-%08X, which is not in the table", ios_hi, ios_lo);
        error = b;
        return false;
    }
    // What IOS leaves for Broadway, then what the system menu adds
    if (!ios::write_ios_memory_values(bus_, *values)) { error = "boot_wii_disc: cannot write low memory"; return false; }
    (void)bus_.dma_write(0, id);
    std::array<u8, 4> pid{};
    if (!part.read(0, pid)) { error = "boot_wii_disc: " + part.last_error(); return false; }
    (void)bus_.dma_write(0x3180, pid);              // game ID as read from the partition
    (void)bus_.write32(0x20, 0x0D15EA5EU);          // boot magic
    (void)bus_.write32(0x24, 1U);                   // version
    (void)bus_.write32(0x2C, 0x00000023U);          // board model: retail
    (void)bus_.write32(0xF8, 243000000U);           // bus clock
    (void)bus_.write32(0xFC, static_cast<u32>(clocks_.cpu_hz));
    const auto region = static_cast<char>(id[3]);
    const bool pal = region == 'P' || region == 'D' || region == 'F' || region == 'I' || region == 'S' ||
                     region == 'H' || region == 'U' || region == 'X' || region == 'Y';
    (void)bus_.write32(0xCC, pal ? 1U : 0U);        // video mode the menu ran in
    (void)bus_.write32(0x3184, 0x80000000U);        // game ID address
    (void)bus_.write32(0x3194, game->type);         // partition type and offset (>> 2)
    (void)bus_.write32(0x3198, static_cast<u32>(game->offset >> 2));
    (void)bus_.write16(0x315E, 0x0113U);            // devkit boot program version (system menu)
    // the system menu's boot program marks itself as one for RVL (0x80): the SDK's
    // OSInit (CheckTargets) stops with "boot program is not for RVL target" on
    // 0x81, and the value IOS leaves (0xDE) is not what the menu passes on
    (void)bus_.write8(0x315C, 0x80U);
    (void)bus_.write16(0x30E0, 0x0000U);            // PADInit not done yet (as the menu leaves it)
    // setting.txt (encrypted, 0x100 bytes) from the system menu's data directory.
    // A console runs only discs of its own region, and games read the region from
    // this file: it is made to match the disc (region field at 0x4E000), with a
    // serial number if it has none, as Dolphin does.
    {
        if (nand_ == nullptr) { error = "boot_wii_disc: no NAND"; return false; }
        ios::WiiRegion disc_region = pal ? ios::WiiRegion::Europe : ios::WiiRegion::Usa;
        std::array<u8, 4> rf{};
        if (d->read(0x4E000, rf)) {
            switch (rf[3]) {
            case 0: disc_region = ios::WiiRegion::Japan; break;
            case 1: disc_region = ios::WiiRegion::Usa; break;
            case 2: disc_region = ios::WiiRegion::Europe; break;
            case 4: disc_region = ios::WiiRegion::Korea; break;
            default: break; // unknown: from the game ID
            }
        }
        // a serial number of 9 digits, stable for this NAND once written
        char serial[16];
        std::snprintf(serial, sizeof serial, "%09u", static_cast<unsigned>(std::random_device{}() % 900000000U + 100000000U));
        bool changed = false;
        const auto st = ios::setting_txt_for_disc(*nand_, disc_region, serial, changed, error);
        if (!st) { error = "boot_wii_disc: " + error; return false; }
        (void)bus_.dma_write(0x3800, *st);
    }
    di_->after_ipl_boot(false, 0);                  // disc ID and apploader read
    // CPU: the boot state plus MEM2 BATs (BATs 4-7 enabled)
    hle_boot(0);
    wii_boot_state();
    write_default_exception_handlers();
    const auto entry = run_apploader(part, 2, error);
    if (!entry) return false;
    hle_boot(*entry);
    wii_boot_state();
    return true;
}

void System::write_default_exception_handlers() {
    // What the IPL / system menu leaves before running an apploader: an rfi at the
    // DSI, floating-point-unavailable and system call vectors, so that an apploader's
    // `sc` (or such an exception) returns. Same three vectors as Dolphin's emulated
    // BS2 (Boot_BS2Emu.cpp), for GameCube and Wii.
    constexpr u32 kRfi = 0x4C000064U;
    for (const u32 vector : {0x300U, 0x800U, 0xC00U}) (void)bus_.write32(vector, kRfi);
}

void System::wii_boot_state() {
    ppc::CpuState& s = cpu_->state();
    s.ibat[4] = {0x90001FFFU, 0x10000002U};
    s.dbat[4] = {0x90001FFFU, 0x10000002U};
    s.dbat[5] = {0xD0001FFFU, 0x1000002AU};
    s.hid4 |= ppc::HID4_SBE;
}

u64 System::steps_until_next_event(u64 end) const {
    const u64 now = scheduler_.now();
    const u64 cpi = config_.cycles_per_instruction;
    auto steps_for = [&](u64 cycles) -> u64 { return cycles == 0U ? 1U : (cycles + cpi - 1U) / cpi; };
    // The interpreter loop runs one more step while now < end.
    u64 k = steps_for(end > now ? end - now : 0U);
    // A scheduler event at time t fires while the step that reaches t advances time;
    // the step after it must see its effect, so the block may end with that step.
    if (const auto next = scheduler_.next_event_time()) k = std::min(k, steps_for(*next > now ? *next - now : 0U));
    // Decrementer underflow (as in Cpu::advance_timebase): ticks until DEC goes 0 -> -1.
    const auto dec = static_cast<s32>(cpu_->state().dec);
    const u64 ticks = dec >= 0 ? static_cast<u64>(dec) + 1U : static_cast<u64>(static_cast<s64>(dec) + (1LL << 32)) + 1U;
    const u64 per_tick = clocks_.cpu_cycles_per_timebase_tick;
    const u64 cycles = ticks * per_tick > timebase_remainder_ ? ticks * per_tick - timebase_remainder_ : 0U;
    k = std::min(k, steps_for(cycles));
    return k;
}

RunResult System::run(u64 max_cycles) {
    RunResult r;
    const u64 start = scheduler_.now();
    const u64 end = start + max_cycles;
    const u64 retired0 = cpu_->instructions_retired();
    const u64 cpi = config_.cycles_per_instruction;
    while (scheduler_.now() < end) {
        if (dsp_->dsp_failed()) { r.device_stop = true; r.device_error = dsp_->dsp_error(); break; }
        if (cp_->stopped()) { r.device_stop = true; r.device_error = cp_->stop_reason(); break; }
        if (ios_ && ios_->stopped()) { r.device_stop = true; r.device_error = ios_->stop_reason(); break; }
        if (di_->image_failed()) { r.device_stop = true; r.device_error = di_->image_failure(); break; }
        if (stop_pc_ && cpu_->state().pc == *stop_pc_) { r.reached_stop_pc = true; break; }
        ppc::StepResult s;
        u64 steps = 1;
        const u32 pc_before = cpu_->state().pc;
        if (jit_ && !stop_pc_) {
            const ppc::Jit::Result jr = jit_->run(steps_until_next_event(end));
            s = jr.result;
            steps = jr.steps;
        } else {
            s = cpu_->step();
        }
        if (s == ppc::StepResult::Executed || s == ppc::StepResult::ExceptionTaken) {
            advance(cpi * steps);
            // a backward jump of at most 16 instructions (or a block that looped onto itself)
            const u32 pc_after = cpu_->state().pc;
            if (config_.idle_skip && !stop_pc_ && s == ppc::StepResult::Executed && pc_after <= pc_before &&
                pc_before - pc_after <= 64U && scheduler_.now() < end)
                (void)try_idle_skip(end);
            continue;
        }
        if (s == ppc::StepResult::Idle) {
            const auto next = scheduler_.next_event_time();
            if (!next) { r.stop = s; break; } // nothing can ever wake the CPU
            const u64 target = std::min(std::max(*next, scheduler_.now()), end);
            advance(target - scheduler_.now());
            if (target == end && *next > end) break;
            continue;
        }
        r.stop = s;
        break;
    }
    mixer_->advance(scheduler_.now()); // audio due by now (minus the mixer latency)
    r.cycles = scheduler_.now() - start;
    r.instructions = cpu_->instructions_retired() - retired0;
    return r;
}

// Instructions a busy-wait loop may contain: loads from memory (no update forms),
// compares, logical/rotate/add immediate operations, sync/isync, and branches
// without link (the loop's own branches).
static bool idle_safe(u32 ins) {
    const u32 op = ins >> 26;
    switch (op) {
    case 32: case 34: case 40: case 42:          // lwz lbz lhz lha
    case 10: case 11:                            // cmpli cmpi
    case 14: case 15:                            // addi addis
    case 21: case 24: case 25: case 26: case 27: // rlwinm ori oris xori xoris
    case 28: case 29:                            // andi. andis.
        return true;
    case 16: case 18:                            // bc, b (no link)
        return (ins & 1U) == 0U;
    case 19:                                     // isync
        return ((ins >> 1) & 0x3FFU) == 150U;
    case 31: {
        const u32 xo = (ins >> 1) & 0x3FFU;
        switch (xo) {
        case 0: case 32:                         // cmp cmpl
        case 23: case 87: case 279: case 343:    // lwzx lbzx lhzx lhax
        case 28: case 60: case 124: case 444: case 412: case 316: // and andc nor or orc xor
        case 598:                                // sync
            return true;
        default: return false;
        }
    }
    default:
        return false;
    }
}

bool System::idle_candidate(u32 pc, unsigned& length) {
    auto& slot = idle_rejects_[(pc >> 2) & 63U];
    ppc::CpuState& st = cpu_->state();
    unsigned n = 0;
    u32 first = 0;
    for (; n < 16U; ++n) {
        const u32 ea = pc + 4U * n;
        ppc::Translation t;
        if (!ppc::translate_without_page_table(st, ea, ppc::AccessType::Fetch, cpu_->config().extended_bats, t)) return false;
        if (!bus_.is_ram(t.physical, 4)) return false;
        u32 ins = 0;
        (void)bus_.read32(t.physical, ins);
        if (n == 0U) {
            first = ins;
            if (slot.first == pc && slot.second == ins) return false; // known not to be a busy-wait loop
        }
        if (!idle_safe(ins)) break;
        // a branch back to the start closes the loop
        const u32 op = ins >> 26;
        if ((op == 16 || op == 18) && (ins & 2U) == 0U) {
            const s32 disp = op == 18 ? static_cast<s32>((ins & 0x03FFFFFCU) << 6) >> 6 : static_cast<s32>(static_cast<s16>(ins & 0xFFFCU));
            if (ea + static_cast<u32>(disp) == pc) { length = n + 1U; return true; }
        }
    }
    slot = {pc, first};
    return false;
}

bool System::try_idle_skip(u64 end) {
    ppc::CpuState& st = cpu_->state();
    const u32 pc = st.pc;
    unsigned length = 0;
    if (!idle_candidate(pc, length)) return false;
    // one probe iteration, executed normally
    const ppc::CpuState before = st;
    const u64 epoch = bus_.write_epoch(), mmio = bus_.mmio_accesses();
    const u64 cpi = config_.cycles_per_instruction;
    u64 steps = 0;
    for (; steps <= 16U * 4U; ++steps) {
        if (scheduler_.now() >= end) return true;
        const ppc::StepResult r = cpu_->step();
        if (r != ppc::StepResult::Executed) {
            if (r == ppc::StepResult::ExceptionTaken) advance(cpi);
            return true; // the run loop sees the CPU where it is (results are those of normal execution)
        }
        advance(cpi);
        if (st.pc == pc) { ++steps; break; }
    }
    if (st.pc != pc) return true;
    const bool same = st.gpr == before.gpr && st.cr == before.cr && st.xer == before.xer && st.ctr == before.ctr &&
                      st.lr == before.lr && st.msr == before.msr && bus_.write_epoch() == epoch &&
                      bus_.mmio_accesses() == mmio && !cpu_->external_interrupt();
    if (!same) return true;
    // proven endless? (no loads in the loop, interrupts off)
    if ((st.msr & ppc::MSR_EE) == 0U) {
        bool loads = false;
        for (unsigned i = 0; i < length; ++i) {
            ppc::Translation t;
            u32 ins = 0;
            if (!ppc::translate_without_page_table(st, pc + 4U * i, ppc::AccessType::Fetch, cpu_->config().extended_bats, t) ||
                bus_.read32(t.physical, ins) != mem::BusStatus::Ok) { loads = true; break; }
            const u32 op = ins >> 26, xo = (ins >> 1) & 0x3FFU;
            if (op == 32U || op == 34U || op == 40U || op == 42U ||
                (op == 31U && (xo == 23U || xo == 87U || xo == 279U || xo == 343U))) loads = true;
        }
        if (!loads) { endless_loop_pc_ = pc; endless_loop_length_ = length; }
    }
    // whole iterations that fit before anything else can happen
    const u64 room = steps_until_next_event(end);
    const u64 iterations = room / steps;
    if (iterations < 2U) return true;
    const u64 skipped = (iterations - 1U) * steps; // the last one runs normally
    advance(skipped * cpi);
    cpu_->add_retired(skipped);
    ++idle_skips_;
    idle_skipped_ += skipped;
    return true;
}

bool System::press_console_button(bool power) {
    if (stm_hook_ == nullptr) return false;
    return stm_hook_->trigger(*ios_, power ? ios::kStmEventPower : ios::kStmEventReset, 0);
}

bool System::insert_memory_card(unsigned slot, u32 size_mbit, std::vector<u8> image, std::string& error) {
    if (slot > 1U) { error = "memory card slot must be 0 (A) or 1 (B)"; return false; }
    if (cards_[slot]) { error = "that slot already has a card"; return false; }
    auto card = hw::MemoryCard::create(scheduler_, clocks_.cpu_hz, size_mbit, std::move(image), error);
    if (!card) return false;
    card->set_interrupt_callback([this, slot] { exi_->set_device_interrupt(slot); });
    if (!exi_->attach(slot, 0, *card)) { error = "EXI slot is occupied"; return false; }
    cards_[slot] = std::move(card);
    exi_->notify_slot_change(slot); // EXTINT
    return true;
}

} // namespace orca::system
