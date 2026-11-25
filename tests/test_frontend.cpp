// Frontend layer: loading, frames, controls, Wii prerequisites.
#include "orca/frontend/emulator.hpp"
#include "test_framework.hpp"
#include "wii_disc_builder.hpp"
#include "test_port.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

using namespace orca;

namespace {
std::vector<u8> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE(frontend_rejects_what_it_cannot_load) {
    frontend::Settings s;
    s.jit = false;
    frontend::Emulator emu(s);
    std::string error;
    CHECK(!emu.run_frame());
    CHECK(emu.error() == "nothing loaded");
    CHECK(!emu.load_disc(std::make_unique<disc::MemoryDisc>(std::vector<u8>(0x100, 0)), error));
    CHECK(error.find("magic") != std::string::npos);
    // an encrypted Wii disc without NAND / key settings
    const std::vector<u8> wii = test_wii::build_disc(test_wii::plain_data(1));
    CHECK(!emu.load_disc(std::make_unique<disc::MemoryDisc>(wii), error));
    CHECK(error.find("NAND") != std::string::npos);
    const auto dir = std::filesystem::temp_directory_path() / "orca_frontend_nand";
    std::filesystem::remove_all(dir);
    frontend::Settings w;
    w.nand_dir = dir.string();
    frontend::Emulator emu2(w);
    CHECK(!emu2.load_disc(std::make_unique<disc::MemoryDisc>(wii), error));
    CHECK(error.find("needs the common key (index 0)") != std::string::npos);
    // the NAND was prepared on the way (SYSCONF and setting.txt)
    CHECK(std::filesystem::exists(dir / "shared2" / "sys" / "SYSCONF"));
    std::filesystem::remove_all(dir);
}

TEST_CASE(frontend_dsp_rom_dumps_are_checked) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto irom = dir / "orca_test_irom.bin", coef = dir / "orca_test_coef.bin";
    { std::ofstream(irom, std::ios::binary).write(std::string(100, '\0').data(), 100); }
    { std::ofstream(coef, std::ios::binary).write(std::string(4096, '\0').data(), 4096); }
    frontend::Settings s;
    s.jit = false;
    s.dsp_irom_file = irom.string();
    s.dsp_coef_file = coef.string();
    frontend::Emulator emu(s);
    std::string error;
    CHECK(!emu.load_executable(std::vector<u8>(16, 0), false, error));
    CHECK(error.find("8192") != std::string::npos);
    std::filesystem::remove(irom);
    std::filesystem::remove(coef);
}

#ifdef ORCA_PROGRAMS_DIR
TEST_CASE(frontend_runs_gpu_draw_and_shows_its_frame) {
    frontend::Settings s;
    s.jit = false;
    frontend::Emulator emu(s);
    std::string error;
    CHECK(emu.load_executable(read_file(ORCA_PROGRAMS_DIR "/gpu_draw.dol"), false, error));
    test::TestPort port;
    port.cpu = &emu.system()->cpu();
    emu.system()->bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    hw::PadState pad;
    pad.buttons = hw::PadButtons::A;
    emu.set_pad(0, pad);
    int frames = 0;
    while (emu.run_frame() && frames < 100) ++frames;
    CHECK(port.exited);
    CHECK(emu.error().find("halted") != std::string::npos || emu.error().find("Halted") != std::string::npos);
    CHECK(emu.capture_frame());
    const auto& f = emu.frame();
    CHECK_EQ(f.width, 640U);
    CHECK_EQ(f.height, 480U);
    const std::size_t red = (100U * 640U + 200U) * 4U;
    CHECK(f.rgba[red] >= 250U && f.rgba[red + 1] <= 3U);
    std::array<s16, 64> audio{};
    CHECK_EQ(emu.take_audio(audio), 0U); // the program plays no sound
}

TEST_CASE(frontend_reports_a_game_that_halts_itself) {
    // what Nintendo's SDK does after OSPanic: interrupts off, branch to itself
    frontend::Settings s;
    s.jit = false;
    frontend::Emulator emu(s);
    std::string error;
    CHECK(emu.load_executable(read_file(ORCA_PROGRAMS_DIR "/gpu_draw.dol"), false, error));
    auto& st = emu.system()->cpu().state();
    const u32 pc = st.pc;
    auto& bus = emu.system()->bus();
    const char msg[] = "Failed assertion in nwc24.c";
    for (u32 i = 0; i < sizeof msg; ++i) CHECK(bus.write8(0x300000U + i, static_cast<u8>(msg[i])) == mem::BusStatus::Ok);
    // PPCHalt's shape: sync; loop: nop; li r4,0; nop; b loop
    const u32 p0 = pc & 0x3FFFFFFFU;
    const u32 code[] = {0x3C608030U /* lis r3,0x8030 */, 0x7C0004ACU /* sync */, 0x60000000U, 0x38800000U, 0x60000000U,
                        0x4BFFFFF4U /* b -12 */};
    for (u32 i = 0; i < 6U; ++i) CHECK(bus.write32(p0 + 4U * i, code[i]) == mem::BusStatus::Ok);
    st.msr &= ~0x8000U;
    st.lr = 0x801B65ECU;
    CHECK(!emu.run_frame());
    const std::string& e = emu.error();
    CHECK(e.find("the game stopped itself") != std::string::npos);
    CHECK(e.find("loop with interrupts off at") != std::string::npos);
    CHECK(e.find("801B65EC") != std::string::npos);
    CHECK(e.find("r3=\"Failed assertion in nwc24.c\"") != std::string::npos);
    // with interrupts on the same loop can be left (by an interrupt): not reported
    frontend::Emulator emu2(s);
    CHECK(emu2.load_executable(read_file(ORCA_PROGRAMS_DIR "/gpu_draw.dol"), false, error));
    auto& st2 = emu2.system()->cpu().state();
    CHECK(emu2.system()->bus().write32(st2.pc & 0x3FFFFFFFU, 0x48000000U) == mem::BusStatus::Ok);
    st2.msr |= 0x8000U;
    for (const u32 v : {0x500U, 0x900U}) CHECK(emu2.system()->bus().write32(v, 0x4C000064U) == mem::BusStatus::Ok); // rfi
    const bool ran = emu2.run_frame();
    if (!ran) std::printf("    %s\n", emu2.error().c_str());
    CHECK(ran);
}

TEST_CASE(frontend_diagnostics_show_where_the_game_runs) {
    frontend::Settings s;
    s.jit = false;
    frontend::Emulator emu(s);
    std::string error;
    CHECK(emu.load_executable(read_file(ORCA_PROGRAMS_DIR "/gpu_draw.dol"), false, error));
    test::TestPort port;
    port.cpu = &emu.system()->cpu();
    emu.system()->bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    (void)emu.run_frame(); // runs some time (the program may finish within it)
    const std::string d = emu.diagnostics();
    if (d.find(" hot 8") == std::string::npos) std::printf("    %s\n", d.c_str());
    CHECK(d.find("PC 8") != std::string::npos);
    CHECK(d.find(" hot 8") != std::string::npos); // sampled PCs, with their share
    CHECK(d.find("%") != std::string::npos);
    CHECK(d.find("\nVI ") != std::string::npos);
    CHECK(d.find("PI cause") != std::string::npos);
    CHECK(emu.diagnostics().find(" hot") == std::string::npos); // samples were reset
}
#endif

int main() { return orca::test::run_all("frontend"); }
