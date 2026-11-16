// Verifies the test encoder (ppc_asm.hpp) against LLVM's PowerPC assembler.
// Usage: orca_encoding_check <path-to-llvm-mc>
// Exit code 77 (CTest "skipped") when llvm-mc is not available.
#include "ppc_asm.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace orca::test::ppc_asm;

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]).empty() || std::string(argv[1]).find("NOTFOUND") != std::string::npos) {
        std::puts("llvm-mc not available: encoding check SKIPPED");
        return 77;
    }
    const std::vector<std::pair<const char*, u32>> cases = {
        {"addi 3, 4, -2", addi(3, 4, -2)}, {"addis 3, 0, 0x7fff", addis(3, 0, 0x7fff)},
        {"addic 3, 4, -1", addic(3, 4, -1)}, {"addic. 3, 4, 5", addic_(3, 4, 5)},
        {"subfic 3, 4, 100", subfic(3, 4, 100)}, {"mulli 5, 6, -7", mulli(5, 6, -7)},
        {"ori 3, 4, 0x1234", ori(3, 4, 0x1234)}, {"oris 3, 4, 0xffff", oris(3, 4, 0xffff)},
        {"xori 3, 4, 1", xori(3, 4, 1)}, {"xoris 3, 4, 1", xoris(3, 4, 1)},
        {"andi. 3, 4, 0xff", andi_(3, 4, 0xff)}, {"andis. 3, 4, 0x8000", andis_(3, 4, 0x8000)},
        {"cmpwi 7, 3, -5", cmpwi(7, 3, -5)}, {"cmplwi 2, 3, 0xffff", cmplwi(2, 3, 0xffff)},
        {"twi 4, 3, 0", twi(4, 3, 0)}, {"tw 31, 0, 0", tw(31, 0, 0)},
        {"add 3, 4, 5", add(3, 4, 5)}, {"addo. 3, 4, 5", add(3, 4, 5, true, true)},
        {"addc 3, 4, 5", addc(3, 4, 5)}, {"addco 3, 4, 5", addc(3, 4, 5, true)},
        {"adde 3, 4, 5", adde(3, 4, 5)}, {"addeo. 3, 4, 5", adde(3, 4, 5, true, true)},
        {"addze 3, 4", addze(3, 4)}, {"addmeo 3, 4", addme(3, 4, true)},
        {"subf 3, 4, 5", subf(3, 4, 5)}, {"subfo. 3, 4, 5", subf(3, 4, 5, true, true)},
        {"subfc 3, 4, 5", subfc(3, 4, 5)}, {"subfe 3, 4, 5", subfe(3, 4, 5)},
        {"subfze 3, 4", subfze(3, 4)}, {"subfme. 3, 4", subfme(3, 4, false, true)},
        {"neg 3, 4", neg(3, 4)}, {"nego 3, 4", neg(3, 4, true)},
        {"mullw 3, 4, 5", mullw(3, 4, 5)}, {"mullwo. 3, 4, 5", mullw(3, 4, 5, true, true)},
        {"mulhw 3, 4, 5", mulhw(3, 4, 5)}, {"mulhwu. 3, 4, 5", mulhwu(3, 4, 5, true)},
        {"divw 3, 4, 5", divw(3, 4, 5)}, {"divwuo 3, 4, 5", divwu(3, 4, 5, true)},
        {"and 3, 4, 5", and_(3, 4, 5)}, {"andc. 3, 4, 5", andc(3, 4, 5, true)},
        {"or 3, 4, 5", or_(3, 4, 5)}, {"orc 3, 4, 5", orc(3, 4, 5)},
        {"xor 3, 4, 5", xor_(3, 4, 5)}, {"nand 3, 4, 5", nand(3, 4, 5)},
        {"nor 3, 4, 5", nor(3, 4, 5)}, {"eqv 3, 4, 5", eqv(3, 4, 5)},
        {"slw 3, 4, 5", slw(3, 4, 5)}, {"srw. 3, 4, 5", srw(3, 4, 5, true)},
        {"sraw 3, 4, 5", sraw(3, 4, 5)}, {"srawi 3, 4, 31", srawi(3, 4, 31)},
        {"cntlzw 3, 4", cntlzw(3, 4)}, {"extsb 3, 4", extsb(3, 4)}, {"extsh. 3, 4", extsh(3, 4, true)},
        {"cmpw 1, 3, 4", cmpw(1, 3, 4)}, {"cmplw 6, 3, 4", cmplw(6, 3, 4)},
        {"rlwinm 3, 4, 5, 6, 7", rlwinm(3, 4, 5, 6, 7)}, {"rlwimi. 3, 4, 31, 0, 31", rlwimi(3, 4, 31, 0, 31, true)},
        {"rlwnm 3, 4, 5, 28, 3", rlwnm(3, 4, 5, 28, 3)},
        {"lwz 3, 129(4)", lwz(3, 129, 4)}, {"lwzu 3, -4(1)", lwzu(3, -4, 1)},
        {"lbz 3, 0(0)", lbz(3, 0, 0)}, {"lbzu 3, 1(4)", lbzu(3, 1, 4)},
        {"stw 3, 8(1)", stw(3, 8, 1)}, {"stwu 1, -16(1)", stwu(1, -16, 1)},
        {"stb 3, 0(4)", stb(3, 0, 4)}, {"stbu 3, 1(4)", stbu(3, 1, 4)},
        {"lhz 3, 2(4)", lhz(3, 2, 4)}, {"lhzu 3, 2(4)", lhzu(3, 2, 4)},
        {"lha 3, 2(4)", lha(3, 2, 4)}, {"lhau 3, 2(4)", lhau(3, 2, 4)},
        {"sth 3, 2(4)", sth(3, 2, 4)}, {"sthu 3, 2(4)", sthu(3, 2, 4)},
        {"lmw 29, 8(1)", lmw(29, 8, 1)}, {"stmw 29, 8(1)", stmw(29, 8, 1)},
        {"lfs 1, 0(3)", lfs(1, 0, 3)},
        {"lwzx 3, 4, 5", lwzx(3, 4, 5)}, {"lwzux 3, 4, 5", lwzux(3, 4, 5)},
        {"lbzx 3, 4, 5", lbzx(3, 4, 5)}, {"lhax 3, 4, 5", lhax(3, 4, 5)},
        {"stwx 3, 4, 5", stwx(3, 4, 5)}, {"stbux 3, 4, 5", stbux(3, 4, 5)},
        {"lwbrx 3, 4, 5", lwbrx(3, 4, 5)}, {"lhbrx 3, 4, 5", lhbrx(3, 4, 5)},
        {"stwbrx 3, 4, 5", stwbrx(3, 4, 5)}, {"sthbrx 3, 4, 5", sthbrx(3, 4, 5)},
        {"lwarx 3, 4, 5", lwarx(3, 4, 5)}, {"stwcx. 3, 4, 5", stwcx_(3, 4, 5)},
        {"lswi 5, 4, 7", lswi(5, 4, 7)}, {"stswi 5, 4, 0", stswi(5, 4, 0)},
        {"ba 0x100", b(0x100, true)},
        {"bclr 20, 0", bclr(20, 0)}, {"bclrl 12, 1", bclr(12, 1, true)},
        {"bcctr 20, 0", bcctr(20, 0)}, {"bcctrl 4, 6", bcctr(4, 6, true)},
        {"crand 1, 2, 3", crand(1, 2, 3)}, {"cror 4, 5, 6", cror(4, 5, 6)},
        {"crxor 7, 8, 9", crxor(7, 8, 9)}, {"crnand 1, 2, 3", crnand(1, 2, 3)},
        {"crnor 1, 2, 3", crnor(1, 2, 3)}, {"creqv 1, 2, 3", creqv(1, 2, 3)},
        {"crandc 1, 2, 3", crandc(1, 2, 3)}, {"crorc 1, 2, 3", crorc(1, 2, 3)},
        {"mcrf 2, 5", mcrf(2, 5)}, {"mfcr 3", mfcr(3)}, {"mtcrf 0x81, 3", mtcrf(0x81, 3)},
        {"mfspr 3, 8", mfspr(3, 8)}, {"mtspr 9, 3", mtspr(9, 3)}, {"mfspr 3, 1008", mfspr(3, 1008)},
        {"mtspr 528, 4", mtspr(528, 4)}, {"mfspr 5, 287", mfspr(5, 287)}, {"mtspr 22, 6", mtspr(22, 6)},
        {"mfmsr 3", mfmsr(3)}, {"mtmsr 3", mtmsr(3)},
        {"mtsr 5, 3", mtsr(5, 3)}, {"mfsr 3, 15", mfsr(3, 15)},
        {"mtsrin 3, 4", mtsrin(3, 4)}, {"mfsrin 3, 4", mfsrin(3, 4)},
        {"sc", sc()}, {"rfi", rfi()}, {"isync", isync()}, {"sync", sync()}, {"eieio", eieio()},
        {"tlbie 4", tlbie(4)}, {"dcbz 3, 4", dcbz(3, 4)}, {"dcbf 3, 4", dcbf(3, 4)},
        {"dcbst 3, 4", dcbst(3, 4)}, {"dcbt 3, 4", dcbt(3, 4)}, {"dcbi 3, 4", dcbi(3, 4)},
        {"icbi 3, 4", icbi(3, 4)},
        // Floating point
        {"fadd 1, 2, 3", fadd(1, 2, 3)}, {"fadds. 1, 2, 3", fadds(1, 2, 3, true)}, {"fsub 4, 5, 6", fsub(4, 5, 6)},
        {"fsubs 4, 5, 6", fsubs(4, 5, 6)}, {"fmul 1, 2, 3", fmul(1, 2, 3)}, {"fmuls 1, 2, 3", fmuls(1, 2, 3)},
        {"fdiv 1, 2, 3", fdiv(1, 2, 3)}, {"fdivs 1, 2, 3", fdivs(1, 2, 3)},
        {"fmadd 1, 2, 3, 4", fmadd(1, 2, 3, 4)}, {"fmadds 1, 2, 3, 4", fmadds(1, 2, 3, 4)},
        {"fmsub 1, 2, 3, 4", fmsub(1, 2, 3, 4)}, {"fmsubs. 1, 2, 3, 4", fmsubs(1, 2, 3, 4, true)},
        {"fnmadd 1, 2, 3, 4", fnmadd(1, 2, 3, 4)}, {"fnmadds 1, 2, 3, 4", fnmadds(1, 2, 3, 4)},
        {"fnmsub 1, 2, 3, 4", fnmsub(1, 2, 3, 4)}, {"fnmsubs 1, 2, 3, 4", fnmsubs(1, 2, 3, 4)},
        {"fsel 1, 2, 3, 4", fsel(1, 2, 3, 4)}, {"fres 1, 2", fres(1, 2)}, {"frsqrte 1, 2", frsqrte(1, 2)},
        {"fsqrt 1, 2", fsqrt(1, 2)}, {"fcmpu 3, 1, 2", fcmpu(3, 1, 2)}, {"fcmpo 7, 1, 2", fcmpo(7, 1, 2)},
        {"frsp 1, 2", frsp(1, 2)}, {"fctiw 1, 2", fctiw(1, 2)}, {"fctiwz. 1, 2", fctiwz(1, 2, true)},
        {"fneg 1, 2", fneg(1, 2)}, {"fmr 1, 2", fmr(1, 2)}, {"fnabs 1, 2", fnabs(1, 2)}, {"fabs 1, 2", fabs_(1, 2)},
        {"mffs 5", mffs(5)}, {"mtfsf 255, 3", mtfsf(255, 3)}, {"mtfsfi 7, 3", mtfsfi(7, 3)},
        {"mtfsb0 24", mtfsb0(24)}, {"mtfsb1 3", mtfsb1(3)}, {"mcrfs 2, 5", mcrfs(2, 5)},
        {"lfs 1, 0(3)", lfs(1, 0, 3)}, {"lfsu 1, 4(3)", lfsu(1, 4, 3)}, {"lfd 1, -8(3)", lfd(1, -8, 3)},
        {"lfdu 1, 8(3)", lfdu(1, 8, 3)}, {"stfs 1, 0(3)", stfs(1, 0, 3)}, {"stfsu 1, 4(3)", stfsu(1, 4, 3)},
        {"stfd 1, 8(3)", stfd(1, 8, 3)}, {"stfdu 1, 8(3)", stfdu(1, 8, 3)},
        {"lfsx 1, 3, 4", lfsx(1, 3, 4)}, {"lfsux 1, 3, 4", lfsux(1, 3, 4)}, {"lfdx 1, 3, 4", lfdx(1, 3, 4)},
        {"lfdux 1, 3, 4", lfdux(1, 3, 4)}, {"stfsx 1, 3, 4", stfsx(1, 3, 4)}, {"stfsux 1, 3, 4", stfsux(1, 3, 4)},
        {"stfdx 1, 3, 4", stfdx(1, 3, 4)}, {"stfdux 1, 3, 4", stfdux(1, 3, 4)}, {"stfiwx 1, 3, 4", stfiwx(1, 3, 4)},
    };

    // Relative branches assemble to relocations, so they are verified in the
    // other direction: LLVM disassembles our encoding.
    const std::vector<std::pair<const char*, u32>> disasm_cases = {
        {"b .+8", b(8)}, {"bl .-4", b(-4, false, true)},
        {"bt 2, .+16", bc(12, 2, 16)}, {"bdnzl .-8", bc(16, 0, -8, false, true)},
        {"bdz .+32", bc(18, 0, 32)}, {"bf 7, .-32768", bc(4, 7, -32768)},
        // LLVM assembles "mftb" as the mfspr 268 alias; the 750CL form is XO 371.
        {"mftb 3, 268", mftb(3)}, {"mftbu 3", mftb(3, 269)},
    };
    // Valid 750CL instructions that LLVM's PowerPC backend does not implement.
    // Checked by hand against the PEM opcode tables; reported, not hidden.
    const std::vector<std::pair<const char*, u32>> unverifiable = {
        {"lswx 5, 3, 4 (XO 533)", lswx(5, 3, 4)}, {"stswx 5, 3, 4 (XO 661)", stswx(5, 3, 4)},
        {"mcrxr 3 (XO 512)", mcrxr(3)},
        {"ps_add 1, 2, 3 (op4 A-form 21)", ps_add(1, 2, 3)}, {"ps_madd 1, 2, 3, 4 (op4 A 29)", ps_madd(1, 2, 3, 4)},
        {"ps_sum0 1, 2, 3, 4 (op4 A 10)", ps_sum0(1, 2, 3, 4)}, {"ps_merge10 1, 2, 3 (op4 X 592)", ps_merge10(1, 2, 3)},
        {"ps_cmpo1 cr1, 2, 3 (op4 X 96)", ps_cmpo1(1, 2, 3)}, {"dcbz_l 3, 4 (op4 X 1014)", dcbz_l(3, 4)},
        {"psq_l 1, -8(3), 0, 5 (op 56)", psq_l(1, -8, 3, 0, 5)}, {"psq_stux 1, 3, 4, 1, 7 (op4 39)", psq_stux(1, 3, 4, 1, 7)},
    };

    const char* asm_path = "orca_encoding_check.s";
    std::FILE* f = std::fopen(asm_path, "w");
    if (!f) { std::perror("fopen"); return 1; }
    for (auto& c : cases) std::fprintf(f, "%s\n", c.first);
    std::fclose(f);

    const std::string cmd = std::string(argv[1]) + " -triple=powerpc-unknown-linux-gnu -mcpu=750 -show-encoding " + asm_path + " 2>&1";
    std::FILE* p = popen(cmd.c_str(), "r");
    if (!p) { std::perror("popen"); return 1; }
    std::vector<u32> got;
    char line[512];
    while (std::fgets(line, sizeof line, p)) {
        const std::string s(line);
        const auto pos = s.find("encoding: [");
        if (pos == std::string::npos) {
            if (s.find("error") != std::string::npos) std::fprintf(stderr, "llvm-mc: %s", line);
            continue;
        }
        unsigned b0, b1, b2, b3;
        if (std::sscanf(s.c_str() + pos, "encoding: [0x%x,0x%x,0x%x,0x%x]", &b0, &b1, &b2, &b3) == 4)
            got.push_back((b0 << 24) | (b1 << 16) | (b2 << 8) | b3);
    }
    pclose(p);

    int failures = 0;
    if (got.size() != cases.size()) {
        std::fprintf(stderr, "llvm-mc produced %zu encodings for %zu instructions\n", got.size(), cases.size());
        return 1;
    }
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (got[i] != cases[i].second) {
            std::fprintf(stderr, "MISMATCH %-28s llvm-mc=%08x orca=%08x\n", cases[i].first, got[i], cases[i].second);
            ++failures;
        }
    }
    // Disassembly direction
    const char* dis_path = "orca_encoding_check.hex";
    f = std::fopen(dis_path, "w");
    if (!f) { std::perror("fopen"); return 1; }
    for (auto& c : disasm_cases)
        std::fprintf(f, "0x%02x 0x%02x 0x%02x 0x%02x\n", c.second >> 24, (c.second >> 16) & 0xFF, (c.second >> 8) & 0xFF, c.second & 0xFF);
    std::fclose(f);
    const std::string dcmd = std::string(argv[1]) + " --disassemble -triple=powerpc-unknown-linux-gnu -mcpu=750 " + dis_path + " 2>&1";
    p = popen(dcmd.c_str(), "r");
    if (!p) { std::perror("popen"); return 1; }
    std::vector<std::string> texts;
    while (std::fgets(line, sizeof line, p)) {
        std::string s(line);
        if (s.empty() || s[0] != '\t' || s.find(".text") != std::string::npos) continue;
        std::string norm;
        for (char ch : s) {
            if (ch == '\n') continue;
            if (ch == '\t' || ch == ' ') { if (!norm.empty() && norm.back() != ' ') norm += ' '; }
            else norm += ch;
        }
        while (!norm.empty() && norm.back() == ' ') norm.pop_back();
        texts.push_back(norm);
    }
    pclose(p);
    if (texts.size() != disasm_cases.size()) {
        std::fprintf(stderr, "llvm-mc disassembled %zu of %zu words\n", texts.size(), disasm_cases.size());
        return 1;
    }
    for (std::size_t i = 0; i < disasm_cases.size(); ++i) {
        if (texts[i] != disasm_cases[i].first) {
            std::fprintf(stderr, "DISASM MISMATCH %08x llvm-mc='%s' expected='%s'\n", disasm_cases[i].second, texts[i].c_str(), disasm_cases[i].first);
            ++failures;
        }
    }
    for (auto& u : unverifiable) std::printf("[encodings] NOT VERIFIABLE WITH LLVM: %-26s %08x\n", u.first, u.second);
    std::printf("[encodings] %zu assembled + %zu disassembled checked against llvm-mc, %d mismatches, %zu unverifiable\n",
                cases.size(), disasm_cases.size(), failures, unverifiable.size());
    return failures == 0 ? 0 : 1;
}
