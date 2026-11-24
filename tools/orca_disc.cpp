// orca_disc: inspects and verifies GameCube and Wii disc images with Orca's own
// readers, for checking real dumps.
//
// Usage: orca_disc [--wii-keys=FILE] info|verify|ls IMAGE
//        orca_disc [--wii-keys=FILE] extract IMAGE /path/in/disc OUTPUT
//   info    format, header, Wii partitions (type, offset, sizes, title, IOS,
//           common key index, header flags) and signature status
//   verify  reads the whole image: GCZ blocks (Adler-32), every Wii cluster of
//           every partition (H0-H3; needs the common key for encrypted ones), the
//           file system, main DOL and apploader
//   ls      the files of the disc (Wii: of the game partition)
//   extract one file
// Exit status 0 = everything checked is good, 1 = a check failed, 2 = usage.
#include "orca/disc/blob.hpp"
#include "orca/disc/fst.hpp"
#include "orca/disc/wii_disc.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::disc;

namespace {

int usage() {
    std::fprintf(stderr, "usage: orca_disc [--wii-keys=FILE] info|verify|ls IMAGE\n"
                         "       orca_disc [--wii-keys=FILE] extract IMAGE /path/in/disc OUTPUT\n");
    return 2;
}

std::string title_text(u64 id) {
    char b[40];
    std::snprintf(b, sizeof b, "%08X-%08X", static_cast<unsigned>(id >> 32), static_cast<unsigned>(id));
    std::string s = b;
    std::string ascii;
    for (int k = 3; k >= 0; --k) {
        const char c = static_cast<char>((id >> (8 * k)) & 0xFFU);
        ascii.push_back(c >= 0x20 && c < 0x7F ? c : '.');
    }
    return s + " (" + ascii + ")";
}

// The volume holding the file system: the disc itself, or the opened game partition.
struct Volume {
    std::unique_ptr<WiiPartition> part;
    DiscReader* reader{nullptr};
    bool wii{false};
};

bool open_volume(DiscReader& disc, const WiiKeys& keys, Volume& v, std::string& error) {
    const auto h = parse_header(disc, error);
    if (!h) return false;
    v.wii = h->wii;
    if (!h->wii) { v.reader = &disc; return true; }
    const auto parts = list_wii_partitions(disc, error);
    if (!parts) return false;
    for (const auto& p : *parts) {
        if (p.type != 0U) continue;
        v.part = WiiPartition::open(disc, p.offset, keys, error);
        if (!v.part) return false;
        v.reader = v.part.get();
        return true;
    }
    error = "the disc has no game partition";
    return false;
}

int info(DiscReader& disc, ImageFormat format, const WiiKeys& keys) {
    std::string err;
    const auto h = parse_header(disc, err);
    if (!h) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::printf("format:  %s\n", image_format_name(format));
    std::printf("console: %s\nID:      %s (disc %u, version %u)\nname:    %s\n", h->wii ? "Wii" : "GameCube", h->game_id.c_str(),
                h->disc_number, h->version, h->name.c_str());
    if (!h->wii) {
        std::printf("DOL at 0x%llX, FST at 0x%llX (%llu bytes)\n", static_cast<unsigned long long>(h->dol_offset),
                    static_cast<unsigned long long>(h->fst_offset), static_cast<unsigned long long>(h->fst_size));
        return 0;
    }
    const auto flags = read_wii_disc_flags(disc, err);
    if (flags) std::printf("hashes:  %s, encryption: %s\n", flags->hashes ? "yes" : "no", flags->encryption ? "yes" : "no");
    const auto parts = list_wii_partitions(disc, err);
    if (!parts) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    int status = 0;
    for (const auto& p : *parts) {
        std::printf("partition group %u at 0x%llX: %s (type %u)\n", p.group, static_cast<unsigned long long>(p.offset), wii_partition_type_name(p.type), p.type);
        auto part = WiiPartition::open(disc, p.offset, keys, err);
        if (!part) {
            std::printf("  cannot open: %s\n", err.c_str());
            if (p.type == 0U) status = 1;
            continue;
        }
        const Tmd& t = part->tmd_info();
        std::printf("  title %s, IOS %u, title version %u, group 0x%04X, %zu content(s)\n", title_text(t.title_id).c_str(),
                    static_cast<unsigned>(t.ios_id & 0xFFFFFFFFU), t.title_version, t.group_id, t.contents.size());
        std::printf("  ticket: issuer %s, common key index %u\n", part->ticket_info().sig.issuer.c_str(), part->ticket_info().common_key_index);
        std::printf("  data at 0x%llX, 0x%llX bytes on disc, 0x%llX bytes of user data\n", static_cast<unsigned long long>(part->data_offset()),
                    static_cast<unsigned long long>(part->data_size()), static_cast<unsigned long long>(part->size()));
        const ChainReport r = part->verify_signatures(keys.root ? &*keys.root : nullptr);
        std::printf("  signatures: %s\n", r.detail.c_str());
    }
    return status;
}

int verify(DiscReader& disc, const WiiKeys& keys) {
    std::string err;
    const auto h = parse_header(disc, err);
    if (!h) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    int status = 0;
    if (h->wii) {
        const auto parts = list_wii_partitions(disc, err);
        if (!parts) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        for (const auto& p : *parts) {
            auto part = WiiPartition::open(disc, p.offset, keys, err);
            if (!part) { std::printf("%s partition at 0x%llX: FAIL (%s)\n", wii_partition_type_name(p.type), static_cast<unsigned long long>(p.offset), err.c_str()); status = 1; continue; }
            const bool ok = part->flags().hashes ? part->verify_all() : true;
            std::printf("%s partition at 0x%llX: %s (%llu clusters decrypted and checked against H0-H3)%s%s\n", wii_partition_type_name(p.type),
                        static_cast<unsigned long long>(p.offset), ok ? "OK" : "FAIL", static_cast<unsigned long long>(part->clusters_decrypted()),
                        ok ? "" : ": ", ok ? "" : part->last_error().c_str());
            if (!ok) status = 1;
            const ChainReport r = part->verify_signatures(keys.root ? &*keys.root : nullptr);
            std::printf("  signatures: %s\n", r.detail.c_str());
        }
    } else {
        std::vector<u8> buf(1U << 20);
        for (u64 at = 0; at < disc.size(); at += buf.size()) {
            const std::size_t n = static_cast<std::size_t>(std::min<u64>(buf.size(), disc.size() - at));
            if (!disc.read(at, std::span<u8>(buf.data(), n))) {
                std::printf("image: FAIL at 0x%llX (%s)\n", static_cast<unsigned long long>(at), disc.failure_reason().c_str());
                return 1;
            }
        }
        std::printf("image: OK (0x%llX bytes read)\n", static_cast<unsigned long long>(disc.size()));
    }
    Volume v;
    if (!open_volume(disc, keys, v, err)) { std::printf("file system: FAIL (%s)\n", err.c_str()); return 1; }
    const auto fs = FileSystem::open(*v.reader, v.wii, err);
    if (!fs) { std::printf("file system: FAIL (%s)\n", err.c_str()); return 1; }
    u64 files = 0, bytes = 0;
    for (const FstEntry& e : fs->entries()) {
        if (e.directory) continue;
        ++files;
        bytes += e.size;
    }
    std::printf("file system: OK (%llu files, %llu bytes)\n", static_cast<unsigned long long>(files), static_cast<unsigned long long>(bytes));
    const auto dol = fs->main_dol(err);
    std::printf("main DOL: %s", dol ? "OK" : "FAIL");
    if (dol) std::printf(" (%zu bytes)\n", dol->size()); else std::printf(" (%s)\n", err.c_str());
    const auto app = fs->apploader(err);
    std::printf("apploader: %s", app ? "OK" : "FAIL");
    if (app) std::printf(" (%zu bytes)\n", app->size()); else std::printf(" (%s)\n", err.c_str());
    if (!dol || !app) status = 1;
    return status;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    WiiKeys keys;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--wii-keys=", 11) == 0) {
            std::string err;
            if (!load_wii_key_file(argv[i] + 11, keys, 0, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() < 2U) return usage();
    std::string err;
    ImageFormat format{};
    auto disc = open_image_file(args[1], &format, err);
    if (!disc) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const std::string& cmd = args[0];
    if (cmd == "info") return info(*disc, format, keys);
    if (cmd == "verify") return verify(*disc, keys);
    if (cmd == "ls" || cmd == "extract") {
        Volume v;
        if (!open_volume(*disc, keys, v, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        const auto fs = FileSystem::open(*v.reader, v.wii, err);
        if (!fs) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        if (cmd == "ls") {
            for (const FstEntry& e : fs->entries())
                if (e.directory) std::printf("%-56s <dir>\n", e.path.c_str());
                else std::printf("%-56s %10llu\n", e.path.c_str(), static_cast<unsigned long long>(e.size));
            return 0;
        }
        if (args.size() < 4U) return usage();
        const auto data = fs->read_file(args[2], err);
        if (!data) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::ofstream out(args[3], std::ios::binary);
        out.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
        if (!out) { std::fprintf(stderr, "cannot write %s\n", args[3].c_str()); return 1; }
        std::printf("%s: %zu bytes\n", args[2].c_str(), data->size());
        return 0;
    }
    return usage();
}
