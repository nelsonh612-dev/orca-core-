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
    // first a delay loop with interrupts off (it ends: not a halt), then PPCHalt's
    // shape: sync; loop: nop; li r4,0; nop; b loop
    const u32 p0 = pc & 0x3FFFFFFFU;
    const u32 code[] = {0x3C608030U /* lis r3,0x8030 */, 0x3CA00002U /* lis r5,2 */, 0x38A5FFFFU /* addi r5,r5,-1 */,
                        0x2C050000U /* cmpwi r5,0 */, 0x4082FFF8U /* bne -8 */, 0x7C0004ACU /* sync */, 0x60000000U,
                        0x38800000U, 0x60000000U, 0x4BFFFFF4U /* b -12 */};
    for (u32 i = 0; i < 10U; ++i) CHECK(bus.write32(p0 + 4U * i, code[i]) == mem::BusStatus::Ok);
    st.msr &= ~0x8000U;
    st.lr = 0x801B65ECU;
    // OSPanic's frame further up the stack keeps a pointer to its message
    const char panic[] = "DVD: no disc";
    for (u32 i = 0; i < sizeof panic; ++i) CHECK(bus.write8(0x300100U + i, static_cast<u8>(panic[i])) == mem::BusStatus::Ok);
    st.gpr[1] = 0x80380000U;
    CHECK(bus.write32(0x380000U, 0x80380040U) == mem::BusStatus::Ok);  // back chain
    CHECK(bus.write32(0x380040U, 0x80380100U) == mem::BusStatus::Ok);
    CHECK(bus.write32(0x380044U, 0x801F76E0U) == mem::BusStatus::Ok);  // saved LR
    CHECK(bus.write32(0x380070U, 0x80300100U) == mem::BusStatus::Ok);  // saved r5 (format)
    CHECK(bus.write32(0x380100U, 0U) == mem::BusStatus::Ok);
    // its register save area: r3 file, r4 line, r5 format, then the format's
    // arguments; the report prints the formatted message
    const char fmt[] = "VIConfigure(): Tried to change mode from (%d) to (%d), which is forbidden\n";
    for (u32 i = 0; i < sizeof fmt; ++i) CHECK(bus.write8(0x300300U + i, static_cast<u8>(fmt[i])) == mem::BusStatus::Ok);
    const char vic[] = "vi.c";
    for (u32 i = 0; i < sizeof vic; ++i) CHECK(bus.write8(0x300380U + i, static_cast<u8>(vic[i])) == mem::BusStatus::Ok);
    const u32 save[] = {0x80300380U, 2647U, 0x80300300U, 1U, 0U};
    for (u32 i = 0; i < 5U; ++i) CHECK(bus.write32(0x380050U + 4U * i, save[i]) == mem::BusStatus::Ok);
    // the caller of OSPanic: OSPanic("OSFile.c", 123, "DVD: no disc") built just before the call
    const char file[] = "OSFile.c";
    for (u32 i = 0; i < sizeof file; ++i) CHECK(bus.write8(0x300200U + i, static_cast<u8>(file[i])) == mem::BusStatus::Ok);
    const u32 caller[] = {0x3C608030U /* lis r3,0x8030 */, 0x38630200U /* addi r3,r3,0x200 */, 0x3880007BU /* li r4,123 */,
                          0x3CA08030U /* lis r5,0x8030 */, 0x60A50100U /* ori r5,r5,0x100 */, 0x4CC63182U /* crclr 6 */,
                          0x48000001U /* bl (OSPanic) */};
    for (u32 i = 0; i < 7U; ++i) CHECK(bus.write32(0x1F76C4U + 4U * i, caller[i]) == mem::BusStatus::Ok);
    int frames = 0;
    while (emu.run_frame() && frames < 20) ++frames;
    CHECK(frames < 20);
    const std::string& e = emu.error();
    CHECK(e.find("the game stopped itself") != std::string::npos);
    char at[16];
    std::snprintf(at, sizeof at, "off at %08X", pc + 24U); // the halt loop, not the delay loop
    CHECK(e.find(at) != std::string::npos);
    CHECK_EQ(emu.system()->cpu().state().gpr[5], 0U);       // the delay loop ran to its end
    CHECK(e.find("801B65EC <- 801F76E0") != std::string::npos);
    CHECK(e.find("\"DVD: no disc\"") != std::string::npos);
    CHECK(e.find("Panic call at 801F76DC: r3=\"OSFile.c\" r4=123 r5=\"DVD: no disc\"") != std::string::npos);
    CHECK(e.find("r3=\"Failed assertion in nwc24.c\"") != std::string::npos);
    CHECK(e.find("Message: \"VIConfigure(): Tried to change mode from (1) to (0), which is forbidden\\n\"") != std::string::npos);
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

// A game spinning on a flag that an interrupt handler should set (lwz r0,
// -0x7020(r13); cmplwi r0,0; beq back): the report names the word and its value.
TEST_CASE(frontend_diagnostics_name_the_word_a_polling_loop_waits_on) {
    frontend::Settings s;
    s.jit = false;
    frontend::Emulator emu(s);
    std::string error;
    CHECK(emu.load_executable(read_file(ORCA_PROGRAMS_DIR "/gpu_draw.dol"), false, error));
    auto& st = emu.system()->cpu().state();
    const u32 pc = st.pc & 0x3FFFFFFFU, pc0 = st.pc;
    const u32 loop[] = {0x800D8FE0U, 0x28000000U, 0x4182FFF8U};
    for (u32 i = 0; i < 3U; ++i) CHECK(emu.system()->bus().write32(pc + 4U * i, loop[i]) == mem::BusStatus::Ok);
    st.gpr[13] = 0x80400000U;
    CHECK(emu.system()->bus().write32(0x003F8FE0U, 0) == mem::BusStatus::Ok);
    // somewhere else the code that sets the flag: stw r3,-0x7020(r13)
    CHECK(emu.system()->bus().write32(0x00012340U, 0x906D8FE0U) == mem::BusStatus::Ok);
    CHECK(emu.run_frame());
    const std::string d = emu.diagnostics();
    char want[64];
    std::snprintf(want, sizeof want, "waits %08X-%08X on [803F8FE0]=00000000", pc0, pc0 + 8U);
    if (d.find(want) == std::string::npos) std::printf("    %s\n", d.c_str());
    CHECK(d.find(want) != std::string::npos);
    CHECK(d.find("(stored at 80012340)") != std::string::npos);
    CHECK(d.find("\nirq") != std::string::npos);
    CHECK(d.find("\nDSP CSR ") != std::string::npos);
    // the second report says where the host time went since the first
    CHECK(emu.run_frame());
    const std::string d2 = emu.diagnostics();
    CHECK(d2.find("\nhost ms per emulated s: ") != std::string::npos);
    CHECK(d2.find("GPU ") != std::string::npos && d2.find("DSP ") != std::string::npos);
    CHECK(d2.find("\nvideo mode word 800000CC=") != std::string::npos);
}
#endif

int main() { return orca::test::run_all("frontend"); }
