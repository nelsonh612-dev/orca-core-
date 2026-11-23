// orca_run: headless runner. Loads a DOL/ELF, boots it (HLE) and runs for a
// number of cycles, then prints why it stopped and the CPU state.
// Usage: orca_run file.(dol|elf|iso|gcm) [cycles] [gc|wii]
// .iso/.gcm images are booted through the HLE IPL + apploader path.
#include "orca/system/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s file.(dol|elf|iso|gcm) [cycles] [gc|wii]\n", argv[0]); return 2; }
    const unsigned long long cycles = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 486000000ULL;
    orca::system::SystemConfig cfg;
    cfg.console = (argc > 3 && std::strcmp(argv[3], "wii") == 0) ? orca::system::Console::Wii : orca::system::Console::GameCube;
    orca::system::System sys(cfg);
    std::string error;
    const std::string path = argv[1];
    const auto ends_with = [&](const char* ext) { const std::size_t n = std::strlen(ext); return path.size() >= n && path.compare(path.size() - n, n, ext) == 0; };
    if (ends_with(".iso") || ends_with(".gcm") || ends_with(".ISO") || ends_with(".GCM")) {
        auto disc = orca::disc::FileDisc::open(path, error);
        if (!disc) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        std::string herr;
        const auto header = orca::disc::parse_header(*disc, herr);
        if (header) std::printf("disc: %s \"%s\"\n", header->game_id.c_str(), header->name.c_str());
        sys.insert_disc(std::move(disc));
        if (!sys.boot_disc(error)) { std::fprintf(stderr, "disc boot failed: %s\n", error.c_str()); return 1; }
        std::printf("apploader finished: game entry point %08X\n", sys.cpu().state().pc);
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        const std::vector<orca::u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!sys.load_executable(file, error)) { std::fprintf(stderr, "load failed: %s\n", error.c_str()); return 1; }
    }
    const auto r = sys.run(cycles);
    const auto& s = sys.cpu().state();
    std::printf("stop: %s after %llu cycles, %llu instructions\n", orca::ppc::to_string(r.stop),
                static_cast<unsigned long long>(r.cycles), static_cast<unsigned long long>(r.instructions));
    if (r.stop != orca::ppc::StepResult::Executed && r.stop != orca::ppc::StepResult::Halted)
        std::printf("reason: %s (pc=%08X ins=%08X addr=%08X)\n", sys.cpu().last_stop().reason.c_str(),
                    sys.cpu().last_stop().pc, sys.cpu().last_stop().instruction, sys.cpu().last_stop().address);
    std::printf("pc=%08X lr=%08X msr=%08X cr=%08X\n", s.pc, s.lr, s.msr, s.cr);
    for (unsigned i = 0; i < 32; i += 4)
        std::printf("r%-2u %08X  r%-2u %08X  r%-2u %08X  r%-2u %08X\n", i, s.gpr[i], i + 1, s.gpr[i + 1], i + 2, s.gpr[i + 2], i + 3, s.gpr[i + 3]);
    return 0;
}
