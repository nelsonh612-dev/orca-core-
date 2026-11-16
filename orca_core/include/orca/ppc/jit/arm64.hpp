// Orca Core: first native AArch64 PowerPC block translator.
// Only straight-line, side-effect-free integer instructions are emitted.
// Unsupported/control-flow instructions terminate a block and remain in the
// reference interpreter. The generated function ABI is void(CpuState*).
#pragma once
#include "orca/ppc/state.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orca::ppc { class Cpu; }

namespace orca::ppc::jit {

// Stable entry ABI for generated blocks. The first field is deliberately the
// architectural state pointer: generated instructions continue to address
// CpuState through x0 after the small entry thunk loads it. The owning Cpu is
// available for future checked memory/helper calls without changing the ABI.
struct ExecutionContext {
    CpuState* state{};
    Cpu* cpu{};
};

struct Block {
    u32 start_pc{};
    u32 end_pc{};
    std::size_t guest_instructions{};
    std::vector<u32> host_words; // little-endian AArch64 instruction words
};

// Decode a maximal straight-line prefix. Does not execute guest code.
Block compile_arm64(std::span<const u32> guest_words, u32 start_pc);

// Persistent W^X allocation for a translated block. The executable page is
// retained by the CPU block cache instead of mmap/munmap on every invocation.
class ExecutableBlock {
public:
    ExecutableBlock() noexcept = default;
    explicit ExecutableBlock(const Block& block) noexcept;
    ~ExecutableBlock();
    ExecutableBlock(const ExecutableBlock&) = delete;
    ExecutableBlock& operator=(const ExecutableBlock&) = delete;
    ExecutableBlock(ExecutableBlock&& other) noexcept;
    ExecutableBlock& operator=(ExecutableBlock&& other) noexcept;
    bool valid() const noexcept { return memory_ != nullptr; }
    bool execute(CpuState& state, Cpu* cpu = nullptr) const noexcept;
private:
    void* memory_{nullptr};
    std::size_t allocation_size_{0};
};

// Convenience one-shot entry point, primarily for tests. CPU execution uses
// ExecutableBlock so generated pages remain cached.
bool execute_arm64(const Block& block, CpuState& state, Cpu* cpu = nullptr) noexcept;

} // namespace orca::ppc::jit
