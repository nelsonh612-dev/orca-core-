// Real disc images, when the user provides them (none are part of Orca or of
// this repository). Environment:
//   ORCA_GC_IMAGE   a GameCube disc image you own (ISO/GCM, CISO, GCZ)
//   ORCA_WII_IMAGE  a Wii disc image you own (ISO, WBFS, CISO, GCZ)
//   ORCA_WII_KEYS   the common key from your own console (16-byte file, BootMii
//                   keys.bin, or a text key file), for encrypted Wii partitions
//   ORCA_REAL_FRAMES   video fields to run after the boot (default 300 = ~5 s)
//   ORCA_REAL_FULL_VERIFY=1  also decrypt and check every Wii cluster
// Without any image the test reports what it needs and exits with 77, which
// ctest shows as "Skipped" (never as passed).
//
// What is checked: the image opens, its file system, main DOL and apploader are
// read (Wii: from the decrypted game partition; signature status printed), the
// disc boots through the frontend (the same path as the Android app) and runs
// the requested number of fields; any stop is a failure and its reason printed.
#include "orca/disc/blob.hpp"
#include "orca/disc/fst.hpp"
#include "orca/disc/wii_disc.hpp"
#include "orca/frontend/emulator.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace orca;

namespace {

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? v : nullptr;
}

bool check_image(const std::string& path, const char* keys_file, bool full_verify) {
    std::string err;
    disc::ImageFormat format{};
    auto d = disc::open_image_file(path, &format, err);
    if (!d) { std::printf("  open: FAIL: %s\n", err.c_str()); return false; }
    const auto h = disc::parse_header(*d, err);
    if (!h) { std::printf("  header: FAIL: %s\n", err.c_str()); return false; }
    std::printf("  %s image, %s disc %s \"%s\"\n", disc::image_format_name(format), h->wii ? "Wii" : "GameCube", h->game_id.c_str(), h->name.c_str());
    std::unique_ptr<disc::WiiPartition> part;
    disc::DiscReader* volume = d.get();
    if (h->wii) {
        disc::WiiKeys keys;
        if (keys_file != nullptr && !disc::load_wii_key_file(keys_file, keys, 0, err)) { std::printf("  keys: FAIL: %s\n", err.c_str()); return false; }
        const auto parts = disc::list_wii_partitions(*d, err);
        if (!parts) { std::printf("  partitions: FAIL: %s\n", err.c_str()); return false; }
        for (const auto& p : *parts) {
            std::printf("  partition group %u at 0x%llX: %s\n", p.group, static_cast<unsigned long long>(p.offset), disc::wii_partition_type_name(p.type));
            if (p.type == 0U && !part) part = disc::WiiPartition::open(*d, p.offset, keys, err);
        }
        if (!part) { std::printf("  game partition: FAIL: %s\n", err.c_str()); return false; }
        std::printf("  signatures: %s\n", part->verify_signatures(keys.root ? &*keys.root : nullptr).detail.c_str());
        if (full_verify) {
            const bool ok = part->verify_all();
            std::printf("  H0-H3 of all %llu clusters: %s %s\n", static_cast<unsigned long long>(part->clusters_decrypted()), ok ? "OK" : "FAIL",
                        ok ? "" : part->last_error().c_str());
            if (!ok) return false;
        }
        volume = part.get();
    }
    const auto fs = disc::FileSystem::open(*volume, h->wii, err);
    if (!fs) { std::printf("  file system: FAIL: %s\n", err.c_str()); return false; }
    const auto dol = fs->main_dol(err);
    const auto app = fs->apploader(err);
    std::printf("  file system: %zu entries; main DOL %zu bytes; apploader %zu bytes\n", fs->entries().size(), dol ? dol->size() : 0U,
                app ? app->size() : 0U);
    if (!dol || !app) { std::printf("  FAIL: %s\n", err.c_str()); return false; }
    return true;
}

bool boot_and_run(const std::string& path, const char* keys_file, unsigned fields) {
    frontend::Settings s;
    s.jit = true; // used only where the host can run it (ARM64)
    const auto nand = std::filesystem::temp_directory_path() / "orca_real_image_nand";
    std::filesystem::remove_all(nand);
    s.nand_dir = nand.string();
    if (keys_file != nullptr) s.wii_key_file = keys_file;
    frontend::Emulator emu(s);
    std::string err;
    auto file = disc::FileDisc::open(path, err);
    if (!file || !emu.load_disc(std::move(file), err)) { std::printf("  boot: FAIL: %s\n", err.c_str()); return false; }
    std::printf("  boot: OK (%s)\n", emu.wii() ? "Wii" : "GameCube");
    unsigned ran = 0;
    while (ran < fields && emu.run_frame()) ++ran;
    const auto& f = emu.frame();
    const std::size_t lit = static_cast<std::size_t>(std::count_if(f.rgba.begin(), f.rgba.end(), [](u8 b) { return b > 16U; }));
    std::printf("  ran %u of %u fields (%.2f s of console time); last frame %ux%u, %zu bright bytes\n", ran, fields,
                static_cast<double>(emu.emulated_cycles()) / static_cast<double>(emu.cpu_hz()), f.width, f.height, lit);
    if (ran < fields) { std::printf("  stopped: %s\n", emu.error().c_str()); return false; }
    return true;
}

} // namespace

int main() {
    const char* gc = env("ORCA_GC_IMAGE");
    const char* wii = env("ORCA_WII_IMAGE");
    const char* keys = env("ORCA_WII_KEYS");
    const char* frames = env("ORCA_REAL_FRAMES");
    const bool full = env("ORCA_REAL_FULL_VERIFY") != nullptr;
    const unsigned fields = frames != nullptr ? static_cast<unsigned>(std::strtoul(frames, nullptr, 0)) : 300U;
    if (gc == nullptr && wii == nullptr) {
        std::printf("[real_images] SKIPPED: no real disc image. Set ORCA_GC_IMAGE to a GameCube disc you own and/or\n"
                    "[real_images] ORCA_WII_IMAGE to a Wii disc you own (plus ORCA_WII_KEYS with your console's common key\n"
                    "[real_images] for encrypted partitions). Formats: ISO/GCM, CISO, WBFS, GCZ.\n");
        return 77;
    }
    bool ok = true;
    for (const auto& [name, path] : {std::pair<const char*, const char*>{"GameCube", gc}, {"Wii", wii}}) {
        if (path == nullptr) continue;
        std::printf("[real_images] %s: %s\n", name, path);
        const bool good = check_image(path, keys, full) && boot_and_run(path, keys, fields);
        std::printf("[real_images] %s: %s\n", name, good ? "OK" : "FAILED");
        ok = ok && good;
    }
    return ok ? 0 : 1;
}
