// Orca Core: first native AArch64 PowerPC block translator.
// Supported integer, control-flow, and raw FPR/paired-single operations are
// emitted as native AArch64. Unsupported instructions remain in the reference
// interpreter. The generated function ABI is void(CpuState*).
#pragma once
#include "orca/ppc/state.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orca::ppc::jit {

struct Block {
    u32 start_pc{};
    u32 end_pc{};
    std::size_t guest_instructions{};
    bool requires_fp{false};
    bool requires_paired{false};
    bool has_guarded_memory{false};
    u32 guarded_memory_prefix{0};
    std::vector<u32> host_words; // little-endian AArch64 instruction words
};

// Decode a maximal straight-line prefix. Does not execute guest code.
Block compile_arm64(std::span<const u32> guest_words, u32 start_pc);

// Persistent W^X allocation for a translated block. The executable page is
// retained by the CPU block cache instead of mmap/munmap on every invocation.
class ExecutableBlock {
public:
    using MemoryResolver = void* (*)(CpuState*, void*, u32, u32, u32);
    ExecutableBlock() noexcept = default;
    explicit ExecutableBlock(const Block& block) noexcept;
    ~ExecutableBlock();
    ExecutableBlock(const ExecutableBlock&) = delete;
    ExecutableBlock& operator=(const ExecutableBlock&) = delete;
    ExecutableBlock(ExecutableBlock&& other) noexcept;
    ExecutableBlock& operator=(ExecutableBlock&& other) noexcept;
    bool valid() const noexcept { return memory_ != nullptr; }
    bool execute(CpuState& state, void* context = nullptr, MemoryResolver resolver = nullptr) const noexcept;
private:
    void* memory_{nullptr};
    std::size_t allocation_size_{0};
    bool has_guarded_memory_{false};
};

// Convenience one-shot entry point, primarily for tests. CPU execution uses
// ExecutableBlock so generated pages remain cached.
bool execute_arm64(const Block& block, CpuState& state) noexcept;

} // namespace orca::ppc::jit
