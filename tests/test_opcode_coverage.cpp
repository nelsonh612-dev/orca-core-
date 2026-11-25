// Every instruction form in Dolphin's opcode tables is decoded by Orca's
// interpreter, except the ones listed below with the reason. Each form is executed
// once (operands 0) on a Wii system with paired singles enabled.
#include "orca/ppc/registers.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace orca;

namespace {
struct Op { u32 word; const char* name; };
const Op kOps[] = {
#include "opcode_forms.inc"
};
bool expected_missing(const char* name) {
    // dcba is not a Gekko/Broadway instruction (Dolphin asserts on it too);
    // mfspr/mtspr/mftb with SPR/TBR field 0 name no register (SPR 0 is not on the 750)
    for (const char* n : {"dcba", "mfspr", "mtspr", "mftb"}) if (std::strcmp(name, n) == 0) return true;
    return false;
}
} // namespace

TEST_CASE(every_dolphin_instruction_form_is_decoded) {
    unsigned decoded = 0;
    for (const Op& op : kOps) {
        system::SystemConfig cfg;
        cfg.console = system::Console::Wii;
        cfg.auto_keys = false;
        system::System sys(cfg);
        sys.hle_boot(0x80003000U);
        auto& s = sys.cpu().state();
        s.hid2 |= ppc::HID2_PSE | ppc::HID2_LSQE;
        for (auto& g : s.gpr) g = 0x80004000U; // loads and stores reach RAM
        CHECK(sys.bus().write32(0x3000, op.word) == mem::BusStatus::Ok);
        const auto r = sys.cpu().step();
        const std::string why = sys.cpu().last_stop().reason;
        const bool unknown = r == ppc::StepResult::UnsupportedInstruction &&
                             (why.find("unknown") != std::string::npos || why.find("unimplemented opcode") != std::string::npos);
        if (unknown && !expected_missing(op.name)) std::printf("    %s %08X: %s\n", op.name, op.word, why.c_str());
        CHECK(!unknown || expected_missing(op.name));
        if (!unknown) ++decoded;
    }
    std::printf("[opcode_coverage] %u of %zu forms decoded\n", decoded, sizeof kOps / sizeof kOps[0]);
}

TEST_MAIN("opcode_coverage")
