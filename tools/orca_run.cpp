// orca_run: headless runner. Loads a DOL/ELF, boots it (HLE) and runs for a
// number of cycles, then prints why it stopped and the CPU state.
// Usage: orca_run [--ipl-rom=FILE] [--rtc=SECONDS] [--dsp-irom=FILE] [--dsp-coef=FILE] [--card-a=FILE] [--card-b=FILE] [--dump-xfb=FILE.ppm] [--dump-audio=FILE.wav] [--dsp-hle] [--jit] [--list-files] file.(dol|elf|iso|gcm) [cycles] [gc|wii]
//   --ipl-rom: IPL ROM image (descrambled, as the CPU reads it) for EXI0/CS1
//   --rtc:     initial RTC counter value (seconds); default 0
//   --list-files: print the disc FST (.iso/.gcm) and exit
//   --dsp-irom / --dsp-coef: DSP ROM dumps (8 KiB / 4 KiB, big-endian words)
//   --card-a / --card-b: raw memory card image for slot A/B (4-128 Mbit). A
//                        missing file creates a blank 16 Mbit card; the image is
//                        written back after the run if the game changed it.
// .iso/.gcm images are booted through the HLE IPL + apploader path.
#include "orca/disc/fst.hpp"
#include "orca/system/system.hpp"
#include "orca/video/xfb.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc_in, char** argv_in) {
    std::vector<char*> args;
    std::string ipl_path;
    unsigned long rtc = 0;
    bool list_files = false, dsp_hle = false, use_jit = false;
    std::string irom_path, coef_path;
    std::string card_path[2];
    std::string xfb_path, audio_path;
    for (int i = 0; i < argc_in; ++i) {
        if (std::strncmp(argv_in[i], "--ipl-rom=", 10) == 0) ipl_path = argv_in[i] + 10;
        else if (std::strncmp(argv_in[i], "--rtc=", 6) == 0) rtc = std::strtoul(argv_in[i] + 6, nullptr, 0);
        else if (std::strcmp(argv_in[i], "--list-files") == 0) list_files = true;
        else if (std::strcmp(argv_in[i], "--dsp-hle") == 0) dsp_hle = true;
        else if (std::strcmp(argv_in[i], "--jit") == 0) use_jit = true;
        else if (std::strncmp(argv_in[i], "--dsp-irom=", 11) == 0) irom_path = argv_in[i] + 11;
        else if (std::strncmp(argv_in[i], "--dsp-coef=", 11) == 0) coef_path = argv_in[i] + 11;
        else if (std::strncmp(argv_in[i], "--card-a=", 9) == 0) card_path[0] = argv_in[i] + 9;
        else if (std::strncmp(argv_in[i], "--card-b=", 9) == 0) card_path[1] = argv_in[i] + 9;
        else if (std::strncmp(argv_in[i], "--dump-xfb=", 11) == 0) xfb_path = argv_in[i] + 11;
        else if (std::strncmp(argv_in[i], "--dump-audio=", 13) == 0) audio_path = argv_in[i] + 13;
        else args.push_back(argv_in[i]);
    }
    const int argc = static_cast<int>(args.size());
    char** argv = args.data();
    if (argc < 2) { std::fprintf(stderr, "usage: %s file.(dol|elf|iso|gcm) [cycles] [gc|wii]\n", argv[0]); return 2; }
    const unsigned long long cycles = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 486000000ULL;
    orca::system::SystemConfig cfg;
    cfg.console = (argc > 3 && std::strcmp(argv[3], "wii") == 0) ? orca::system::Console::Wii : orca::system::Console::GameCube;
    cfg.dsp_hle = dsp_hle;
    cfg.use_jit = use_jit;
    orca::system::System sys(cfg);
    std::string error;
    sys.ipl().set_rtc(static_cast<orca::u32>(rtc));
    if (!ipl_path.empty()) {
        std::ifstream rin(ipl_path, std::ios::binary);
        std::vector<orca::u8> rom((std::istreambuf_iterator<char>(rin)), std::istreambuf_iterator<char>());
        if (!sys.ipl().load_rom(std::move(rom))) { std::fprintf(stderr, "cannot use IPL ROM %s (empty or > 2 MiB)\n", ipl_path.c_str()); return 1; }
    }
    auto load_words = [](const std::string& f, std::size_t words, std::vector<orca::u16>& out) {
        std::ifstream in(f, std::ios::binary);
        const std::vector<orca::u8> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (b.size() != words * 2U) return false;
        out.resize(words);
        for (std::size_t i = 0; i < words; ++i) out[i] = static_cast<orca::u16>((b[2 * i] << 8) | b[2 * i + 1]);
        return true;
    };
    std::vector<orca::u16> words;
    if (!irom_path.empty() && !(load_words(irom_path, orca::dsp::kIromWords, words) && sys.dsp().core().load_irom(words))) {
        std::fprintf(stderr, "cannot use DSP IROM %s (must be 8192 bytes)\n", irom_path.c_str());
        return 1;
    }
    if (!coef_path.empty() && !(load_words(coef_path, orca::dsp::kCoefWords, words) && sys.dsp().core().load_coef(words))) {
        std::fprintf(stderr, "cannot use DSP COEF %s (must be 4096 bytes)\n", coef_path.c_str());
        return 1;
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (card_path[slot].empty()) continue;
        std::vector<orca::u8> img;
        orca::u32 mbit = 16;
        std::ifstream cin_(card_path[slot], std::ios::binary);
        if (cin_) {
            img.assign(std::istreambuf_iterator<char>(cin_), std::istreambuf_iterator<char>());
            mbit = static_cast<orca::u32>(img.size() / orca::hw::memcard::kBytesPerMbit);
        }
        if (!sys.insert_memory_card(slot, mbit, std::move(img), error)) {
            std::fprintf(stderr, "cannot use memory card %s: %s\n", card_path[slot].c_str(), error.c_str());
            return 1;
        }
    }
    const std::string path = argv[1];
    const auto ends_with = [&](const char* ext) { const std::size_t n = std::strlen(ext); return path.size() >= n && path.compare(path.size() - n, n, ext) == 0; };
    if (ends_with(".iso") || ends_with(".gcm") || ends_with(".ISO") || ends_with(".GCM")) {
        auto disc = orca::disc::FileDisc::open(path, error);
        if (!disc) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        std::string herr;
        const auto header = orca::disc::parse_header(*disc, herr);
        if (header) std::printf("disc: %s \"%s\"\n", header->game_id.c_str(), header->name.c_str());
        if (list_files) {
            if (!header) { std::fprintf(stderr, "%s\n", herr.c_str()); return 1; }
            std::string ferr;
            const auto fst = orca::disc::parse_fst(*disc, *header, ferr);
            if (!fst) { std::fprintf(stderr, "%s\n", ferr.c_str()); return 1; }
            for (const auto& e : *fst)
                if (e.directory) std::printf("%-48s <dir>\n", e.path.c_str());
                else std::printf("%-48s %10u bytes @ 0x%08X\n", e.path.c_str(), e.size, e.offset);
            return 0;
        }
        sys.insert_disc(std::move(disc));
        if (!sys.boot_disc(error)) { std::fprintf(stderr, "disc boot failed: %s\n", error.c_str()); return 1; }
        std::printf("apploader finished: game entry point %08X\n", sys.cpu().state().pc);
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        const std::vector<orca::u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!sys.load_executable(file, error)) { std::fprintf(stderr, "load failed: %s\n", error.c_str()); return 1; }
    }
    std::vector<orca::s16> audio;
    if (!audio_path.empty()) sys.mixer().set_sink([&audio](std::span<const orca::s16> f) { audio.insert(audio.end(), f.begin(), f.end()); });
    const auto r = sys.run(cycles);
    if (!audio_path.empty()) {
        // 16-bit stereo PCM WAV at 48 kHz, little-endian
        std::ofstream out(audio_path, std::ios::binary);
        auto le = [&out](orca::u32 v, int n) { for (int k = 0; k < n; ++k) out.put(static_cast<char>((v >> (8 * k)) & 0xFFU)); };
        const auto data = static_cast<orca::u32>(audio.size() * 2U);
        out.write("RIFF", 4); le(36U + data, 4); out.write("WAVEfmt ", 8); le(16, 4); le(1, 2); le(2, 2);
        le(orca::audio::kOutputRate, 4); le(orca::audio::kOutputRate * 4U, 4); le(4, 2); le(16, 2);
        out.write("data", 4); le(data, 4);
        for (const orca::s16 v : audio) le(static_cast<orca::u16>(v), 2);
        if (!out) std::fprintf(stderr, "cannot write %s\n", audio_path.c_str());
        else std::printf("audio: %zu frames at 48 kHz written to %s (DMA underruns %llu, streaming underruns %llu)\n", audio.size() / 2U,
                         audio_path.c_str(), static_cast<unsigned long long>(sys.mixer().dma_underruns()),
                         static_cast<unsigned long long>(sys.mixer().stream_underruns()));
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
        const auto* card = sys.memory_card(slot);
        if (card == nullptr || !card->dirty()) continue;
        std::ofstream out(card_path[slot], std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(card->image().data()), static_cast<std::streamsize>(card->image().size()));
        if (!out) std::fprintf(stderr, "cannot save memory card %s\n", card_path[slot].c_str());
        else std::printf("memory card %c saved: %s\n", slot == 0 ? 'A' : 'B', card_path[slot].c_str());
    }
    if (!xfb_path.empty()) {
        // the top field as the VI would scan it out, converted to RGB (binary PPM)
        const orca::hw::XfbField f = sys.vi().xfb_field(false);
        std::vector<orca::u8> raw(f.height == 0U ? 0U : static_cast<std::size_t>(f.height - 1U) * f.stride + f.width * 2U), rgba;
        if (f.width == 0U || f.height == 0U) std::fprintf(stderr, "XFB not configured by the program (width %u, height %u)\n", f.width, f.height);
        else if (sys.bus().dma_read(f.address & 0x03FFFFFFU, raw) != orca::mem::BusStatus::Ok) std::fprintf(stderr, "XFB at %08X is outside RAM\n", f.address);
        else if (!orca::video::decode_xfb(raw, f.width, f.height, f.stride, rgba)) std::fprintf(stderr, "cannot decode the XFB\n");
        else {
            std::ofstream out(xfb_path, std::ios::binary);
            out << "P6\n" << f.width << ' ' << f.height << "\n255\n";
            for (std::size_t i = 0; i < rgba.size(); i += 4) out.write(reinterpret_cast<const char*>(&rgba[i]), 3);
            if (!out) std::fprintf(stderr, "cannot write %s\n", xfb_path.c_str());
            else std::printf("XFB %ux%u at %08X written to %s\n", f.width, f.height, f.address, xfb_path.c_str());
        }
    }
    const auto& s = sys.cpu().state();
    std::printf("stop: %s after %llu cycles, %llu instructions\n", orca::ppc::to_string(r.stop),
                static_cast<unsigned long long>(r.cycles), static_cast<unsigned long long>(r.instructions));
    if (r.stop != orca::ppc::StepResult::Executed && r.stop != orca::ppc::StepResult::Halted)
        std::printf("reason: %s (pc=%08X ins=%08X addr=%08X)\n", sys.cpu().last_stop().reason.c_str(),
                    sys.cpu().last_stop().pc, sys.cpu().last_stop().instruction, sys.cpu().last_stop().address);
    if (r.device_stop) std::printf("device stop: %s\n", r.device_error.c_str());
    for (const auto& w : sys.mi().warnings()) std::printf("warning: %s\n", w.c_str());
    if (sys.dsp().hle_dsp() != nullptr)
        for (const auto& w : sys.dsp().hle_dsp()->warnings()) std::printf("warning: %s\n", w.c_str());
    std::printf("pc=%08X lr=%08X msr=%08X cr=%08X\n", s.pc, s.lr, s.msr, s.cr);
    for (unsigned i = 0; i < 32; i += 4)
        std::printf("r%-2u %08X  r%-2u %08X  r%-2u %08X  r%-2u %08X\n", i, s.gpr[i], i + 1, s.gpr[i + 1], i + 2, s.gpr[i + 2], i + 3, s.gpr[i + 3]);
    return 0;
}
