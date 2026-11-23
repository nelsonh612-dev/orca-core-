// Orca Core — executable memory for the JIT. See code_buffer.hpp.
#include "orca/jit/code_buffer.hpp"

#include <cerrno>
#include <cstring>
#include <sys/mman.h>

namespace orca::jit {

CodeBuffer::CodeBuffer(std::size_t bytes) : size_(bytes) {
    void* p = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        error_ = std::string("mmap failed: ") + std::strerror(errno);
        size_ = 0;
        return;
    }
    base_ = static_cast<u8*>(p);
    writable_ = true;
}

CodeBuffer::~CodeBuffer() {
    if (base_ != nullptr) munmap(base_, size_);
}

bool CodeBuffer::set_writable() {
    if (writable_) return true;
    if (mprotect(base_, size_, PROT_READ | PROT_WRITE) != 0) {
        error_ = std::string("mprotect(RW) failed: ") + std::strerror(errno);
        return false;
    }
    writable_ = true;
    return true;
}

const u32* CodeBuffer::append(std::span<const u32> words) {
    if (base_ == nullptr || !set_writable()) return nullptr;
    const std::size_t start = (used_ + 15U) & ~std::size_t{15};
    const std::size_t bytes = words.size() * sizeof(u32);
    if (start + bytes > size_) { error_ = "code buffer full"; return nullptr; }
    std::memcpy(base_ + start, words.data(), bytes);
    used_ = start + bytes;
    return reinterpret_cast<const u32*>(base_ + start);
}

bool CodeBuffer::make_executable() {
    if (base_ == nullptr) return false;
    if (!writable_) return true;
    if (mprotect(base_, size_, PROT_READ | PROT_EXEC) != 0) {
        error_ = std::string("mprotect(RX) failed: ") + std::strerror(errno);
        return false;
    }
    writable_ = false;
    if (used_ > flushed_) {
        __builtin___clear_cache(reinterpret_cast<char*>(base_ + flushed_), reinterpret_cast<char*>(base_ + used_));
        flushed_ = used_;
    }
    return true;
}

bool CodeBuffer::patch(const u32* at, u32 word) {
    if (base_ == nullptr) return false;
    const auto* p = reinterpret_cast<const u8*>(at);
    if (p < base_ || p + 4 > base_ + used_) return false;
    const bool was_executable = !writable_;
    if (!set_writable()) return false;
    u32* w = reinterpret_cast<u32*>(base_ + (p - base_));
    *w = word;
    if (was_executable) {
        // the new word must reach the instruction side before the code runs again
        __builtin___clear_cache(reinterpret_cast<char*>(w), reinterpret_cast<char*>(w + 1));
        return make_executable();
    }
    return true;
}

void CodeBuffer::reset() {
    used_ = 0;
    flushed_ = 0;
}

} // namespace orca::jit
