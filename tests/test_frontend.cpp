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
