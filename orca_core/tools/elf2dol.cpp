// elf2dol: converts a big-endian PowerPC ELF into a GameCube/Wii DOL.
// Usage: orca_elf2dol input.elf output.dol
#include "orca/loader/executable.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s input.elf output.dol\n", argv[0]); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    const std::vector<orca::u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    const auto image = orca::loader::parse_elf(file, error);
    if (!image) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    const auto dol = orca::loader::make_dol(*image, error);
    if (!dol) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(dol->data()), static_cast<std::streamsize>(dol->size()));
    if (!out) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    std::printf("%s: %zu segments, entry 0x%08X, %zu bytes\n", argv[2], image->segments.size(), image->entry, dol->size());
    return 0;
}
