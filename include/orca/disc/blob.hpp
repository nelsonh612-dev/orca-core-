// Orca Core — disc image formats: the physical layer under the disc readers.
//
// Formats (Dolphin DiscIO/*Blob.cpp read as information; the code is Orca's):
//   ISO/GCM  raw 1:1 dumps (also NKit images, which are raw-compatible).
//   CISO     "CISO", block size (LE u32), 0x7FF8-byte map (1 = block stored);
//            stored blocks follow the 0x8000-byte header in order; others read 0.
//   WBFS     "WBFS", HD sector count (BE u32), HD sector shift, WBFS sector shift;
//            the disc's block table (BE u16 per WBFS sector, 0 = not stored) at
//            HD sector size + 0x100; one disc per file (split .wbf1 files and
//            multi-disc partitions are refused).
//   GCZ      0xB10BB10B (LE), sub type, compressed size (u64), disc size (u64),
//            block size, block count; then block pointers (u64, bit 63 = stored
//            uncompressed), Adler-32 of each stored block, the data. Needs zlib.
//   WIA, RVZ, TGC, NFS: recognised and refused with the reason (WIA/RVZ need
//            bzip2/LZMA/Zstandard; convert them to ISO with Dolphin).
// Reads are bounded by the image's data size; a block missing from a truncated
// file fails with a reason rather than returning made-up data.
#pragma once
#include "orca/disc/disc_image.hpp"

#include <memory>
#include <string>

namespace orca::disc {

enum class ImageFormat { Raw, NKit, Ciso, Wbfs, Gcz };
const char* image_format_name(ImageFormat f) noexcept;
bool gcz_supported() noexcept; // built with zlib

// Detects the format of `file` (the image file's bytes) and returns the reader of
// the disc it holds; also checks that the disc has the GameCube or Wii magic.
std::unique_ptr<DiscReader> open_image(std::unique_ptr<DiscReader> file, ImageFormat* format, std::string& error);
std::unique_ptr<DiscReader> open_image_file(const std::string& path, ImageFormat* format, std::string& error);
#if defined(__unix__) || defined(__APPLE__)
std::unique_ptr<DiscReader> open_image_fd(int fd, ImageFormat* format, std::string& error); // takes ownership of fd
#endif

} // namespace orca::disc
