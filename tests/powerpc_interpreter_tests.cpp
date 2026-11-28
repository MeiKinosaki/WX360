#include "onix360/cpu/powerpc_interpreter.hpp"

#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
constexpr std::uint32_t branch_form(unsigned op, unsigned bo, unsigned bi, unsigned displacement, bool link = false) {
    return (op << 26U) | ((bo & 31U) << 21U) | ((bi & 31U) << 16U) |
           (displacement & 0xFFFCU) | (link ? 1U : 0U);
}
constexpr std::uint32_t d_form(unsigned op, unsigned rt, unsigned ra, unsigned imm) {
    return (op << 26U) | (rt << 21U) | (ra << 16U) | (imm & 0xFFFFU);
}
constexpr std::uint32_t compare_form(unsigned bf, bool is64, unsigned ra, unsigned rb, unsigned xo) {
    return (31U << 26U) | ((bf & 7U) << 23U) | (static_cast<unsigned>(is64) << 21U) |
           (ra << 16U) | (rb << 11U) | (xo << 1U);
}
constexpr std::uint32_t rlwinm_form(unsigned rs, unsigned ra, unsigned sh, unsigned mb, unsigned me, bool rc = false) {
    return (21U << 26U) | (rs << 21U) | (ra << 16U) | (sh << 11U) |
           (mb << 6U) | (me << 1U) | (rc ? 1U : 0U);
}
constexpr std::uint32_t x_form(unsigned rt, unsigned ra, unsigned rb, unsigned xo, bool rc = false) {
    return (31U << 26U) | (rt << 21U) | (ra << 16U) |
           (rb << 11U) | (xo << 1U) | (rc ? 1U : 0U);
}
}

int main() {
    const auto vx_form = [](unsigned vd, unsigned va, unsigned vb, unsigned xo) {
        return (4U << 26U) | (vd << 21U) | (va << 16U) |
               (vb << 11U) | (xo & 0x7FFU);
    };
    using namespace onix360::cpu;
    constexpr std::uint64_t xer_ca = std::uint64_t{1} << 29U;
    XenonThreadState s;
    s.reset(0x1000);
    // Initial double-precision FPU arithmetic coverage.
    s.fpr[1] = std::bit_cast<std::uint64_t>(1.5);
    s.fpr[2] = std::bit_cast<std::uint64_t>(2.25);
    const std::uint32_t fadd = (63U << 26U) | (3U << 21U) | (1U << 16U) | (2U << 11U) | (21U << 1U);
    check(execute(s, fadd) == StepResult::Executed, "FADD double executes");
    check(std::bit_cast<double>(s.fpr[3]) == 3.75, "FADD writes IEEE-754 result");

    // Single-precision A-form operations round operands and results to binary32.
    s.fpr[1] = std::bit_cast<std::uint64_t>(1.25);
    s.fpr[2] = std::bit_cast<std::uint64_t>(2.5);
    const std::uint32_t fadds = (59U << 26U) | (7U << 21U) |
        (1U << 16U) | (2U << 11U) | (21U << 1U);
    check(execute(s, fadds) == StepResult::Executed, "FADDS executes");
    check(s.fpr[7] == std::bit_cast<std::uint64_t>(
              static_cast<double>(static_cast<float>(3.75))),
          "FADDS stores the binary32-rounded result in the FPR");

    s.fpr[1] = std::bit_cast<std::uint64_t>(1.5);
    s.fpr[2] = std::bit_cast<std::uint64_t>(2.25);
    const std::uint32_t fmuls = (59U << 26U) | (8U << 21U) |
        (1U << 16U) | (25U << 1U) | (2U << 6U);
    check(execute(s, fmuls) == StepResult::Executed, "FMULS executes using FRC");
    check(s.fpr[8] == std::bit_cast<std::uint64_t>(
              static_cast<double>(static_cast<float>(3.375))),
          "FMULS reads FRC and stores a binary32-rounded result");

    s.fpr[1] = std::bit_cast<std::uint64_t>(2.0);
    s.fpr[2] = std::bit_cast<std::uint64_t>(11.0);
    s.fpr[3] = std::bit_cast<std::uint64_t>(-7.0);
    const std::uint32_t fsel = (63U << 26U) | (9U << 21U) |
        (1U << 16U) | (2U << 11U) | (3U << 6U) | (23U << 1U);
    check(execute(s, fsel) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[9]) == 11.0,
          "FSEL selects FRB when FRA is nonnegative");
    s.fpr[1] = std::bit_cast<std::uint64_t>(-2.0);
    check(execute(s, fsel) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[9]) == -7.0,
          "FSEL selects FRC when FRA is negative");
    s.fpr[1] = std::bit_cast<std::uint64_t>(1.5);
    s.fpr[2] = std::bit_cast<std::uint64_t>(2.25);
    s.fpscr = 0xA0000000U;
    check(execute(s, fadd | 1U) == StepResult::Executed, "FADD. executes with Rc set");
    check(s.condition_field(1) == 0xA,
          "FADD. copies FPSCR[0:3] into CR1");
    const std::uint32_t fsub = (63U << 26U) | (4U << 21U) | (2U << 16U) | (1U << 11U) | (20U << 1U);
    check(execute(s, fsub) == StepResult::Executed, "FSUB double executes");
    check(std::bit_cast<double>(s.fpr[4]) == 0.75, "FSUB computes FRA minus FRB");
    const std::uint32_t fdiv = (63U << 26U) | (5U << 21U) | (2U << 16U) | (1U << 11U) | (18U << 1U);
    check(execute(s, fdiv) == StepResult::Executed, "FDIV double executes");
    check(std::bit_cast<double>(s.fpr[5]) == 1.5, "FDIV computes FRA divided by FRB");
    s.fpr[1] = std::bit_cast<std::uint64_t>(1.5);
    s.fpr[6] = std::bit_cast<std::uint64_t>(4.0);
    const std::uint32_t fmul = (63U << 26U) | (7U << 21U) | (1U << 16U) | (6U << 6U) | (25U << 1U);
    check(execute(s, fmul) == StepResult::Executed, "FMUL double executes");
    check(std::bit_cast<double>(s.fpr[7]) == 6.0, "FMUL uses FRC operand");
    s.fpr[1] = std::bit_cast<std::uint64_t>(2.0);
    s.fpr[2] = std::bit_cast<std::uint64_t>(4.0);
    s.fpr[3] = std::bit_cast<std::uint64_t>(3.0);
    const auto double_fma = [](unsigned frt, unsigned fra, unsigned frb,
                               unsigned frc, unsigned xo) {
        return (63U << 26U) | (frt << 21U) | (fra << 16U) |
               (frb << 11U) | (frc << 6U) | (xo << 1U);
    };
    check(execute(s, double_fma(10,1,2,3,29)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[10]) == 10.0, "FMADD uses fused multiply-add");
    check(execute(s, double_fma(11,1,2,3,28)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[11]) == 2.0, "FMSUB uses fused multiply-subtract");
    check(execute(s, double_fma(12,1,2,3,31)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[12]) == -10.0, "FNMADD negates fused multiply-add");
    check(execute(s, double_fma(13,1,2,3,30)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[13]) == -2.0, "FNMSUB negates fused multiply-subtract");
    s.fpscr = 0xA1234567U;
    const std::uint32_t mffs = (63U << 26U) | (18U << 21U) | (583U << 1U) | 1U;
    check(execute(s, mffs) == StepResult::Executed &&
          s.fpr[18] == 0xA1234567U && s.condition_field(1) == 0xAU,
          "MFFS copies FPSCR to FPR and updates CR1 when Rc is set");
    s.fpscr = 0x01234567U;
    const std::uint32_t mtfsfi = (63U << 26U) | (4U << 23U) |
        (0xAU << 12U) | (134U << 1U) | 1U;
    check(execute(s, mtfsfi) == StepResult::Executed &&
          ((s.fpscr >> 12U) & 0xFU) == 0xAU &&
          s.condition_field(1) == ((s.fpscr >> 28U) & 0xFU),
          "MTFSFI updates a selected FPSCR nibble and records CR1");
    const std::uint32_t mcrfs = (63U << 26U) | (2U << 23U) |
        (3U << 18U) | (64U << 1U);
    check(execute(s, mcrfs) == StepResult::Executed &&
          s.condition_field(2) == 0x3U,
          "MCRFS copies the selected FPSCR field into CR");
    const std::uint32_t mtfsb1_fx = (63U << 26U) | (0U << 21U) | (38U << 1U);
    check(execute(s, mtfsb1_fx) == StepResult::Executed &&
          (s.fpscr & 0x80000000U) != 0U,
          "MTFSB1 sets the selected FPSCR bit");
    const std::uint32_t mtfsb0_fx = (63U << 26U) | (0U << 21U) | (70U << 1U);
    check(execute(s, mtfsb0_fx) == StepResult::Executed &&
          (s.fpscr & 0x80000000U) == 0U,
          "MTFSB0 clears the selected FPSCR bit");
    s.fpr[19] = 0xF000000AULL;
    s.fpscr = 0x01234567U;
    const std::uint32_t mtfsf = (63U << 26U) | (0x81U << 17U) |
        (19U << 11U) | (711U << 1U) | 1U;
    check(execute(s, mtfsf) == StepResult::Executed &&
          s.fpscr == 0xF123456AU && s.condition_field(1) == 0xFU,
          "MTFSF updates only selected FPSCR fields");

    // Single-precision arithmetic opcodes use IEEE-754 single rounding.
    const auto single_fp = [](unsigned frt, unsigned fra, unsigned frb, unsigned xo,
                              unsigned frc = 0, bool rc = false) {
        return (59U << 26U) | (frt << 21U) | (fra << 16U) |
               (frb << 11U) | (frc << 6U) | (xo << 1U) | (rc ? 1U : 0U);
    };
    s.fpr[1] = std::bit_cast<std::uint64_t>(1.5);
    s.fpr[2] = std::bit_cast<std::uint64_t>(2.25);
    check(execute(s, single_fp(3, 1, 2, 21)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[3]) == 3.75, "FADDS computes single-precision sum");
    check(execute(s, single_fp(4, 2, 1, 20)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[4]) == 0.75, "FSUBS computes single-precision difference");
    check(execute(s, single_fp(5, 2, 1, 18)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[5]) == 1.5, "FDIVS computes single-precision quotient");
    check(execute(s, single_fp(6, 1, 0, 25, 2)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[6]) == 3.375, "FMULS reads FRC and rounds single precision");
    s.fpscr = 0x50000000U;
    check(execute(s, single_fp(7, 1, 2, 21, 0, true)) == StepResult::Executed &&
          s.condition_field(1) == 0x5U, "FADDS. copies FPSCR summary to CR1");
    s.fpr[1] = std::bit_cast<std::uint64_t>(2.0);
    s.fpr[2] = std::bit_cast<std::uint64_t>(4.0);
    s.fpr[3] = std::bit_cast<std::uint64_t>(3.0);
    check(execute(s, single_fp(14,1,2,29,3)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[14]) == 10.0, "FMADDS uses fused single-precision multiply-add");
    check(execute(s, single_fp(15,1,2,28,3)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[15]) == 2.0, "FMSUBS uses fused single-precision multiply-subtract");
    check(execute(s, single_fp(16,1,2,31,3)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[16]) == -10.0, "FNMADDS negates fused single-precision multiply-add");
    check(execute(s, single_fp(17,1,2,30,3)) == StepResult::Executed &&
          std::bit_cast<double>(s.fpr[17]) == -2.0, "FNMSUBS negates fused single-precision multiply-subtract");

    // FPU memory transfers and compare/conversion coverage.
    std::array<std::uint8_t, 64> fpu_ram{};
    s.gpr[0] = 0;
    s.fpr[8] = std::bit_cast<std::uint64_t>(3.5);
    const std::uint32_t stfd = (54U << 26U) | (8U << 21U) | 16U;
    check(execute(s, stfd, fpu_ram) == StepResult::Executed, "STFD stores double");
    check(fpu_ram[16] == 0x40 && fpu_ram[17] == 0x0C, "STFD stores big-endian IEEE-754 bytes");
    const std::uint32_t lfd = (50U << 26U) | (9U << 21U) | 16U;
    check(execute(s, lfd, fpu_ram) == StepResult::Executed, "LFD loads double");
    check(std::bit_cast<double>(s.fpr[9]) == 3.5, "LFD restores double value");
    s.fpr[10] = std::bit_cast<std::uint64_t>(1.25);
    const std::uint32_t stfs = (52U << 26U) | (10U << 21U) | 24U;
    check(execute(s, stfs, fpu_ram) == StepResult::Executed, "STFS stores single");
    const std::uint32_t lfs = (48U << 26U) | (11U << 21U) | 24U;
    check(execute(s, lfs, fpu_ram) == StepResult::Executed, "LFS loads single");
    check(std::bit_cast<double>(s.fpr[11]) == 1.25, "LFS converts single to internal double");
    const std::uint32_t fcmpu = (63U << 26U) | (2U << 23U) | (1U << 16U) | (2U << 11U);
    check(execute(s, fcmpu) == StepResult::Executed, "FCMPU executes");
    check(s.condition_field(2) == 0x8, "FCMPU writes less-than condition field");
    const std::uint32_t frsp = (63U << 26U) | (12U << 1U);
    check(execute(s, frsp) == StepResult::Executed, "FRSP executes");

    s.fpr[1] = std::bit_cast<std::uint64_t>(1.0);
    s.fpr[2] = std::bit_cast<std::uint64_t>(0.0);
    const std::uint32_t fdiv_zero = (63U << 26U) | (13U << 21U) |
        (1U << 16U) | (2U << 11U) | (18U << 1U);
    check(execute(s, fdiv_zero) == StepResult::Executed, "FDIV by zero executes with FPSCR status");
    check((s.fpscr & 0x84000000U) == 0x84000000U, "FDIV by zero sets sticky FX and ZX flags");

    // VMX vector maximum XO encodings (VX form).

    s.vr[1].lane = {0x01020304U, 0x7F80FF00U, 0x80000001U, 0xFFFFFFFFU};
    s.vr[2].lane = {0x04030201U, 0x0101FE01U, 0x7F000002U, 0x00000000U};
    check(execute(s, vx_form(3, 1, 2, 514U)) == StepResult::Executed,
          "VMX VMAXUB executes");
    check(s.vr[3].lane[0] == 0x04030304U &&
          s.vr[3].lane[1] == 0x7F80FF01U,
          "VMAXUB compares each byte as unsigned");
    check(execute(s, vx_form(4, 1, 2, 642U)) == StepResult::Executed,
          "VMX VMAXUW executes");
    check(s.vr[4].lane[0] == 0x04030201U &&
          s.vr[4].lane[2] == 0x80000001U,
          "VMAXUW compares unsigned words");
    check(execute(s, vx_form(5, 1, 2, 770U)) == StepResult::Executed,
          "VMX VMAXSB executes");
    check(s.vr[5].lane[0] == 0x04030304U,
          "VMAXSB compares signed byte lanes");
    check(execute(s, vx_form(6, 1, 2, 898U)) == StepResult::Executed,
          "VMX VMAXSW executes");
    check(s.vr[6].lane[2] == 0x7F000002U,
          "VMAXSW compares signed words");

    s.reset(0x1000); // Keep the legacy integer tests independent of FPU PC changes.
    s.gpr[3] = 10;
    check(execute(s, d_form(14, 4, 3, 5)) == StepResult::Executed, "ADDI executes");
    check(s.gpr[4] == 15 && s.pc == 0x1004, "ADDI writes RT and advances PC");

    // D-form ADDI/ADDIS interpret RA=0 as literal zero, even when GPR[0] is nonzero.
    s.gpr[0] = 0x123456789ABCDEF0ULL;
    check(execute(s, d_form(14, 6, 0, 0xFFFC)) == StepResult::Executed,
          "ADDI rD,0,SIMM executes with nonzero GPR0");
    check(s.gpr[6] == 0xFFFFFFFFFFFFFFFCULL,
          "ADDI uses literal zero for RA=0 and sign-extends SIMM");
    check(execute(s, d_form(15, 7, 0, 0x0001)) == StepResult::Executed,
          "ADDIS rD,0,SIMM executes with nonzero GPR0");
    check(s.gpr[7] == 0x10000ULL,
          "ADDIS uses literal zero for RA=0 before shifting SIMM");

    check(execute(s, d_form(14, 5, 0, 0xFFFF)) == StepResult::Executed, "ADDI accepts negative immediate");
    check(s.gpr[5] == UINT64_MAX, "ADDI sign-extends immediate");

    // Immediate logical instructions use zero-extended 16-bit immediates.
    s.gpr[1] = 0x1234000000000000ULL;
    check(execute(s, d_form(24, 1, 2, 0x00F0)) == StepResult::Executed, "ORI executes");
    check(s.gpr[2] == 0x12340000000000F0ULL, "ORI zero-extends immediate and writes RA");
    s.gpr[1] = 0x00000000000000F0ULL;
    check(execute(s, d_form(25, 1, 3, 0x0001)) == StepResult::Executed, "ORIS executes");
    check(s.gpr[3] == 0x00000000000100F0ULL, "ORIS shifts immediate by 16");
    s.gpr[1] = 0xFFFF0000000000FFULL;
    check(execute(s, d_form(26, 1, 4, 0x00F0)) == StepResult::Executed, "XORI executes");
    check(s.gpr[4] == 0xFFFF00000000000FULL, "XORI applies zero-extended immediate");
    s.gpr[1] = 0x00000000000100F0ULL;
    check(execute(s, d_form(27, 1, 5, 0x0001)) == StepResult::Executed, "XORIS executes");
    check(s.gpr[5] == 0x00000000000000F0ULL, "XORIS shifts immediate by 16");
    s.gpr[1] = 0x80000000000000FFULL;
    check(execute(s, d_form(28, 1, 6, 0x00F0)) == StepResult::Executed, "ANDI. executes");
    check(s.gpr[6] == 0xF0 && s.condition_field(0) == 0x4, "ANDI. masks and updates CR0");
    s.gpr[1] = 0x80000000000100F0ULL;
    check(execute(s, d_form(29, 1, 7, 0x0001)) == StepResult::Executed, "ANDIS. executes");
    check(s.gpr[7] == 0x0000000000010000ULL && s.condition_field(0) == 0x4,
          "ANDIS. shifts mask and updates CR0");

    s.gpr[6] = 2;
    check(execute(s, d_form(15, 7, 6, 1)) == StepResult::Executed, "ADDIS executes");
    check(s.gpr[7] == 0x10002, "ADDIS shifts signed immediate by 16");

    s.gpr[8] = 0xF0;
    s.gpr[9] = 0xCC;
    check(execute(s, x_form(8, 10, 9, 444)) == StepResult::Executed, "OR executes");
    check(s.gpr[10] == 0xFC, "OR computes bitwise result");
    check(execute(s, x_form(8, 11, 9, 316)) == StepResult::Executed, "XOR executes");
    check(s.gpr[11] == 0x3C, "XOR computes bitwise result");
    check(execute(s, x_form(8, 12, 9, 28)) == StepResult::Executed, "AND executes");
    check(s.gpr[12] == 0xC0, "AND computes bitwise result");

    // CNTLZW counts leading zeroes in the low 32 bits and supports Rc.
    s.gpr[8] = 0x0000000000000001ULL;
    check(execute(s, x_form(8, 9, 0, 26)) == StepResult::Executed, "CNTLZW executes");
    check(s.gpr[9] == 31, "CNTLZW counts 31 leading zeroes");
    s.gpr[8] = 0x0000000080000000ULL;
    check(execute(s, x_form(8, 10, 0, 26)) == StepResult::Executed, "CNTLZW handles top bit set");
    check(s.gpr[10] == 0, "CNTLZW returns zero when bit 31 is set");
    s.gpr[8] = 0;
    s.xer = (std::uint64_t{1} << 31U);
    check(execute(s, x_form(8, 11, 0, 26, true)) == StepResult::Executed,
          "CNTLZW. executes with record bit set");
    check(s.gpr[11] == 32 && s.condition_field(0) == 0x5,
          "CNTLZW. records positive result and preserves XER[SO]");

    // Word shifts operate on the low 32 bits and zero/sign extend appropriately.
    s.gpr[1] = 0x0000000080000001ULL;
    s.gpr[2] = 1;
    check(execute(s, x_form(1, 3, 2, 24)) == StepResult::Executed, "SLW executes");
    check(s.gpr[3] == 2, "SLW shifts left and zero-extends word result");
    s.gpr[1] = 0x0000000080000001ULL;
    s.gpr[2] = 1;
    check(execute(s, x_form(1, 4, 2, 536)) == StepResult::Executed, "SRW executes");
    check(s.gpr[4] == 0x40000000ULL, "SRW shifts right logically");
    s.gpr[1] = 0x0000000080000001ULL;
    s.gpr[2] = 32;
    check(execute(s, x_form(1, 5, 2, 24)) == StepResult::Executed, "SLW handles shift >= 32");
    check(s.gpr[5] == 0, "SLW returns zero when RB bit 5 is set");
    s.gpr[1] = 0x00000000FFFFFFF3ULL;
    s.gpr[2] = 2;
    check(execute(s, x_form(1, 6, 2, 792)) == StepResult::Executed, "SRAW executes");
    check(s.gpr[6] == 0xFFFFFFFFFFFFFFFCULL, "SRAW sign-extends arithmetic word result");
    check((s.xer & xer_ca) != 0, "SRAW sets CA when negative one bits are shifted out");
    s.gpr[1] = 0x000000007FFFFFF0ULL;
    s.gpr[2] = 2;
    check(execute(s, x_form(1, 7, 2, 792)) == StepResult::Executed, "SRAW handles positive operand");
    check(s.gpr[7] == 0x1FFFFFFCULL && (s.xer & xer_ca) == 0,
          "SRAW positive result does not set CA");

    // SRAWI uses the SH field encoded in the RB position, not GPR[RB].
    s.gpr[1] = 0x00000000FFFFFFF0ULL;
    s.gpr[2] = 0x12345678ULL;
    s.xer &= ~xer_ca;
    const std::uint32_t srawi_shift_zero = (31U << 26U) | (1U << 21U) | (8U << 16U) | (824U << 1U);
    check(execute(s, srawi_shift_zero) == StepResult::Executed,
          "SRAWI executes using immediate shift amount");
    check(s.gpr[8] == 0xFFFFFFFFFFFFFFF0ULL && (s.xer & xer_ca) == 0,
          "SRAWI shift by zero sign-extends and preserves clear CA");
    const std::uint32_t srawi_shift_two = (31U << 26U) | (1U << 21U) |
        (9U << 16U) | (2U << 11U) | (824U << 1U);
    s.gpr[1] = 0x00000000FFFFFFF3ULL;
    check(execute(s, srawi_shift_two) == StepResult::Executed,
          "SRAWI executes with shift amount two");
    check(s.gpr[9] == 0xFFFFFFFFFFFFFFFCULL && (s.xer & xer_ca) != 0,
          "SRAWI sets CA when discarded negative bits contain ones");
    const std::uint32_t srawi_positive_two = (31U << 26U) | (1U << 21U) |
        (10U << 16U) | (2U << 11U) | (824U << 1U);
    s.gpr[1] = 0x000000007FFFFFF3ULL;
    check(execute(s, srawi_positive_two) == StepResult::Executed,
          "SRAWI executes with positive operand");
    check(s.gpr[10] == 0x1FFFFFFCULL && (s.xer & xer_ca) == 0,
          "SRAWI positive operand does not set CA");
    s.gpr[1] = 0x00000000FFFFFFF0ULL;
    check(execute(s, rlwinm_form(1, 8, 0, 0, 31)) == StepResult::Executed,
          "RLWINM helper remains usable after shift tests");

    // Doubleword shifts: counts >= 64 produce zero or sign fill.
    s.gpr[1] = 0x8000000000000001ULL;
    s.gpr[2] = 1;
    check(execute(s, x_form(1, 3, 2, 27)) == StepResult::Executed, "SLD executes");
    check(s.gpr[3] == 2, "SLD shifts left 64-bit value");
    s.gpr[1] = 0x8000000000000001ULL;
    s.gpr[2] = 1;
    check(execute(s, x_form(1, 4, 2, 539)) == StepResult::Executed, "SRD executes");
    check(s.gpr[4] == 0x4000000000000000ULL, "SRD shifts right logically");
    s.gpr[1] = 0x8000000000000001ULL;
    s.gpr[2] = 64;
    check(execute(s, x_form(1, 5, 2, 27)) == StepResult::Executed, "SLD handles shift >= 64");
    check(s.gpr[5] == 0, "SLD returns zero for shift >= 64");
    s.gpr[1] = 0xFFFFFFFFFFFFFFF3ULL;
    s.gpr[2] = 2;
    s.xer &= ~xer_ca;
    check(execute(s, x_form(1, 6, 2, 794)) == StepResult::Executed, "SRAD executes");
    check(s.gpr[6] == 0xFFFFFFFFFFFFFFFCULL, "SRAD sign-fills shifted result");
    check((s.xer & xer_ca) != 0, "SRAD sets CA when discarded negative bits contain ones");
    s.gpr[1] = 0x7FFFFFFFFFFFFFF0ULL;
    s.gpr[2] = 2;
    s.xer |= xer_ca;
    check(execute(s, x_form(1, 7, 2, 794)) == StepResult::Executed, "SRAD handles positive value");
    check(s.gpr[7] == 0x1FFFFFFFFFFFFFFCULL && (s.xer & xer_ca) == 0,
          "SRAD positive result clears CA");
    s.gpr[1] = 0x8000000000000000ULL;
    s.gpr[2] = 64;
    check(execute(s, x_form(1, 8, 2, 794)) == StepResult::Executed, "SRAD handles shift >= 64");
    check(s.gpr[8] == UINT64_MAX && (s.xer & xer_ca) == 0,
          "SRAD saturates sign and only sets CA for discarded one bits");

    s.gpr[13] = 20;
    s.gpr[14] = 22;
    check(execute(s, x_form(15, 13, 14, 266)) == StepResult::Executed, "ADD executes");
    check(s.gpr[15] == 42, "ADD computes sum");
    check(execute(s, x_form(15, 13, 14, 40)) == StepResult::Executed, "SUBF executes");
    check(s.gpr[15] == 2, "SUBF computes RB minus RA");

    s.gpr[17] = 0xFFFFFFFFFFFFFFF0ULL;
    s.gpr[18] = 5;
    check(execute(s, compare_form(2, true, 17, 18, 0)) == StepResult::Executed, "CMP signed 64-bit executes");
    check(s.condition_field(2) == 0x9, "CMP sets selected CR field to LT and preserves SO");
    check(execute(s, compare_form(3, true, 17, 18, 32)) == StepResult::Executed, "CMPL unsigned 64-bit executes");
    check(s.condition_field(3) == 0x5, "CMPL sets selected CR field to GT and preserves SO");

    s.gpr[19] = 0x80000001U;
    check(execute(s, rlwinm_form(19, 20, 1, 0, 31, true)) == StepResult::Executed, "RLWINM executes");
    check(s.gpr[20] == 3, "RLWINM rotates 32-bit word and applies full mask");
    check(s.condition_field(0) == 0x5, "RLWINM record form evaluates 32-bit result and preserves SO");

    s.xer = (std::uint64_t{1} << 31U);
    s.gpr[8] = 1; s.gpr[9] = 2;
    check(execute(s, x_form(8, 16, 9, 444, true)) == StepResult::Executed, "OR. record form executes");
    check(s.condition_field(0) == 0x5, "record form updates CR0 GT and SO");

    // SUBFIC computes SIMM - RA and sets CA when no borrow occurs.
    s.gpr[2] = 3;
    check(execute(s, d_form(8, 9, 2, 5)) == StepResult::Executed, "SUBFIC executes");
    check(s.gpr[9] == 2 && (s.xer & xer_ca) != 0, "SUBFIC sets carry when no borrow");
    s.gpr[2] = 8;
    check(execute(s, d_form(8, 9, 2, 5)) == StepResult::Executed, "SUBFIC executes with borrow");
    check(s.gpr[9] == UINT64_MAX - 2U && (s.xer & xer_ca) == 0,
          "SUBFIC clears carry when borrow occurs");
    s.gpr[2] = 7;
    check(execute(s, x_form(10, 2, 0, 104)) == StepResult::Executed, "NEG executes");
    check(s.gpr[10] == UINT64_MAX - 6U, "NEG computes two's-complement negation");

    // ADDIC updates XER[CA] on unsigned carry-out; ADDIC. also records CR0.
    s.gpr[26] = UINT64_MAX;
    check(execute(s, d_form(12, 27, 26, 1)) == StepResult::Executed, "ADDIC executes");
    check(s.gpr[27] == 0 && (s.xer & (std::uint64_t{1} << 29U)) != 0,
          "ADDIC wraps and sets XER carry");
    s.gpr[26] = 7;
    check(execute(s, d_form(13, 27, 26, 1)) == StepResult::Executed, "ADDIC. executes");
    check(s.gpr[27] == 8 && s.condition_field(0) == 0x5, "ADDIC. updates CR0 and preserves XER[SO]");

    // ADDE and SUBFE consume and update XER[CA].
    s.gpr[3] = UINT64_MAX;
    s.gpr[4] = 0;
    s.xer |= xer_ca;
    check(execute(s, x_form(5, 3, 4, 138)) == StepResult::Executed, "ADDE executes");
    check(s.gpr[5] == 0 && (s.xer & xer_ca) != 0, "ADDE consumes carry-in and sets carry-out");
    s.gpr[3] = 5;
    s.gpr[4] = 3;
    s.xer |= xer_ca;
    check(execute(s, x_form(6, 3, 4, 136)) == StepResult::Executed, "SUBFE executes");
    check(s.gpr[6] == UINT64_MAX - 1U && (s.xer & xer_ca) == 0,
          "SUBFE computes complemented minuend and clears carry when no unsigned carry-out");
    s.gpr[3] = 5;
    s.gpr[4] = 3;
    s.xer &= ~xer_ca;
    check(execute(s, x_form(6, 3, 4, 136)) == StepResult::Executed, "SUBFE executes without carry-in");
    check(s.gpr[6] == UINT64_MAX - 2U && (s.xer & xer_ca) == 0,
          "SUBFE clears carry-out when borrow occurs");

    // ADDZE and SUBFZE use XER[CA] as their carry-in.
    s.gpr[3] = UINT64_MAX;
    s.xer |= xer_ca;
    check(execute(s, x_form(7, 3, 0, 202)) == StepResult::Executed, "ADDZE executes");
    check(s.gpr[7] == 0 && (s.xer & xer_ca) != 0, "ADDZE wraps and retains carry-out");
    s.gpr[3] = 5;
    s.xer |= xer_ca;
    check(execute(s, x_form(8, 3, 0, 200)) == StepResult::Executed, "SUBFZE executes");
    check(s.gpr[8] == UINT64_MAX - 4U && (s.xer & xer_ca) == 0,
          "SUBFZE computes complement plus carry-in");
    s.gpr[3] = 5;
    s.xer &= ~xer_ca;
    check(execute(s, x_form(8, 3, 0, 200)) == StepResult::Executed, "SUBFZE executes without carry-in");
    check(s.gpr[8] == UINT64_MAX - 5U && (s.xer & xer_ca) == 0,
          "SUBFZE computes complement without carry-in");

    // ADDME and SUBFME compute operand + carry-in - 1.
    s.gpr[3] = 0;
    s.xer |= xer_ca;
    check(execute(s, x_form(9, 3, 0, 234)) == StepResult::Executed, "ADDME executes");
    check(s.gpr[9] == 0 && (s.xer & xer_ca) != 0,
          "ADDME with carry-in one returns zero and sets carry-out");
    s.gpr[3] = 0;
    s.xer &= ~xer_ca;
    check(execute(s, x_form(9, 3, 0, 234)) == StepResult::Executed, "ADDME executes without carry-in");
    check(s.gpr[9] == UINT64_MAX && (s.xer & xer_ca) == 0,
          "ADDME without carry-in wraps to all ones");
    s.gpr[3] = 5;
    s.xer |= xer_ca;
    check(execute(s, x_form(10, 3, 0, 232)) == StepResult::Executed, "SUBFME executes");
    check(s.gpr[10] == UINT64_MAX - 5U && (s.xer & xer_ca) != 0,
          "SUBFME computes complemented operand plus carry-in minus one");

    // Doubleword arithmetic shift must sign-fill and update XER[CA].
    s.gpr[1] = 0x8000000000000003ULL;
    s.gpr[2] = 1;
    s.xer &= ~xer_ca;
    check(execute(s, x_form(1, 11, 2, 794)) == StepResult::Executed,
          "SRAD executes for a negative operand");
    check(s.gpr[11] == 0xC000000000000001ULL && (s.xer & xer_ca) != 0,
          "SRAD sign-fills and sets CA for discarded one bits");
    s.gpr[1] = 0x8000000000000000ULL;
    s.gpr[2] = 1;
    s.xer |= xer_ca;
    check(execute(s, x_form(1, 12, 2, 794)) == StepResult::Executed,
          "SRAD executes when discarded bits are zero");
    check(s.gpr[12] == 0xC000000000000000ULL && (s.xer & xer_ca) == 0,
          "SRAD clears CA when no discarded one bits are shifted out");
    s.gpr[1] = 0x7FFFFFFFFFFFFFFFULL;
    s.gpr[2] = 64;
    check(execute(s, x_form(1, 13, 2, 794)) == StepResult::Executed,
          "SRAD handles shift amount 64");
    check(s.gpr[13] == 0 && (s.xer & xer_ca) == 0,
          "SRAD shift by 64 produces zero for positive operands");
    s.gpr[1] = 0x8000000000000001ULL;
    s.gpr[2] = 64;
    s.xer &= ~xer_ca;
    check(execute(s, x_form(1, 14, 2, 794)) == StepResult::Executed,
          "SRAD handles a negative operand shifted by 64");
    check(s.gpr[14] == UINT64_MAX && (s.xer & xer_ca) != 0,
          "SRAD shift by 64 sign-fills and sets CA for discarded one bits");
    s.gpr[1] = 0x8000000000000000ULL;
    s.xer |= xer_ca;
    check(execute(s, x_form(1, 15, 2, 794)) == StepResult::Executed,
          "SRAD handles negative shift by 64 with no discarded one bits");
    check(s.gpr[15] == UINT64_MAX && (s.xer & xer_ca) == 0,
          "SRAD shift by 64 clears CA when lower bits are all zero");
    s.gpr[1] = 0x8000000000000003ULL;
    s.gpr[2] = 1;
    s.xer &= ~(std::uint64_t{1} << 31U);
    check(execute(s, x_form(1, 16, 2, 794, true)) == StepResult::Executed,
          "SRAD. executes with record bit set");
    check(s.gpr[16] == 0xC000000000000001ULL && s.condition_field(0) == 0x8,
          "SRAD. records the 64-bit negative result in CR0");

    // SRAW variable shifts at and beyond the 32-bit word boundary.
    s.gpr[1] = 0x0000000080000001ULL;
    s.gpr[2] = 32;
    s.xer &= ~xer_ca;
    check(execute(s, x_form(1, 17, 2, 792)) == StepResult::Executed,
          "SRAW executes with shift amount 32");
    check(s.gpr[17] == UINT64_MAX && (s.xer & xer_ca) != 0,
          "SRAW shift by 32 sign-extends and sets CA for discarded ones");
    s.gpr[1] = 0x0000000080000000ULL;
    s.gpr[2] = 63;
    s.xer |= xer_ca;
    check(execute(s, x_form(1, 18, 2, 792)) == StepResult::Executed,
          "SRAW executes with shift amount 63");
    check(s.gpr[18] == UINT64_MAX && (s.xer & xer_ca) == 0,
          "SRAW shift by 63 clears CA when discarded low bits are zero");
    s.gpr[1] = 0x000000007FFFFFFFULL;
    s.gpr[2] = 32;
    check(execute(s, x_form(1, 19, 2, 792)) == StepResult::Executed,
          "SRAW executes for a positive operand at shift 32");
    check(s.gpr[19] == 0 && (s.xer & xer_ca) == 0,
          "SRAW shift by 32 produces zero and clears CA for positive operands");

    // SLW: word shifts at 31 and with RB[5] set (32/63) zero the result.
    s.gpr[1] = 0x0000000080000001ULL;
    s.gpr[2] = 31;
    check(execute(s, x_form(1, 20, 2, 24)) == StepResult::Executed,
          "SLW executes at shift amount 31");
    check(s.gpr[20] == 0x80000000ULL,
          "SLW shift by 31 keeps the low-word result and zero-extends");
    s.gpr[2] = 32;
    check(execute(s, x_form(1, 21, 2, 24)) == StepResult::Executed,
          "SLW executes when RB bit 5 is set");
    check(s.gpr[21] == 0, "SLW shift by 32 produces zero");
    s.gpr[2] = 63;
    check(execute(s, x_form(1, 22, 2, 24)) == StepResult::Executed,
          "SLW executes at shift amount 63");
    check(s.gpr[22] == 0, "SLW shift by 63 produces zero");

    // SRW: word shifts at 31 and with RB[5] set (32/63) zero the result.
    s.gpr[1] = 0x0000000080000001ULL;
    s.gpr[2] = 31;
    check(execute(s, x_form(1, 29, 2, 536)) == StepResult::Executed,
          "SRW executes at shift amount 31");
    check(s.gpr[29] == 1, "SRW shift by 31 is logical and zero-extends");
    s.gpr[2] = 32;
    check(execute(s, x_form(1, 30, 2, 536)) == StepResult::Executed,
          "SRW executes when RB bit 5 is set");
    check(s.gpr[30] == 0, "SRW shift by 32 produces zero");
    s.gpr[2] = 63;
    check(execute(s, x_form(1, 31, 2, 536)) == StepResult::Executed,
          "SRW executes at shift amount 63");
    check(s.gpr[31] == 0, "SRW shift by 63 produces zero");

    // Doubleword logical shifts: 63 is valid; 64 and 127 produce zero.
    s.gpr[1] = 0x8000000000000001ULL;
    s.gpr[2] = 63;
    check(execute(s, x_form(1, 23, 2, 27)) == StepResult::Executed,
          "SLD executes at shift amount 63");
    check(s.gpr[23] == 0x8000000000000000ULL,
          "SLD shift by 63 moves the low bit to the sign position");
    s.gpr[2] = 64;
    check(execute(s, x_form(1, 24, 2, 27)) == StepResult::Executed,
          "SLD executes at shift amount 64");
    check(s.gpr[24] == 0, "SLD shift by 64 produces zero");
    s.gpr[2] = 127;
    check(execute(s, x_form(1, 25, 2, 27)) == StepResult::Executed,
          "SLD executes at shift amount 127");
    check(s.gpr[25] == 0, "SLD shift by 127 produces zero");

    s.gpr[1] = 0x8000000000000001ULL;
    s.gpr[2] = 63;
    check(execute(s, x_form(1, 26, 2, 539)) == StepResult::Executed,
          "SRD executes at shift amount 63");
    check(s.gpr[26] == 1, "SRD shift by 63 keeps the original top bit");
    s.gpr[2] = 64;
    check(execute(s, x_form(1, 27, 2, 539)) == StepResult::Executed,
          "SRD executes at shift amount 64");
    check(s.gpr[27] == 0, "SRD shift by 64 produces zero");
    s.gpr[2] = 127;
    check(execute(s, x_form(1, 28, 2, 539)) == StepResult::Executed,
          "SRD executes at shift amount 127");
    check(s.gpr[28] == 0, "SRD shift by 127 produces zero");

    // SRAWI record form: verify CR0 uses the sign of the 32-bit result.
    s.gpr[1] = 0x0000000080000001ULL;
    s.xer &= ~(std::uint64_t{1} << 31U);
    const std::uint32_t srawi_record_zero = (31U << 26U) | (1U << 21U) | (9U << 16U) | (824U << 1U) | 1U;
    check(execute(s, srawi_record_zero) == StepResult::Executed,
          "SRAWI. executes with record bit set");
    check(s.gpr[9] == 0xFFFFFFFF80000001ULL && s.condition_field(0) == 0x8,
          "SRAWI. records a negative sign-extended word in CR0");
    s.gpr[1] = 0x0000000000000004ULL;
    const std::uint32_t srawi_record_zero_positive = (31U << 26U) | (1U << 21U) | (10U << 16U) | (824U << 1U) | 1U;
    check(execute(s, srawi_record_zero_positive) == StepResult::Executed,
          "SRAWI. executes for a positive word");
    check(s.gpr[10] == 4 && s.condition_field(0) == 0x4,
          "SRAWI. records a positive nonzero result in CR0");

    std::array<std::uint8_t, 64> ram{};
    s.gpr[21] = 0x12345678U;
    check(execute(s, d_form(36, 21, 0, 8), ram) == StepResult::Executed, "STW executes");
    check(ram[8] == 0x12 && ram[9] == 0x34 && ram[10] == 0x56 && ram[11] == 0x78,
          "STW stores big-endian word");
    check(execute(s, d_form(32, 22, 0, 9), ram) == StepResult::AlignmentFault,
          "LWZ rejects a misaligned effective address");
    check(execute(s, d_form(40, 16, 0, 25), ram) == StepResult::AlignmentFault,
          "LHZ rejects a misaligned effective address");
    s.gpr[22] = 0;
    check(execute(s, d_form(32, 22, 0, 8), ram) == StepResult::Executed, "LWZ executes");
    check(s.gpr[22] == 0x12345678U, "LWZ loads big-endian word");
    ram[12] = 0xAB;
    check(execute(s, d_form(34, 23, 0, 12), ram) == StepResult::Executed, "LBZ executes");
    check(s.gpr[23] == 0xAB, "LBZ zero extends byte");
    s.gpr[24] = 0xCD;
    check(execute(s, d_form(38, 24, 0, 13), ram) == StepResult::Executed, "STB executes");
    check(ram[13] == 0xCD, "STB stores low byte");

    // Update forms write the effective address back to RA only after success.
    s.gpr[28] = 4;
    check(execute(s, d_form(33, 29, 28, 8), ram) == StepResult::Executed, "LWZU executes");
    if (s.gpr[29] != 0xABCD0000U || s.gpr[28] != 12)
        std::cerr << "LWZU actual RT=" << std::hex << s.gpr[29]
                  << " RA=" << s.gpr[28] << std::dec << '\n';
    check(s.gpr[29] == 0xABCD0000U && s.gpr[28] == 12, "LWZU loads and updates RA");
    s.gpr[28] = 16;
    s.gpr[30] = 0xA1B2C3D4U;
    check(execute(s, d_form(37, 30, 28, 4), ram) == StepResult::Executed, "STWU executes");
    check(s.gpr[28] == 20 && ram[20] == 0xA1 && ram[23] == 0xD4,
          "STWU stores and updates RA");
    s.gpr[28] = 63;
    const auto ra_before_fault = s.gpr[28];
    check(execute(s, d_form(33, 29, 28, 1), ram) == StepResult::MemoryFault,
          "LWZU reports out-of-range access");
    check(s.gpr[28] == ra_before_fault, "LWZU fault leaves RA unchanged");
    const auto before_fault_pc = s.pc;
    check(execute(s, d_form(32, 25, 0, 64), ram) == StepResult::MemoryFault,
          "out-of-range LWZ reports memory fault");
    check(s.pc == before_fault_pc, "memory fault does not advance PC");

    const auto branch_pc = s.pc;
    check(execute(s, (18U << 26U) | 0x10U | 1U) == StepResult::Executed, "B with link executes");
    check(s.pc == branch_pc + 0x10U && s.lr == branch_pc + 4U, "B updates PC and LR");
    const auto bc_pc = s.pc;
    s.set_condition_field(0, 0x2);
    check(execute(s, branch_form(16, 12, 2, 0x20U)) == StepResult::Executed, "BC conditional branch executes");
    check(s.pc == bc_pc + 0x20U, "BC takes branch when CR bit matches BO");
    const auto not_taken_pc = s.pc;
    check(execute(s, branch_form(16, 4, 2, 0x20U)) == StepResult::Executed, "BC not-taken path executes");
    check(s.pc == not_taken_pc + 4U, "BC advances normally when condition fails");

    // Halfword accesses are big-endian; LHA sign-extends while LHZ zero-extends.
    ram[24] = 0x80;
    ram[25] = 0x01;
    check(execute(s, d_form(40, 16, 0, 24), ram) == StepResult::Executed, "LHZ executes");
    check(s.gpr[16] == 0x8001, "LHZ zero-extends big-endian halfword");
    check(execute(s, d_form(42, 17, 0, 24), ram) == StepResult::Executed, "LHA executes");
    check(s.gpr[17] == 0xFFFFFFFFFFFF8001ULL, "LHA sign-extends big-endian halfword");
    s.gpr[18] = 0x1234;
    check(execute(s, d_form(44, 18, 0, 26), ram) == StepResult::Executed, "STH executes");
    check(ram[26] == 0x12 && ram[27] == 0x34, "STH stores big-endian halfword");
    s.gpr[28] = 28;
    check(execute(s, d_form(41, 19, 28, 0), ram) == StepResult::Executed, "LHZU executes");
    check(s.gpr[19] == 0 && s.gpr[28] == 28, "LHZU loads and updates RA");
    s.gpr[28] = 30;
    s.gpr[20] = 0xBEEF;
    check(execute(s, d_form(45, 20, 28, 0), ram) == StepResult::Executed, "STHU executes");
    check(ram[30] == 0xBE && ram[31] == 0xEF && s.gpr[28] == 30,
          "STHU stores and updates RA");
    s.gpr[28] = 64;
    const auto halfword_ra_before_fault = s.gpr[28];
    check(execute(s, d_form(41, 19, 28, 0), ram) == StepResult::MemoryFault,
          "LHZU reports out-of-range access");
    check(s.gpr[28] == halfword_ra_before_fault, "LHZU fault leaves RA unchanged");

    const auto old_pc = s.pc;
    check(execute(s, 0x100007FFU) == StepResult::UnsupportedInstruction, "unknown vector instruction is rejected");
    check(s.pc == old_pc, "unsupported instruction does not advance PC");

    // Additional integer ISA coverage used by Xenon game code.
    s.gpr[1] = 0xF0F0F0F0F0F0F0F0ULL;
    s.gpr[2] = 0x0FF00FF00FF00FF0ULL;
    check(execute(s, x_form(1, 3, 2, 60)) == StepResult::Executed, "ANDC executes");
    check(s.gpr[3] == 0xF000F000F000F000ULL, "ANDC clears bits selected by RB");
    check(execute(s, x_form(1, 4, 2, 124)) == StepResult::Executed, "NOR executes");
    check(s.gpr[4] == 0x000F000F000F000FULL, "NOR complements bitwise OR");
    s.gpr[1] = 0x80;
    check(execute(s, x_form(1, 5, 0, 954)) == StepResult::Executed, "EXTSB executes");
    check(s.gpr[5] == 0xFFFFFFFFFFFFFF80ULL, "EXTSB sign extends low byte");
    s.gpr[1] = 0x8001;
    check(execute(s, x_form(1, 6, 0, 922)) == StepResult::Executed, "EXTSH executes");
    check(s.gpr[6] == 0xFFFFFFFFFFFF8001ULL, "EXTSH sign extends low halfword");
    s.gpr[1] = 0x0000000100000000ULL;
    check(execute(s, x_form(1, 7, 0, 58)) == StepResult::Executed, "CNTLZD executes");
    check(s.gpr[7] == 31, "CNTLZD counts leading zeroes across 64 bits");
    s.gpr[1] = 0;
    check(execute(s, x_form(1, 8, 0, 58)) == StepResult::Executed, "CNTLZD handles zero");
    check(s.gpr[8] == 64, "CNTLZD returns 64 for zero");

    // RFI restores the saved execution context and resumes at SRR0.
    s.pc = 0x700;
    s.srr0 = 0x12345678;
    s.srr1 = 0x0000000000009030ULL;
    s.msr = 0;
    s.exception_pending = true;
    s.exception_vector = 0x700;
    const std::uint32_t rfi = (19U << 26U) | (50U << 1U);
    check(execute(s, rfi) == StepResult::Executed, "RFI executes");
    check(s.pc == 0x12345678 && s.msr == 0x9030,
          "RFI restores SRR0 and SRR1 without incrementing restored PC");
    check(!s.exception_pending && s.exception_vector == 0,
          "RFI clears pending exception state");

    // SPR and condition-register moves used by compiled PowerPC code.
    const auto encode_spr = [](unsigned rt, unsigned spr, unsigned xo) {
        return (31U << 26U) | (rt << 21U) | ((spr & 0x1FU) << 16U) |
               ((spr >> 5U) << 11U) | (xo << 1U);
    };
    s.lr = 0x1122334455667788ULL;
    check(execute(s, encode_spr(4, 8, 339)) == StepResult::Executed, "MFSPR reads LR");
    check(s.gpr[4] == s.lr, "MFSPR copies LR to RT");
    s.ctr = 0x8877665544332211ULL;
    check(execute(s, encode_spr(5, 9, 339)) == StepResult::Executed, "MFSPR reads CTR");
    check(s.gpr[5] == s.ctr, "MFSPR copies CTR to RT");
    s.gpr[6] = 0xAABBCCDDEEFF0011ULL;
    check(execute(s, encode_spr(6, 8, 467)) == StepResult::Executed, "MTSPR writes LR");
    check(s.lr == s.gpr[6], "MTSPR copies RS to LR");
    s.gpr[7] = 0x123456789ABCDEF0ULL;
    check(execute(s, encode_spr(7, 9, 467)) == StepResult::Executed, "MTSPR writes CTR");
    check(s.ctr == s.gpr[7], "MTSPR copies RS to CTR");
    s.cr = 0x12345678U;
    check(execute(s, (31U << 26U) | (8U << 21U) | (19U << 1U)) == StepResult::Executed,
          "MFCR executes");
    check(s.gpr[8] == 0x12345678U, "MFCR copies complete CR to RT");
    s.gpr[9] = 0xA5B6C7D8U;
    const std::uint32_t mtcrf_cr0_cr2 = (31U << 26U) | (9U << 21U) |
        (0xA0U << 12U) | (144U << 1U);
    check(execute(s, mtcrf_cr0_cr2) == StepResult::Executed, "MTCRF executes");
    if (!(s.condition_field(0) == 0xA && s.condition_field(2) == 0xB &&
          s.condition_field(1) == 0x2)) {
        std::cerr << "MTCRF actual CR=" << std::hex << s.cr
                  << " fields=" << static_cast<unsigned>(s.condition_field(0)) << ","
                  << static_cast<unsigned>(s.condition_field(1)) << ","
                  << static_cast<unsigned>(s.condition_field(2)) << std::dec << '\\n';
        ++failures;
    }
    const auto unsupported_spr_pc = s.pc;
    check(execute(s, encode_spr(10, 268, 339)) == StepResult::UnsupportedInstruction,
          "MFSPR rejects unsupported SPR without fabricating a value");
    check(s.pc == unsupported_spr_pc, "unsupported MFSPR leaves PC unchanged");

    std::array<std::uint8_t, 16> indexed_ram{
        0x12,0x34,0x56,0x78, 0,0,0,0, 0,0,0,0, 0,0,0,0
    };
    const auto indexed = [](unsigned rt, unsigned ra, unsigned rb, unsigned xo, bool rc = false) {
        return (31U << 26U) | (rt << 21U) | (ra << 16U) |
               (rb << 11U) | (xo << 1U) | (rc ? 1U : 0U);
    };

    // Indexed byte/halfword loads and stores, including update variants.
    indexed_ram[4] = 0x80; indexed_ram[5] = 0x01;
    s.gpr[3] = 0; s.gpr[5] = 4;
    check(execute(s, indexed(9, 3, 5, 87), indexed_ram) == StepResult::Executed &&
          s.gpr[9] == 0x80U, "LBZX zero-extends indexed byte");
    check(execute(s, indexed(10, 3, 5, 279), indexed_ram) == StepResult::Executed &&
          s.gpr[10] == 0x8001U, "LHZX zero-extends indexed halfword");
    check(execute(s, indexed(11, 3, 5, 343), indexed_ram) == StepResult::Executed &&
          s.gpr[11] == 0xFFFFFFFFFFFF8001ULL, "LHAX sign-extends indexed halfword");
    s.gpr[12] = 0x5AU;
    check(execute(s, indexed(12, 3, 5, 215), indexed_ram) == StepResult::Executed &&
          indexed_ram[4] == 0x5A, "STBX stores indexed byte");
    s.gpr[12] = 0xBEEFU;
    check(execute(s, indexed(12, 3, 5, 407), indexed_ram) == StepResult::Executed &&
          indexed_ram[4] == 0xBE && indexed_ram[5] == 0xEF,
          "STHX stores indexed big-endian halfword");
    s.gpr[3] = 0; s.gpr[5] = 4;
    check(execute(s, indexed(13, 3, 5, 311), indexed_ram) == StepResult::Executed &&
          s.gpr[13] == 0xBEEFU && s.gpr[3] == 4U,
          "LHZUX loads halfword and updates RA");
    s.gpr[3] = 0; s.gpr[5] = 4;
    check(execute(s, indexed(14, 3, 5, 375), indexed_ram) == StepResult::Executed &&
          s.gpr[14] == 0xFFFFFFFFFFFFBEEFULL && s.gpr[3] == 4U,
          "LHAUX sign-extends and updates RA");
    s.gpr[3] = 0; s.gpr[5] = 4; s.gpr[12] = 0x1234U;
    check(execute(s, indexed(12, 3, 5, 439), indexed_ram) == StepResult::Executed &&
          indexed_ram[4] == 0x12 && indexed_ram[5] == 0x34 && s.gpr[3] == 4U,
          "STHUX stores and updates RA");
    s.gpr[3] = 0; s.gpr[5] = 4; s.gpr[12] = 0x66U;
    check(execute(s, indexed(12, 3, 5, 247), indexed_ram) == StepResult::Executed &&
          indexed_ram[4] == 0x66 && s.gpr[3] == 4U,
          "STBUX stores byte and updates RA");
    indexed_ram[4] = 0x12; indexed_ram[5] = 0x34;
    s.gpr[3] = 0; s.gpr[5] = 4;
    check(execute(s, indexed(15, 3, 5, 790), indexed_ram) == StepResult::Executed &&
          s.gpr[15] == 0x3412U, "LHBRX reverses indexed halfword byte order");
    s.gpr[3] = 0; s.gpr[5] = 4; s.gpr[16] = 0xABCDU;
    check(execute(s, indexed(16, 3, 5, 918), indexed_ram) == StepResult::Executed &&
          indexed_ram[4] == 0xCD && indexed_ram[5] == 0xAB,
          "STHBRX stores indexed halfword in reversed byte order");

    // VX-form VMX arithmetic: opcode extension includes its low bit.
    s.vr[1].lane = {0xFF01FE02U, 0xFFFFFFFFU, 0xFFFF0001U, 0x3F800000U};
    s.vr[2].lane = {0x01020304U, 0x00000002U, 0x00020003U, 0x40000000U};
    check(execute(s, vx_form(3, 1, 2, 0)) == StepResult::Executed &&
          s.vr[3].lane[0] == 0x00030106U, "VMX VADDUBM wraps each byte independently");
    check(execute(s, vx_form(3, 1, 2, 64)) == StepResult::Executed &&
          s.vr[3].lane[2] == 0x00010004U, "VMX VADDUHM wraps each halfword independently");
    check(execute(s, vx_form(3, 1, 2, 128)) == StepResult::Executed &&
          s.vr[3].lane[1] == 1U, "VMX VADDUWM wraps 32-bit lanes");
    check(execute(s, vx_form(3, 1, 2, 1024)) == StepResult::Executed &&
          s.vr[3].lane[0] == 0xFEFFFBFEU, "VMX VSUBUBM uses the 11-bit opcode field");
    check(execute(s, vx_form(3, 1, 2, 10)) == StepResult::Executed &&
          std::bit_cast<float>(s.vr[3].lane[3]) == 3.0F, "VMX VADDFP adds four float lanes");
    check(execute(s, vx_form(3, 1, 2, 1028)) == StepResult::Executed &&
          s.vr[3].lane[0] == (0xFF01FE02U & 0x01020304U), "VMX VAND executes");
    check(execute(s, vx_form(3, 1, 2, 1220)) == StepResult::Executed &&
          s.vr[3].lane[0] == (0xFF01FE02U ^ 0x01020304U), "VMX VXOR executes");
    check(execute(s, vx_form(3, 1, 2, 1156)) == StepResult::Executed &&
          s.vr[3].lane[0] == (0xFF01FE02U | 0x01020304U), "VMX VOR executes");
    s.vr[1].lane = {0xAAAAAAAAU, 0x00000000U, 0xFFFFFFFFU, 0x12345678U};
    s.vr[2].lane = {0x55555555U, 0xFFFFFFFFU, 0x00000000U, 0x87654321U};
    s.vr[4].lane = {0xF0F0F0F0U, 0xAAAAAAAAU, 0x55555555U, 0xFFFF0000U};
    const std::uint32_t vsel = (4U << 26U) | (3U << 21U) |
        (1U << 16U) | (2U << 11U) | (4U << 6U) | 42U;
    check(execute(s, vsel) == StepResult::Executed,
          "VMX VSEL executes with its third vector operand");
    check(s.vr[3].lane == std::array<std::uint32_t,4>{
              0x5A5A5A5AU, 0xAAAAAAAAU, 0xAAAAAAAAU, 0x87655678U},
          "VMX VSEL selects each bit from VA or VB using VC");
    s.vr[1].lane = {0x00010203U, 0x04050607U, 0x08090A0BU, 0x0C0D0E0FU};
    s.vr[2].lane = {0x10111213U, 0x14151617U, 0x18191A1BU, 0x1C1D1E1FU};
    s.vr[4].lane = {0x000F101FU, 0x010E111EU, 0x020D121DU, 0x030C131CU};
    const std::uint32_t vperm = (4U << 26U) | (3U << 21U) |
        (1U << 16U) | (2U << 11U) | (4U << 6U) | 43U;
    check(execute(s, vperm) == StepResult::Executed,
          "VMX VPERM executes with VA, VB and VC");
    check(s.vr[3].lane == std::array<std::uint32_t,4>{
              0x000F101FU, 0x010E111EU, 0x020D121DU, 0x030C131CU},
          "VMX VPERM selects bytes across concatenated VA and VB");
    s.vr[1].lane = {0xFF0180FEU, 0x80000001U, 0xFFFFFFFFU, 0x7FFFFFFFU};
    s.vr[2].lane = {0x0102FF7FU, 0x7FFFFFFFU, 0x00000000U, 0x80000000U};
    check(execute(s, vx_form(3, 1, 2, 2)) == StepResult::Executed &&
          s.vr[3].lane[0] == 0x0101807FU, "VMX VMINUB compares each byte as unsigned");
    check(execute(s, vx_form(3, 1, 2, 258)) == StepResult::Executed &&
          s.vr[3].lane[0] == 0xFF0180FEU, "VMX VMINSB compares each byte as signed");
    check(execute(s, vx_form(3, 1, 2, 130)) == StepResult::Executed &&
          s.vr[3].lane[1] == 0x7FFFFFFFU &&
          s.vr[3].lane[2] == 0U, "VMX VMINUW compares 32-bit lanes as unsigned");
    s.vr[1].lane = {0x01020304U, 0x00020004U, 0x11223344U, 0xAABBCCDDU};
    s.vr[2].lane = {0x01090304U, 0x00020005U, 0x11223344U, 0xAABBCC00U};
    check(execute(s, vx_form(3, 1, 2, 6U)) == StepResult::Executed &&
          s.vr[3].lane[0] == 0xFF00FFFFU,
          "VMX VCMPEQUB returns byte masks for equal elements");
    check(execute(s, vx_form(4, 1, 2, 70U)) == StepResult::Executed &&
          s.vr[4].lane[1] == 0xFFFF0000U,
          "VMX VCMPEQUH returns halfword masks for equal elements");
    check(execute(s, vx_form(5, 1, 2, 134U)) == StepResult::Executed &&
          s.vr[5].lane[2] == 0xFFFFFFFFU &&
          s.vr[5].lane[3] == 0U,
          "VMX VCMPEQUW returns word masks for equal elements");
    s.vr[1].lane = {0xFF000201U, 0x0002FF04U, 0x80000002U, 0x7FFFFFFFU};
    s.vr[2].lane = {0x01010001U, 0x00010005U, 0x7FFFFFFFU, 0x80000000U};
    check(execute(s, vx_form(6, 1, 2, 518U)) == StepResult::Executed &&
          s.vr[6].lane[0] == 0xFF00FF00U,
          "VMX VCMPGTUB compares unsigned byte lanes");
    check(execute(s, vx_form(7, 1, 2, 774U)) == StepResult::Executed &&
          s.vr[7].lane[0] == 0x0000FF00U,
          "VMX VCMPGTSB compares signed byte lanes");
    check(execute(s, vx_form(8, 1, 2, 838U)) == StepResult::Executed &&
          s.vr[8].lane[1] == 0xFFFF0000U,
          "VMX VCMPGTSH compares signed halfword lanes");
    check(execute(s, vx_form(9, 1, 2, 646U)) == StepResult::Executed &&
          s.vr[9].lane[2] == 0xFFFFFFFFU,
          "VMX VCMPGTUW compares unsigned word lanes");
    check(execute(s, vx_form(10, 1, 2, 902U)) == StepResult::Executed &&
          s.vr[10].lane[2] == 0U && s.vr[10].lane[3] == 0xFFFFFFFFU,
          "VMX VCMPGTSW compares signed word lanes");
    s.vr[1].lane = {1U, 2U, 3U, 4U};
    s.vr[2].lane = {1U, 2U, 3U, 4U};
    check(execute(s, vx_form(20, 1, 2, 135U)) == StepResult::Executed &&
          s.condition_field(6) == 0x8U,
          "VMX compare record sets CR6 when every element matches");
    s.vr[2].lane = {5U, 6U, 7U, 8U};
    check(execute(s, vx_form(20, 1, 2, 135U)) == StepResult::Executed &&
          s.condition_field(6) == 0x2U,
          "VMX compare record sets CR6 when no element matches");
    s.vr[2].lane = {1U, 6U, 3U, 8U};
    check(execute(s, vx_form(20, 1, 2, 135U)) == StepResult::Executed &&
          s.condition_field(6) == 0x0U,
          "VMX compare record clears CR6 result bits for mixed element results");
    s.vr[1].lane = {std::bit_cast<std::uint32_t>(1.0F),
                    std::bit_cast<std::uint32_t>(2.0F),
                    std::bit_cast<std::uint32_t>(-1.0F),
                    std::bit_cast<std::uint32_t>(0.0F)};
    s.vr[2].lane = {std::bit_cast<std::uint32_t>(1.0F),
                    std::bit_cast<std::uint32_t>(1.0F),
                    std::bit_cast<std::uint32_t>(-2.0F),
                    std::bit_cast<std::uint32_t>(0.0F)};
    check(execute(s, vx_form(11, 1, 2, 198U)) == StepResult::Executed &&
          s.vr[11].lane == std::array<std::uint32_t,4>{
              0xFFFFFFFFU, 0U, 0U, 0xFFFFFFFFU},
          "VMX VCMPEQFP compares floating-point lanes");
    check(execute(s, vx_form(12, 1, 2, 454U)) == StepResult::Executed &&
          s.vr[12].lane == std::array<std::uint32_t,4>{
              0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU},
          "VMX VCMPGEFP compares floating-point lanes");
    check(execute(s, vx_form(13, 1, 2, 710U)) == StepResult::Executed &&
          s.vr[13].lane == std::array<std::uint32_t,4>{
              0U, 0xFFFFFFFFU, 0xFFFFFFFFU, 0U},
          "VMX VCMPGTFP compares floating-point lanes");
    s.vr[1].lane = {0x10203040U, 0x50607080U, 0x90A0B0C0U, 0xD0E0F001U};
    check(execute(s, vx_form(17, 1U, 3U, 524U)) == StepResult::Executed &&
          s.vr[17].lane == std::array<std::uint32_t,4>{
              0x40404040U, 0x40404040U, 0x40404040U, 0x40404040U},
          "VMX VSPLTB broadcasts the selected byte");
    check(execute(s, vx_form(18, 1U, 4U, 588U)) == StepResult::Executed &&
          s.vr[18].lane == std::array<std::uint32_t,4>{
              0xA0B0A0B0U, 0xA0B0A0B0U, 0xA0B0A0B0U, 0xA0B0A0B0U},
          "VMX VSPLTH broadcasts the selected halfword");
    check(execute(s, vx_form(19, 1U, 2U, 652U)) == StepResult::Executed &&
          s.vr[19].lane == std::array<std::uint32_t,4>{
              0x90A0B0C0U, 0x90A0B0C0U, 0x90A0B0C0U, 0x90A0B0C0U},
          "VMX VSPLTW broadcasts the selected word");
    check(execute(s, vx_form(14, 0x1EU, 0U, 780U)) == StepResult::Executed &&
          s.vr[14].lane == std::array<std::uint32_t,4>{
              0xFEFEFEFEU, 0xFEFEFEFEU, 0xFEFEFEFEU, 0xFEFEFEFEU},
          "VMX VSPLTISB broadcasts a signed byte immediate");
    check(execute(s, vx_form(15, 0x11U, 0U, 844U)) == StepResult::Executed &&
          s.vr[15].lane == std::array<std::uint32_t,4>{
              0xFFF1FFF1U, 0xFFF1FFF1U, 0xFFF1FFF1U, 0xFFF1FFF1U},
          "VMX VSPLTISH broadcasts a signed halfword immediate");
    check(execute(s, vx_form(16, 0x1FU, 0U, 908U)) == StepResult::Executed &&
          s.vr[16].lane == std::array<std::uint32_t,4>{
              0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU},
          "VMX VSPLTISW broadcasts a signed word immediate");
    s.vr[1].lane = {0x01030507U, 0x00020004U, 0x00000002U, 0xFEFCFAF8U};
    s.vr[2].lane = {0x02040608U, 0x00040006U, 0x00000004U, 0x00020406U};
    check(execute(s, vx_form(3, 1, 2, 4)) == StepResult::Executed &&
          s.vr[3].lane[0] == 0x02040608U,
          "VMX VAVGUB rounds unsigned byte averages upward");
    check(execute(s, vx_form(3, 1, 2, 68)) == StepResult::Executed &&
          s.vr[3].lane[1] == 0x00030005U,
          "VMX VAVGUH rounds unsigned halfword averages upward");
    check(execute(s, vx_form(3, 1, 2, 132)) == StepResult::Executed &&
          s.vr[3].lane[2] == 0x00000003U,
          "VMX VAVGUW rounds unsigned word averages upward");
    check(execute(s, vx_form(3, 1, 2, 260)) == StepResult::Executed &&
          s.vr[3].lane[3] == 0xFFFFFFFFU,
          "VMX VAVGSB rounds signed byte averages toward positive infinity");
    s.vr[1].lane[1] = 0xFFFEFFFCU;
    s.vr[2].lane[1] = 0x00020004U;
    check(execute(s, vx_form(3, 1, 2, 324)) == StepResult::Executed &&
          s.vr[3].lane[1] == 0U,
          "VMX VAVGSH handles negative and positive halfword lanes");
    s.vr[1].lane[2] = 0xFFFFFFFEU;
    s.vr[2].lane[2] = 0U;
    check(execute(s, vx_form(3, 1, 2, 388)) == StepResult::Executed &&
          s.vr[3].lane[2] == 0xFFFFFFFFU,
          "VMX VAVGSW rounds negative odd word averages correctly");
    s.vr[1].lane = {0xFF0180FEU, 0x80000001U, 0xFFFFFFFFU, 0x7FFFFFFFU};
    s.vr[2].lane = {0x0102FF7FU, 0x7FFFFFFFU, 0x00000000U, 0x80000000U};
    check(execute(s, vx_form(3, 1, 2, 386)) == StepResult::Executed &&
          s.vr[3].lane[0] == 0xFF0180FEU &&
          s.vr[3].lane[1] == 0x80000001U &&
          s.vr[3].lane[3] == 0x80000000U, "VMX VMINSW compares 32-bit lanes as signed");
    std::array<std::uint8_t, 32> vector_ram{
        0x01,0x23,0x45,0x67, 0x89,0xAB,0xCD,0xEF,
        0x10,0x32,0x54,0x76, 0x98,0xBA,0xDC,0xFE,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0
    };
    s.gpr[5] = 3;
    check(execute(s, indexed(10, 0, 5, 103), vector_ram) == StepResult::Executed &&
          s.vr[10].lane == std::array<std::uint32_t,4>{
              0x01234567U,0x89ABCDEFU,0x10325476U,0x98BADCFEU},
          "LVX loads aligned 128-bit vector in big-endian lane order");
    s.vr[11].lane = {0x11223344U,0x55667788U,0x99AABBCCU,0xDDEEFF00U};
    s.gpr[5] = 19;
    check(execute(s, indexed(11, 0, 5, 231), vector_ram) == StepResult::Executed,
          "STVX stores aligned 128-bit vector");
    check(vector_ram[16] == 0x11 && vector_ram[19] == 0x44 &&
          vector_ram[20] == 0x55 && vector_ram[23] == 0x88 &&
          vector_ram[24] == 0x99 && vector_ram[27] == 0xCC &&
          vector_ram[28] == 0xDD && vector_ram[31] == 0x00,
          "STVX writes four big-endian vector lanes");
    check(execute(s, indexed(12, 0, 5, 359), vector_ram) == StepResult::Executed &&
          s.vr[12].lane == s.vr[11].lane, "LVXL loads vector from aligned block");
    s.gpr[5] = 32;
    check(execute(s, indexed(12, 0, 5, 103), vector_ram) == StepResult::MemoryFault,
          "LVX reports vector read beyond memory");


    // LMW/STMW: complete big-endian register-window transfers.
    std::array<std::uint8_t, 16> multiple_ram{
        0x11,0x22,0x33,0x44, 0x55,0x66,0x77,0x88,
        0x99,0xAA,0xBB,0xCC, 0xDD,0xEE,0xF0,0x01
    };
    s.reset(0x2000);
    check(execute(s, d_form(46, 28, 0, 0), multiple_ram) == StepResult::Executed,
          "LMW loads GPR28 through GPR31");
    check(s.gpr[28] == 0x11223344U && s.gpr[29] == 0x55667788U &&
          s.gpr[30] == 0x99AABBCCU && s.gpr[31] == 0xDDEEF001U,
          "LMW loads all registers in big-endian order");
    s.gpr[28] = 0x01020304U; s.gpr[29] = 0x11223344U;
    s.gpr[30] = 0x55667788U; s.gpr[31] = 0x99AABBCCU;
    check(execute(s, d_form(47, 28, 0, 0), multiple_ram) == StepResult::Executed,
          "STMW stores GPR28 through GPR31");
    check(multiple_ram[0] == 0x01 && multiple_ram[3] == 0x04 &&
          multiple_ram[4] == 0x11 && multiple_ram[7] == 0x44 &&
          multiple_ram[8] == 0x55 && multiple_ram[11] == 0x88 &&
          multiple_ram[12] == 0x99 && multiple_ram[15] == 0xCC,
          "STMW writes all registers in big-endian order");
    const auto multiple_fault_pc = s.pc;
    check(execute(s, d_form(46, 28, 0, 1), multiple_ram) == StepResult::AlignmentFault,
          "LMW rejects unaligned address");
    check(s.pc == multiple_fault_pc, "unaligned LMW leaves PC unchanged");
    std::array<std::uint8_t, 8> short_multiple_ram{};
    check(execute(s, d_form(46, 28, 0, 0), short_multiple_ram) == StepResult::MemoryFault,
          "LMW reports incomplete memory range");

    // Indexed loads/stores: XO 23 is LWZX; XO 20 is LWARX.
    s.reset(0x3000);
    s.gpr[3] = 0; s.gpr[5] = 0;
    check(execute(s, indexed(6, 3, 5, 23), indexed_ram) == StepResult::Executed &&
          s.gpr[6] == 0x12345678U, "LWZX reads indexed word");
    check(execute(s, indexed(7, 3, 5, 20), indexed_ram) == StepResult::Executed &&
          s.gpr[7] == 0x12345678U && s.reservation_valid,
          "LWARX loads word and establishes reservation");
    s.gpr[7] = 0xA1B2C3D4U;
    check(execute(s, indexed(7, 3, 5, 150, true), indexed_ram) == StepResult::Executed,
          "STWCX executes with Rc set");
    check(indexed_ram[0] == 0xA1 && indexed_ram[1] == 0xB2 &&
          indexed_ram[2] == 0xC3 && indexed_ram[3] == 0xD4 &&
          s.condition_field(0) == 0x2 && !s.reservation_valid,
          "STWCX stores when reservation matches and sets CR0 EQ");
    s.gpr[7] = 0x55667788U;
    check(execute(s, indexed(7, 3, 5, 150, true), indexed_ram) == StepResult::Executed,
          "STWCX without reservation completes as failed conditional store");
    check(indexed_ram[0] == 0xA1 && s.condition_field(0) == 0,
          "failed STWCX does not modify memory and clears CR0 EQ");
    check(execute(s, indexed(7, 3, 5, 20), indexed_ram) == StepResult::Executed,
          "LWARX can establish a new reservation");
    const std::array<std::uint8_t, 2> adjacent_before{indexed_ram[4], indexed_ram[5]};
    s.gpr[5] = 4; s.gpr[7] = 0xCAFEBABEU;
    check(execute(s, indexed(7, 3, 5, 150, true), indexed_ram) == StepResult::Executed &&
          s.condition_field(0) == 0 && indexed_ram[4] == adjacent_before[0] &&
          indexed_ram[5] == adjacent_before[1],
          "STWCX at a different word fails without modifying adjacent memory");
    s.gpr[5] = 0;
    check(execute(s, indexed(7, 3, 5, 20), indexed_ram) == StepResult::Executed,
          "LWARX restores reservation at exact word address");
    s.gpr[8] = 0xDEADBEEFU;
    check(execute(s, d_form(36, 8, 3, 4), indexed_ram) == StepResult::Executed,
          "ordinary store executes while reservation is active");
    check(!s.reservation_valid, "overlapping ordinary store invalidates reservation");

    // X-form integer logical instructions, including Rc/CR0 behavior.
    s.reset(0x4800);
    s.gpr[3] = 0xF0F00FF0F0F00FF0ULL;
    s.gpr[4] = 0x0FF0FF0F0FF0FF0FULL;
    check(execute(s, x_form(3, 5, 4, 28)) == StepResult::Executed &&
          s.gpr[5] == (s.gpr[3] & s.gpr[4]), "AND computes register bitwise intersection");
    check(execute(s, x_form(3, 6, 4, 60)) == StepResult::Executed &&
          s.gpr[6] == (s.gpr[3] & ~s.gpr[4]), "ANDC computes A AND NOT B");
    check(execute(s, x_form(3, 7, 4, 444)) == StepResult::Executed &&
          s.gpr[7] == (s.gpr[3] | s.gpr[4]), "OR computes register bitwise union");
    check(execute(s, x_form(3, 8, 4, 316)) == StepResult::Executed &&
          s.gpr[8] == (s.gpr[3] ^ s.gpr[4]), "XOR computes register exclusive-or");
    check(execute(s, x_form(3, 9, 4, 124)) == StepResult::Executed &&
          s.gpr[9] == ~(s.gpr[3] | s.gpr[4]), "NOR computes complement of OR");
    check(execute(s, x_form(3, 10, 4, 284)) == StepResult::Executed &&
          s.gpr[10] == ~(s.gpr[3] ^ s.gpr[4]), "EQV computes complement of XOR");
    check(execute(s, x_form(3, 11, 4, 412)) == StepResult::Executed &&
          s.gpr[11] == (s.gpr[3] | ~s.gpr[4]), "ORC computes A OR NOT B");
    check(execute(s, x_form(3, 12, 4, 476)) == StepResult::Executed &&
          s.gpr[12] == ~(s.gpr[3] & s.gpr[4]), "NAND computes complement of AND");
    check(execute(s, x_form(3, 13, 4, 28, true)) == StepResult::Executed &&
          s.gpr[13] == (s.gpr[3] & s.gpr[4]) &&
          s.condition_field(0) == 0x4U, "AND. updates CR0 for positive nonzero result");

    // XL-form BCLR: branch through LR, with BO/BI condition handling.
    s.reset(0x5000);
    s.lr = 0x2347;
    const std::uint32_t bclr = (19U << 26U) | (20U << 21U) | (16U << 1U);
    check(execute(s, bclr) == StepResult::Executed && s.pc == 0x2344U,
          "BCLR branches to aligned link-register target");
    s.reset(0x5100);
    s.lr = 0x3450;
    s.set_condition_field(0, 0x2U); // EQ=1
    const std::uint32_t bclr_if_eq = (19U << 26U) | (12U << 21U) |
        (2U << 16U) | (16U << 1U);
    check(execute(s, bclr_if_eq) == StepResult::Executed && s.pc == 0x3450U,
          "BCLR honors selected CR condition bit");
    s.reset(0x5200);
    s.lr = 0x4560;
    s.set_condition_field(0, 0x4U); // EQ=0
    check(execute(s, bclr_if_eq) == StepResult::Executed && s.pc == 0x5204U,
          "BCLR falls through when CR condition is false");
    s.reset(0x5300);
    s.lr = 0x5670;
    s.ctr = 2;
    const std::uint32_t bclr_ctr = (19U << 26U) | (16U << 21U) |
        (16U << 1U);
    check(execute(s, bclr_ctr) == StepResult::Executed && s.pc == 0x5670U &&
          s.ctr == 1U, "BCLR decrements CTR and branches when count test passes");

    // XL-form BCCTR: branch through CTR; unlike BCLR it must not decrement CTR.
    s.reset(0x5400);
    s.ctr = 0x6783;
    const std::uint32_t bcctr = (19U << 26U) | (20U << 21U) | (528U << 1U);
    check(execute(s, bcctr) == StepResult::Executed && s.pc == 0x6780U &&
          s.ctr == 0x6783U, "BCCTR branches to aligned CTR target without decrement");
    s.reset(0x5500);
    s.ctr = 0x7890;
    s.set_condition_field(0, 0x2U);
    const std::uint32_t bcctr_if_eq = (19U << 26U) | (12U << 21U) |
        (2U << 16U) | (528U << 1U);
    check(execute(s, bcctr_if_eq) == StepResult::Executed && s.pc == 0x7890U,
          "BCCTR honors CR condition");
    s.reset(0x5600);
    s.ctr = 0x89A0;
    s.set_condition_field(0, 0x4U);
    check(execute(s, bcctr_if_eq) == StepResult::Executed && s.pc == 0x5604U,
          "BCCTR falls through when CR condition is false");
    s.reset(0x5700);
    s.ctr = 0x9AB0;
    const std::uint32_t invalid_bcctr = (19U << 26U) | (16U << 21U) |
        (528U << 1U);
    check(execute(s, invalid_bcctr) == StepResult::UnsupportedInstruction &&
          s.pc == 0x5700U, "BCCTR rejects BO encoding that requests CTR decrement");

    // Link variants must capture the old target before updating LR.
    s.reset(0x5800);
    s.lr = 0xA123;
    const std::uint32_t bclrl = bclr | 1U;
    check(execute(s, bclrl) == StepResult::Executed && s.pc == 0xA120U &&
          s.lr == 0x5804U, "BCLRL branches to old LR and writes return address");
    s.reset(0x5900);
    s.ctr = 0xB123;
    const std::uint32_t bcctrl = bcctr | 1U;
    check(execute(s, bcctrl) == StepResult::Executed && s.pc == 0xB120U &&
          s.ctr == 0xB123U && s.lr == 0x5904U,
          "BCCTRL branches to old CTR, preserves CTR and writes return address");

    // Integer multiply/divide coverage (X-form and D-form).
    s.reset(0x6000);
    s.gpr[3] = 0x100000000ULL;
    check(execute(s, d_form(7, 4, 3, 0xFFFEU)) == StepResult::Executed &&
          s.gpr[4] == 0xFFFFFFFE00000000ULL,
          "MULLI multiplies 64-bit RA by signed immediate");
    s.reset(0x6010);
    s.gpr[3] = 0x7FFFFFFFULL;
    s.gpr[5] = 2U;
    check(execute(s, x_form(6, 3, 5, 235)) == StepResult::Executed &&
          s.gpr[6] == 0xFFFFFFFFFFFFFFFEULL,
          "MULLW keeps low 32-bit product and sign-extends result");
    s.reset(0x6020);
    s.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(-21));
    s.gpr[5] = 4U;
    check(execute(s, x_form(6, 3, 5, 491)) == StepResult::Executed &&
          s.gpr[6] == 0xFFFFFFFFFFFFFFFBU,
          "DIVW performs signed division rounded toward zero");
    s.reset(0x6030);
    s.gpr[3] = 21U;
    s.gpr[5] = 4U;
    check(execute(s, x_form(6, 3, 5, 459)) == StepResult::Executed &&
          s.gpr[6] == 5U,
          "DIVWU performs unsigned word division");
    // OE variants report signed overflow in XER[OV,SO]; SO remains sticky.
    constexpr std::uint64_t xer_ov = std::uint64_t{1} << 30U;
    constexpr std::uint64_t xer_so = std::uint64_t{1} << 31U;
    s.reset(0x6040);
    s.gpr[3] = 0x7FFFFFFFU;
    s.gpr[5] = 2U;
    check(execute(s, x_form(6, 3, 5, 235) | (1U << 10U)) == StepResult::Executed &&
          (s.xer & (xer_ov | xer_so)) == (xer_ov | xer_so),
          "MULLW with OE sets OV and sticky SO on signed overflow");
    s.reset(0x6050);
    s.gpr[3] = 6U;
    s.gpr[5] = 7U;
    s.xer = xer_so | xer_ov;
    check(execute(s, x_form(6, 3, 5, 235) | (1U << 10U)) == StepResult::Executed &&
          (s.xer & xer_ov) == 0 && (s.xer & xer_so) != 0,
          "MULLW with OE clears OV without clearing sticky SO");
    s.reset(0x6060);
    s.gpr[3] = 123U;
    s.gpr[5] = 0U;
    check(execute(s, x_form(6, 3, 5, 491) | (1U << 10U)) == StepResult::Executed &&
          (s.xer & (xer_ov | xer_so)) == (xer_ov | xer_so),
          "DIVW with OE reports divide-by-zero overflow");
    s.reset(0x6070);
    s.gpr[3] = static_cast<std::uint32_t>(INT32_MIN);
    s.gpr[5] = UINT32_MAX;
    check(execute(s, x_form(6, 3, 5, 491) | (1U << 10U)) == StepResult::Executed &&
          (s.xer & (xer_ov | xer_so)) == (xer_ov | xer_so),
          "DIVW with OE reports signed minimum divided by negative one");
    s.reset(0x6080);
    s.gpr[3] = UINT64_MAX;
    s.gpr[5] = 1U;
    s.xer = xer_ov | xer_so;
    check(execute(s, x_form(6, 3, 5, 266) | (1U << 10U)) == StepResult::Executed &&
          s.gpr[6] == 0U && (s.xer & xer_ov) == 0 && (s.xer & xer_so) != 0,
          "ADD with OE does not report overflow for negative one plus one");
    s.reset(0x6090);
    s.gpr[3] = 0x7FFFFFFFFFFFFFFFULL;
    s.gpr[5] = 1U;
    check(execute(s, x_form(6, 3, 5, 266) | (1U << 10U)) == StepResult::Executed &&
          s.gpr[6] == 0x8000000000000000ULL &&
          (s.xer & (xer_ov | xer_so)) == (xer_ov | xer_so),
          "ADD with OE reports maximum positive plus one");
    s.reset(0x60A0);
    s.gpr[3] = 0x8000000000000000ULL;
    s.gpr[5] = 1U;
    check(execute(s, x_form(6, 3, 5, 40) | (1U << 10U)) == StepResult::Executed &&
          s.gpr[6] == 0x8000000000000001ULL &&
          (s.xer & (xer_ov | xer_so)) == (xer_ov | xer_so),
          "SUBF with OE reports signed subtraction overflow");
    s.reset(0x60B0);
    s.gpr[3] = 123U;
    s.gpr[5] = 0U;
    check(execute(s, x_form(6, 3, 5, 459) | (1U << 10U)) == StepResult::Executed &&
          (s.xer & (xer_ov | xer_so)) == (xer_ov | xer_so),
          "DIVWU with OE reports division by zero");
    s.reset(0x60C0);
    s.gpr[3] = 1U;
    s.gpr[5] = 2U;
    s.xer = xer_ov | xer_so;
    check(execute(s, x_form(6, 3, 5, 266) | (1U << 10U)) == StepResult::Executed &&
          (s.xer & xer_ov) == 0 && (s.xer & xer_so) != 0,
          "ADD with OE clears OV on success and preserves sticky SO");
    s.reset(0x60D0);
    s.gpr[3] = 0x7FFFFFFFFFFFFFFFULL;
    s.gpr[5] = 1U;
    check(execute(s, x_form(6, 3, 5, 778, true)) == StepResult::Executed &&
          s.condition_field(0) == 0x9U,
          "ADD. with OE records negative result and sticky SO in CR0");
    s.reset(0x60E0);
    s.gpr[3] = 123U;
    s.gpr[5] = 7U;
    s.xer = xer_ov | xer_so;
    check(execute(s, x_form(6, 3, 5, 459) | (1U << 10U)) == StepResult::Executed &&
          (s.xer & xer_ov) == 0 && (s.xer & xer_so) != 0,
          "DIVWU with OE clears OV on success and preserves sticky SO");



    // Privileged machine-state register access: MFMSR / MTMSR.
    s.reset(0x6100);
    s.msr = 0x8000U;
    const std::uint32_t mfmsr = (31U << 26U) | (7U << 21U) | (83U << 1U);
    check(execute(s, mfmsr) == StepResult::Executed &&
          s.gpr[7] == 0x8000U && s.pc == 0x6104U,
          "MFMSR reads MSR in privileged state");
    s.reset(0x6110);
    s.msr = 0x4000U; // PR=1: problem state.
    s.gpr[7] = 0x1234U;
    check(execute(s, mfmsr) == StepResult::ProgramException &&
          s.gpr[7] == 0x1234U && s.pc == 0x6110U,
          "MFMSR is rejected in problem state without modifying destination");
    s.reset(0x6120);
    s.msr = 0x4000U;
    s.gpr[7] = 0x8000U;
    const std::uint32_t mtmsr = (31U << 26U) | (7U << 21U) | (146U << 1U);
    check(execute(s, mtmsr) == StepResult::ProgramException &&
          s.msr == 0x4000U && s.pc == 0x6120U,
          "MTMSR is rejected in problem state without changing MSR");
    s.reset(0x6130);
    s.gpr[7] = 0x8000U;
    check(execute(s, mtmsr) == StepResult::Executed &&
          s.msr == 0x8000U && s.pc == 0x6134U,
          "MTMSR updates MSR in privileged state");
    s.reset(0x6140);
    s.msr = 0x4000U;
    check(execute_with_exception_delivery(s, mfmsr) == StepResult::ProgramException &&
          s.exception_pending && s.exception_vector == 0x700U &&
          s.srr0 == 0x6140U && s.srr1 == 0x4000U && s.pc == 0x700U,
          "privileged instruction violation enters program exception vector");

    // XFX SPR access: the ten-bit SPR number is split across instruction fields.
    const auto xfx_spr = [](unsigned rt, unsigned spr, unsigned xo) {
        return (31U << 26U) | (rt << 21U) | ((spr & 0x1FU) << 16U) |
               (((spr >> 5U) & 0x1FU) << 11U) | (xo << 1U);
    };
    s.reset(0x6150);
    s.xer = 0x00000000A0000000ULL;
    check(execute(s, xfx_spr(8, 1, 339)) == StepResult::Executed &&
          s.gpr[8] == s.xer && s.pc == 0x6154U,
          "MFSPR reads XER using split SPR encoding");
    s.reset(0x6160);
    s.lr = 0x12345678ULL;
    check(execute(s, xfx_spr(9, 8, 339)) == StepResult::Executed &&
          s.gpr[9] == 0x12345678ULL, "MFSPR reads LR");
    s.reset(0x6170);
    s.ctr = 0x87654321ULL;
    check(execute(s, xfx_spr(10, 9, 339)) == StepResult::Executed &&
          s.gpr[10] == 0x87654321ULL, "MFSPR reads CTR");
    s.reset(0x6180);
    s.gpr[11] = 0x13579BDFULL;
    check(execute(s, xfx_spr(11, 1, 467)) == StepResult::Executed &&
          s.xer == 0x13579BDFULL, "MTSPR writes XER");
    s.reset(0x6190);
    s.gpr[12] = 0x2468ACE0ULL;
    check(execute(s, xfx_spr(12, 8, 467)) == StepResult::Executed &&
          s.lr == 0x2468ACE0ULL, "MTSPR writes LR");
    s.reset(0x61A0);
    s.gpr[13] = 0xABCDEF01ULL;
    check(execute(s, xfx_spr(13, 9, 467)) == StepResult::Executed &&
          s.ctr == 0xABCDEF01ULL, "MTSPR writes CTR");
    s.reset(0x61B0);
    check(execute(s, xfx_spr(14, 268, 339)) == StepResult::UnsupportedInstruction &&
          s.pc == 0x61B0U,
          "MFSPR rejects unimplemented time-base SPR without changing PC");

    // MFTB reads TBL (268) and TBU (269), using the XFX split TBR field.
    s.reset(0x61C0);
    s.timebase_ticks = 0x89ABCDEF12345678ULL;
    check(execute(s, xfx_spr(15, 268, 371)) == StepResult::Executed &&
          s.gpr[15] == 0x12345678U && s.pc == 0x61C4U,
          "MFTB reads lower time-base word");
    s.reset(0x61D0);
    s.timebase_ticks = 0x89ABCDEF12345678ULL;
    check(execute(s, xfx_spr(16, 269, 371)) == StepResult::Executed &&
          s.gpr[16] == 0x89ABCDEFU && s.pc == 0x61D4U,
          "MFTB reads upper time-base word");
    s.reset(0x61E0);
    check(execute(s, xfx_spr(17, 270, 371)) == StepResult::UnsupportedInstruction &&
          s.pc == 0x61E0U,
          "MFTB rejects unsupported TBR without changing PC");

    s.reset(0x61F0);
    s.timebase_ticks = 0x1122334455667788ULL;
    s.gpr[18] = 0xAABBCCDDU;
    check(execute(s, xfx_spr(18, 284, 467)) == StepResult::Executed &&
          s.timebase_ticks == 0x11223344AABBCCDDULL,
          "MTSPR writes TBL while preserving TBU");
    s.reset(0x6200);
    s.timebase_ticks = 0x1122334455667788ULL;
    s.gpr[19] = 0xDEADBEEFU;
    check(execute(s, xfx_spr(19, 285, 467)) == StepResult::Executed &&
          s.timebase_ticks == 0xDEADBEEF55667788ULL,
          "MTSPR writes TBU while preserving TBL");
    s.reset(0x6210);
    s.msr = 0x4000U;
    s.gpr[20] = 0x12345678U;
    check(execute_with_exception_delivery(s, xfx_spr(20, 284, 467)) == StepResult::ProgramException &&
          s.exception_pending && s.exception_vector == 0x700U &&
          s.timebase_ticks == 0,
          "MTSPR TBL write is rejected in problem state");

    // MFSR/MTSR access the architectural SR bank and require supervisor state.
    s.reset(0x6220);
    s.segment_registers[3] = 0xA1234567U;
    const auto mfsr_r3 = (31U << 26U) | (21U << 21U) | (3U << 16U) | (595U << 1U);
    check(execute(s, mfsr_r3) == StepResult::Executed &&
          s.gpr[21] == 0xA1234567U && s.pc == 0x6224U,
          "MFSR reads selected segment register");
    s.reset(0x6230);
    s.gpr[22] = 0xB7654321U;
    const auto mtsr_r7 = (31U << 26U) | (22U << 21U) | (7U << 16U) | (210U << 1U);
    check(execute(s, mtsr_r7) == StepResult::Executed &&
          s.segment_registers[7] == 0xB7654321U,
          "MTSR writes selected segment register");
    s.reset(0x6240);
    s.msr = 0x4000U;
    check(execute_with_exception_delivery(s, mfsr_r3) == StepResult::ProgramException &&
          s.exception_pending && s.exception_vector == 0x700U,
          "MFSR is privileged and traps in problem state");
    s.reset(0x6250);
    s.msr = 0x4000U;
    s.gpr[22] = 0xFFFFFFFFU;
    check(execute_with_exception_delivery(s, mtsr_r7) == StepResult::ProgramException &&
          s.exception_pending && s.exception_vector == 0x700U &&
          s.segment_registers[7] == 0,
          "MTSR is privileged and does not modify SR in problem state");

    s.segment_registers[4] = 0xFFFFFFFFU;
    s.reset(0x6260);
    check(s.segment_registers[4] == 0,
          "thread reset clears all segment registers");

    // Segment-register N bit prevents instruction fetch from its 256 MiB segment.
    s.reset(0x7000);
    s.segment_registers[0] = 0;
    check(segment_allows_instruction_fetch(s, 0x01234567U),
          "segment with N clear permits instruction fetch");
    const auto segment_fields = decode_segment_register(
        0xE1234567U | segment_no_execute_mask);
    check(segment_fields.vsid == (0xA1234567U & segment_vsid_mask),
          "segment register decoder extracts the 24-bit VSID field");
    check(segment_fields.key_supervisor && segment_fields.key_problem,
          "segment register decoder extracts both protection key bits");
    check(decode_segment_register(segment_key_supervisor_mask).key_supervisor &&
          !decode_segment_register(segment_key_supervisor_mask).key_problem,
          "segment register Ks is the architectural bit 1");
    check(decode_segment_register(segment_key_problem_mask).key_problem &&
          !decode_segment_register(segment_key_problem_mask).key_supervisor,
          "segment register Kp is the architectural bit 2");
    check(segment_fields.no_execute && segment_fields.direct_store,
          "segment register decoder extracts N and direct-store bits");
    const auto plain_segment = decode_segment_register(0x00123456U);
    check(plain_segment.vsid == 0x00123456U &&
          !plain_segment.key_supervisor && !plain_segment.key_problem &&
          !plain_segment.no_execute && !plain_segment.direct_store,
          "segment register decoder handles a plain VSID");
    constexpr std::uint64_t hash_test_ea = 0x12345ABCULL;
    static_assert(segment_page_index(hash_test_ea) == 0x2345U);
    static_assert(segment_primary_hash(0x00123456U, hash_test_ea) == 0x00121713U);
    static_assert(segment_secondary_hash(0x00123456U, hash_test_ea) == 0xFFEDE8ECU);
    check(segment_page_index(hash_test_ea) == 0x2345U,
          "PowerPC hashed-page-table page index uses EA bits 12 through 27");
    check(segment_primary_hash(0x00123456U, hash_test_ea) == 0x00121713U,
          "PowerPC primary page hash combines VSID and page index");
    check(segment_secondary_hash(0x00123456U, hash_test_ea) == 0xFFEDE8ECU,
          "PowerPC secondary page hash complements the primary hash");
    s.segment_registers[1] = segment_no_execute_mask;
    check(!segment_allows_instruction_fetch(s, 0x11234567U),
          "segment N bit blocks instruction fetch");
    check(segment_allows_instruction_fetch(s, 0x01234567U),
          "N bit in one segment does not block another segment");
    s.segment_registers[15] = segment_no_execute_mask;
    check(!segment_allows_instruction_fetch(s, 0xF0000000U),
          "segment 15 no-execute protection is selected by EA high nibble");

    // End-to-end synthetic fetch through AddressSpaceMemoryBus and SR protection.
    {
        onix360::memory::PhysicalMemory ram(8192);
        onix360::memory::AddressSpace mmu;
        check(mmu.map_page(0x1000, 0x0000, 5U) == onix360::memory::MemoryResult::Ok,
              "maps executable guest page for segment fetch test");
        ram.raw_bytes()[0] = 0x60U; ram.raw_bytes()[1] = 0x00U;
        ram.raw_bytes()[2] = 0x00U; ram.raw_bytes()[3] = 0x00U;
        s.reset(0x1000);
        onix360::cpu::AddressSpaceMemoryBus bus{&mmu, &ram, &s};
        const auto callbacks = bus.callbacks();
        std::uint32_t fetched_word = 0;
        std::array<std::uint8_t,4> fetched_bytes{};
        s.segment_registers[0] = onix360::cpu::segment_no_execute_mask;
        check(!callbacks.fetch(callbacks.context, 0x1000, fetched_bytes),
              "bus denies instruction fetch when segment N bit is set");
        s.segment_registers[0] = 0;
        check(callbacks.fetch(callbacks.context, 0x1000, fetched_bytes) &&
              fetched_bytes[0] == 0x60U && fetched_bytes[3] == 0x00U,
              "bus fetch succeeds after segment execute permission is restored");
        onix360::cpu::AddressSpaceMemoryBus unbound_bus{&mmu, &ram, nullptr};
        const auto unbound_callbacks = unbound_bus.callbacks();
        check(!unbound_callbacks.fetch(unbound_callbacks.context, 0x1000, fetched_bytes),
              "bus fails closed when no thread state is bound for segment permission checks");
        fetched_word = (static_cast<std::uint32_t>(fetched_bytes[0]) << 24U) |
                       (static_cast<std::uint32_t>(fetched_bytes[1]) << 16U) |
                       (static_cast<std::uint32_t>(fetched_bytes[2]) << 8U) |
                       fetched_bytes[3];
        check(fetched_word == 0x60000000U,
              "segment-protected bus preserves big-endian instruction bytes");
    }

    // End-to-end segment T-bit translation through the guest HTAB/PTE walker.
    {
        onix360::memory::PhysicalMemory ram(1U << 20U);
        onix360::memory::AddressSpace software_mmu;
        onix360::memory::PowerPcHashedPageTable htab;
        constexpr std::uint32_t vsid = 0x12345U;
        constexpr std::uint64_t effective = 0x234U;
        check(htab.configure(0x10000, 1024, ram) == onix360::memory::MemoryResult::Ok,
              "configures HTAB for bus integration");
        check(htab.insert(vsid, effective, 0x8000, 0, ram) ==
                  onix360::memory::MemoryResult::Ok,
              "inserts segment translation PTE");
        ram.raw_bytes()[0x8234] = 0x60U;
        ram.raw_bytes()[0x8235] = 0x00U;
        ram.raw_bytes()[0x8236] = 0x00U;
        ram.raw_bytes()[0x8237] = 0x00U;
        s.reset(effective);
        s.segment_registers[0] =
            vsid;
        onix360::cpu::AddressSpaceMemoryBus bus{
            &software_mmu, &ram, &s, &htab};
        check(bus.configure_htab_from_sdr1(0x00010000U) ==
                  onix360::memory::MemoryResult::Ok,
              "configures integrated HTAB through guest SDR1 value");
        const auto callbacks = bus.callbacks();
        std::array<std::uint8_t,4> fetched{};
        check(callbacks.fetch(callbacks.context, effective, fetched) &&
              fetched[0] == 0x60U && fetched[3] == 0x00U,
              "instruction fetch resolves through segment VSID and HTAB PTE");
        const std::array<std::uint8_t,4> replacement{0x12U,0x34U,0x56U,0x78U};
        check(callbacks.write(callbacks.context, effective, replacement),
              "data write resolves through segment HTAB translation");
        std::array<std::uint8_t,4> readback{};
        check(callbacks.read(callbacks.context, effective, readback) &&
              readback == replacement,
              "data read resolves the same translated physical page");
        constexpr std::uint64_t crossing_ea = 0x2FFCU;
        check(htab.insert(vsid, 0x2000U, 0xA000U, 0, ram) ==
                  onix360::memory::MemoryResult::Ok &&
              htab.insert(vsid, 0x3000U, 0xB000U, 0, ram) ==
                  onix360::memory::MemoryResult::Ok,
              "maps adjacent virtual pages to adjacent physical pages");
        const auto crossing = bus.translate(
            crossing_ea, 8, onix360::memory::Access::Read);
        check(crossing.result == onix360::memory::MemoryResult::Ok &&
              crossing.physical_address == 0xAFFCU,
              "hashed MMU translates an access spanning adjacent pages");
        s.segment_registers[0] =
            segment_key_problem_mask | vsid;
        s.msr = (1ULL << 14U); // Problem state selects Kp.
        check(!callbacks.read(callbacks.context, effective, readback),
              "user key is denied by PP=00");
        constexpr std::uint64_t user_rw_ea = 0x1234U;
        check(htab.insert(vsid, user_rw_ea, 0x9000, 2, ram) ==
                  onix360::memory::MemoryResult::Ok,
              "inserts user read-write PTE");
        check(callbacks.write(callbacks.context, user_rw_ea, replacement),
              "user key with PP=10 is granted read-write access");
        check(callbacks.read(callbacks.context, user_rw_ea, readback) &&
              readback == replacement,
              "user key can read PP=10 mapping");
        s.segment_registers[0] = segment_translation_mask | vsid; // T=1 is direct-store.
        check(!callbacks.fetch(callbacks.context, effective, fetched),
              "direct-store segment is rejected instead of treated as hashed-page translation");
    }

    if (failures == 0) std::cout << "Onix360 PowerPC interpreter tests passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
