#pragma once
// Diagnostics: host time spent in a scope, added to a counter (nanoseconds).
#include "orca/core/bits.hpp"

#include <chrono>

namespace orca::core {

class HostTimer {
public:
    explicit HostTimer(u64& total) noexcept : total_(total), start_(std::chrono::steady_clock::now()) {}
    ~HostTimer() {
        total_ += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count());
    }
    HostTimer(const HostTimer&) = delete;
    HostTimer& operator=(const HostTimer&) = delete;

private:
    u64& total_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace orca::core
