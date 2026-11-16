#include "orca/system/system.hpp"

#include "orca/memory/console_maps.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace orca::system {

System::System(SystemConfig config) : config_(config), clocks_(clocks_for(config.console)) {
    if (config_.cycles_per_instruction == 0U) config_.cycles_per_instruction = 1U;
    if (config_.console == Console::GameCube) mem::map_gamecube_ram(bus_);
    else mem::map_wii_ram(bus_);
    cpu_ = std::make_unique<ppc::Cpu>(bus_, ppc::CpuConfig{config_.pvr});
    pi_ = std::make_unique<hw::ProcessorInterface>(bus_, [this](bool level) { cpu_->set_external_interrupt(level); });
    gather_pipe_ = std::make_unique<hw::GatherPipe>(*pi_);
    bus_.map_mmio("PI", hw::pi::kBase, 0x1000, *pi_);
    bus_.map_mmio("GatherPipe", hw::pi::kGatherPipeBase, 0x1000, *gather_pipe_);
    const bool wii = config_.console != Console::GameCube;
    pe_ = std::make_unique<hw::PixelEngine>(scheduler_, *pi_);
    gpu_ = std::make_unique<gpu::Gpu>(bus_, *pe_, wii ? 0x1FFFFFFFU : 0x03FFFFFFU);
    cp_ = std::make_unique<hw::CommandProcessor>(bus_, *pi_, *gpu_, wii);
    bus_.map_mmio("CP", hw::cp::kBase, 0x1000, *cp_);
    bus_.map_mmio("PE", hw::pe::kBase, 0x1000, *pe_);
    pi_->set_burst_listener([this](u32) { cp_->on_gather_burst(); });
    di_ = std::make_unique<hw::DiscInterface>(bus_, scheduler_, *pi_);
    page_0c006_ = std::make_unique<hw::MmioSplitter>();
    vi_ = std::make_unique<hw::VideoInterface>(scheduler_, *pi_, clocks_.cpu_hz);
    si_ = std::make_unique<hw::SerialInterface>(scheduler_, *pi_, vi_.get());
    controller_ = std::make_shared<hw::StandardController>();
    si_->connect(0, controller_);
    ai_ = std::make_unique<hw::AudioInterface>(scheduler_, *pi_, clocks_.cpu_hz);
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
}

System::~System() = default;

void System::hle_boot(u32 entry) {
    ppc::CpuState& s = cpu_->state();
    s.ibat[0] = {0x80001FFFU, 0x00000002U};
    s.dbat[0] = {0x80001FFFU, 0x00000002U};
    s.dbat[1] = {0xC0001FFFU, 0x0000002AU}; // MEM1 uncached alias, WIMG = 0101, PP = 10
    if (config_.console == Console::Wii) {
        // The Wii's uncached MEM2 alias is a separate 256 MiB BAT window:
        // 0xD0000000 -> physical 0x10000000. DBAT1 above maps 0xC0000000
        // to physical zero and therefore cannot serve this alias.
        s.dbat[3] = {0xD00007FFU, 0x1000002AU};
        // Wii MEM2 is a distinct 64 MiB region at physical 0x10000000.
        // Install its cached 0x90000000 alias for instruction fetches and
        // data accesses; DBAT1 already provides the uncached 0xD0000000 alias.
        s.ibat[1] = {0x900007FFU, 0x10000002U};
        s.dbat[2] = {0x900007FFU, 0x10000002U};
    }
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
    di_->set_read_view(nullptr);
    wii_partition_.reset();
    disc::DiscReader* raw = di_->disc();
    if (raw == nullptr) { error = "boot_disc: no disc inserted"; return false; }

    std::array<u8,0x20> raw_header{};
    if(!raw->read(0,raw_header)) { error="boot_disc: raw disc header is truncated"; return false; }
    const u32 magic=(u32{raw_header[0x18]}<<24U)|(u32{raw_header[0x19]}<<16U)|
                    (u32{raw_header[0x1A]}<<8U)|u32{raw_header[0x1B]};
    const bool is_wii=magic==disc::kWiiMagic;
    disc::DiscReader* boot_reader=raw;
    std::optional<disc::DiscHeader> header;

    if(is_wii) {
        if(config_.console!=Console::Wii) {
            error="boot_disc: Wii disc requires a System configured for the Wii console";
            return false;
        }
        if(wii_key_provider_==nullptr) {
            error="boot_disc: encrypted Wii disc requires a user-supplied common-key provider";
            return false;
        }
        const auto table=disc::parse_wii_partition_table(*raw,error);
        if(!table) return false;
        const auto it=std::find_if(table->partitions.begin(),table->partitions.end(),
                                   [](const disc::WiiPartitionInfo& p){ return p.type==0U; });
        if(it==table->partitions.end()) {
            error="boot_disc: no game-data partition (type 0) found";
            return false;
        }
        crypto::Aes128Key title_key{};
        if(!disc::derive_wii_title_key(*raw,*it,*wii_key_provider_,title_key,error)) return false;
        wii_partition_=std::make_unique<disc::WiiPartitionReader>(*raw,*it,title_key);
        volatile u8* wipe=title_key.data();
        for(std::size_t i=0;i<title_key.size();++i) wipe[i]=0;
        if(!wii_partition_->valid()) {
            error=wii_partition_->error();
            wii_partition_.reset();
            return false;
        }
        boot_reader=wii_partition_.get();
        header=disc::parse_wii_partition_header(*boot_reader,error);
    } else {
        if(config_.console!=Console::GameCube) {
            error="boot_disc: GameCube disc requires a System configured for GameCube";
            return false;
        }
        header=disc::parse_header(*raw,error);
    }
    if(!header) return false;

    const auto app=disc::parse_apploader_header(*boot_reader,error);
    if(!app) return false;
    if(is_wii) di_->set_read_view(wii_partition_.get());
    const u32 app_bytes=app->size+app->trailer_size;
    if(disc::kApploaderLoadAddress+app_bytes>kGuestReturn) {
        error="boot_disc: apploader overlaps the HLE stub area";
        return false;
    }

    // The decrypted Wii partition presents the same logical header/BI2/
    // apploader/DOL address space as a GameCube disc.
    std::array<u8,0x20> id{};
    if(!boot_reader->read(0,id)) { error="boot_disc: cannot read logical disc ID"; return false; }
    (void)bus_.dma_write(0,id);
    const bool gc=config_.console==Console::GameCube;
    (void)bus_.write32(0x20,0x0D15EA5EU);
    (void)bus_.write32(0x28,mem::kMem1Size);
    (void)bus_.write32(0xF8,gc?162000000U:243000000U);
    (void)bus_.write32(0xFC,static_cast<u32>(clocks_.cpu_hz));
    std::vector<u8> bi2(0x2000);
    if(!boot_reader->read(0x440,bi2)) { error="boot_disc: cannot read bi2.bin"; return false; }
    (void)bus_.dma_write(loader::virtual_to_physical(kBi2Address),bi2);
    (void)bus_.write32(0xF4,kBi2Address);

    std::vector<u8> code(app_bytes);
    if(!boot_reader->read(disc::kApploaderOffset+0x20U,code)) {
        error="boot_disc: cannot read the apploader";
        return false;
    }
    (void)bus_.dma_write(loader::virtual_to_physical(disc::kApploaderLoadAddress),code);
    const u32 stub_pa=loader::virtual_to_physical(kGuestReturn);
    (void)bus_.write32(stub_pa,0x4E800020U);
    (void)bus_.write32(stub_pa+4U,0x4E800020U);
    hle_boot(0);
    cpu_->state().gpr[1]=kApploaderStack;

    constexpr u64 kBudget=2000000000ULL;
    const u32 slots=kGuestReturn+0x10U,args=kGuestReturn+0x20U;
    if(!call_guest(app->entry,{slots,slots+4U,slots+8U},kBudget,error)) return false;
    u32 init=0,main_fn=0,close=0;
    bus_.read32(loader::virtual_to_physical(slots),init);
    bus_.read32(loader::virtual_to_physical(slots+4U),main_fn);
    bus_.read32(loader::virtual_to_physical(slots+8U),close);
    if(!call_guest(init,{kReportStub},kBudget,error)) return false;
    for(int i=0;;++i) {
        if(i>100000) { error="boot_disc: apploader main() never finished"; return false; }
        const auto more=call_guest(main_fn,{args,args+4U,args+8U},kBudget,error);
        if(!more) return false;
        u32 dst=0,size=0,offset=0;
        bus_.read32(loader::virtual_to_physical(args),dst);
        bus_.read32(loader::virtual_to_physical(args+4U),size);
        bus_.read32(loader::virtual_to_physical(args+8U),offset);
        if(size!=0U) {
            const u32 pa=loader::virtual_to_physical(dst);
            std::vector<u8> chunk(size);
            if(!bus_.is_ram(pa,size)) { error="boot_disc: apploader requested a read outside RAM"; return false; }
            if(!boot_reader->read(offset,chunk)) {
                error="boot_disc: apploader requested a read outside the logical disc/partition";
                return false;
            }
            (void)bus_.dma_write(pa,chunk);
            (void)bus_.write32(loader::virtual_to_physical(args+4U),0);
        }
        if(*more==0U) break;
    }
    const auto entry=call_guest(close,{},kBudget,error);
    if(!entry) return false;
    hle_boot(*entry);
    return true;
}

RunResult System::run(u64 max_cycles) {
    RunResult r;
    const u64 start = scheduler_.now();
    const u64 end = start + max_cycles;
    const u64 retired0 = cpu_->instructions_retired();
    const u64 cpi = std::max<u64>(config_.cycles_per_instruction, 1U);
    while (scheduler_.now() < end) {
        if (dsp_->dsp_failed()) { r.device_stop = true; r.device_error = dsp_->dsp_error(); break; }
        if (cp_->stopped()) { r.device_stop = true; r.device_error = cp_->stop_reason(); break; }
        if (stop_pc_ && cpu_->state().pc == *stop_pc_) { r.reached_stop_pc = true; break; }
        ppc::StepResult s = ppc::StepResult::Executed;
        u64 completed = 0;
#if defined(__aarch64__)
        // Run native blocks only up to the next scheduler boundary. This keeps
        // VI/AI/DI/DSP events and asynchronous interrupt delivery ordered.
        const auto cycles_to_instructions = [cpi](u64 cycles) {
            return cycles / cpi + (cycles % cpi != 0U ? 1U : 0U);
        };
        u64 budget = std::max<u64>(1U, cycles_to_instructions(end - scheduler_.now()));
        if (const auto next = scheduler_.next_event_time()) {
            const u64 until_event = *next > scheduler_.now() ? *next - scheduler_.now() : 0U;
            budget = std::min(budget, std::max<u64>(1U, cycles_to_instructions(until_event)));
        }
        // run_until()/call_guest() must stop before executing the return stub.
        if (stop_pc_ && *stop_pc_ > cpu_->state().pc) {
            const u64 distance = static_cast<u64>(*stop_pc_) - cpu_->state().pc;
            if ((distance & 3U) == 0U) budget = std::min(budget, distance / 4U);
        }
        s = cpu_->run_jit(budget, &completed, stop_pc_ ? &*stop_pc_ : nullptr);
#else
        s = cpu_->step();
        if (s == ppc::StepResult::Executed || s == ppc::StepResult::ExceptionTaken) completed = 1U;
#endif
        if (completed != 0U) advance(completed * cpi);
        if (s == ppc::StepResult::Executed || s == ppc::StepResult::ExceptionTaken) continue;
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
    r.cycles = scheduler_.now() - start;
    r.instructions = cpu_->instructions_retired() - retired0;
    return r;
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
