// Orca Core — overwriting memory that held key material (through a volatile
// pointer, so the stores are not optimised away).
#pragma once
#include <cstddef>

namespace orca::crypto {

inline void secure_zero(void* p, std::size_t n) noexcept {
    volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) v[i] = 0;
}

} // namespace orca::crypto
