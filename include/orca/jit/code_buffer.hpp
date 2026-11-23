// Orca Core — executable memory for the JIT (W^X).
//
// The buffer is mapped read+write while code is appended and switched to
// read+execute before running it (mprotect), with the instruction cache made
// coherent for the new bytes. It never is writable and executable at once, which
// also satisfies Android. Code can only run when the host is AArch64
// (host_can_execute()); on other hosts the buffer still holds the words for tests.
#pragma once
#include "orca/core/bits.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace orca::jit {

constexpr bool host_can_execute() noexcept {
#if defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

class CodeBuffer {
public:
    explicit CodeBuffer(std::size_t bytes = 32U << 20);
    ~CodeBuffer();
    CodeBuffer(const CodeBuffer&) = delete;
    CodeBuffer& operator=(const CodeBuffer&) = delete;

    bool ok() const noexcept { return base_ != nullptr; }
    const std::string& error() const noexcept { return error_; }
    std::size_t capacity() const noexcept { return size_; }
    std::size_t used() const noexcept { return used_; }

    // Copies `words` at the end (16-byte aligned start); returns its address or
    // nullptr when full. The buffer is writable afterwards until make_executable().
    const u32* append(std::span<const u32> words);
    // Read+execute for the whole buffer and instruction-cache maintenance for the
    // bytes appended since the last call.
    bool make_executable();
    // Replaces one instruction of code already appended (block linking) and makes
    // it coherent; the buffer returns to the protection it had (so code that is
    // running and called a helper can continue).
    bool patch(const u32* at, u32 word);
    // Drops all code (the caller must forget every pointer it got).
    void reset();

private:
    bool set_writable();
    u8* base_{nullptr};
    std::size_t size_{0}, used_{0}, flushed_{0};
    bool writable_{false};
    std::string error_;
};

} // namespace orca::jit
