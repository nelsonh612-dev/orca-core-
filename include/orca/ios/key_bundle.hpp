// Orca Core — the build-time key bundle: default values for OrcaIOSC's entries,
// compiled into the binary the way Dolphin compiles its default entries.
//
// The repository holds no values. A build gets them only when the builder passes
// a file with -DORCA_DEFAULT_KEYS_FILE=/path (CMake; the Android build forwards
// the environment variable ORCA_DEFAULT_KEYS_FILE): any form
// disc::parse_wii_keys accepts (text "common_key = <hex>" / "korean_common_key"
// / "vwii_common_key" / "dev_common_key" lines, a 16-byte key, or a BootMii
// keys.bin, whose common key alone is used). The file is embedded at configure
// time into a source generated in the build directory; supplying values one is
// entitled to is the builder's responsibility.
#pragma once
#include "orca/core/bits.hpp"

#include <span>

namespace orca::ios {

// The bundle's bytes (empty when the build has none).
std::span<const u8> build_time_key_bundle() noexcept;
// The file name the bundle was made from (empty when none), for reports.
const char* build_time_key_bundle_origin() noexcept;

} // namespace orca::ios
