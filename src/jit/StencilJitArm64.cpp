#include "JitInternal.h"

#include <iomanip>
#include <sstream>

#if UINTPTR_MAX == 0xffffffffffffffffULL

namespace realscript::jit::jit_detail {
namespace {

// P2-A Tier 2B: Native ARM64 (AArch64) Stencil Machine Code Emitter
// Covers macOS Apple Silicon (Darwin ARM64), iOS, Android/Linux AAPCS64 & Win-ARM64
// Strictly avoids platform-reserved register X18 across all OS targets!
// ============================================================================

enum class A64Reg : std::uint8_t {
    X0 = 0,
    X1 = 1,
    X2 = 2,
    X3 = 3,
    X4 = 4,
    X5 = 5,
    X6 = 6,
    X7 = 7,
    X8 = 8,
    X9 = 9,   // Primary address-offset scratch register
    X10 = 10, // Primary immediate/tag scratch register
    X11 = 11, // Auxiliary loop/slice scratch register
    X12 = 12,
    X13 = 13,
    X14 = 14,
    X15 = 15,
    X16 = 16, // IP0: C++ helper call target register (`blr x16`)
    X17 = 17, // IP1
    // NOTE: X18 is INTENTIONALLY OMITTED (platform-reserved on Apple Darwin ARM64 & Windows ARM64)
    X19 = 19, // Callee-saved: pinned `JitAbiContext* ctx`
    X20 = 20, // Callee-saved: pinned `runtime::Value* frameBase`
    X21 = 21, // Callee-saved: pinned `uint64_t remainingBudget`
    X22 = 22, // Callee-saved: pinned `uint64_t branchesTaken`
    X23 = 23, // Callee-saved
    X24 = 24, // Callee-saved
    X29 = 29, // FP
    X30 = 30, // LR
    XZR = 31, // Zero register (or SP in stack/immediate base contexts)
};

enum class A64Cond : std::uint8_t {
    EQ = 0x0, // Z == 1
    NE = 0x1, // Z == 0
    HS = 0x2, // C == 1 (unsigned >=, CS)
    LO = 0x3, // C == 0 (unsigned <,  CC / borrow on SUBS)
    MI = 0x4, // N == 1 (FCMP ordered <)
    PL = 0x5, // N == 0
    VS = 0x6, // V == 1 (signed overflow / FCMP unordered NaN)
    VC = 0x7, // V == 0
    HI = 0x8, // C == 1 && Z == 0 (unsigned >)
    LS = 0x9, // !(C == 1 && Z == 0) (unsigned <= / FCMP ordered <=)
    GE = 0xA, // N == V (signed >= / FCMP ordered >=)
    LT = 0xB, // N != V (signed <)
    GT = 0xC, // Z == 0 && N == V (signed > / FCMP ordered >)
    LE = 0xD, // Z == 1 || N != V (signed <=)
    AL = 0xE,
};

[[nodiscard]] constexpr A64Cond invertA64Cond(A64Cond cc) noexcept {
    return static_cast<A64Cond>(static_cast<std::uint8_t>(cc) ^ 1u);
}

[[nodiscard]] constexpr A64Cond quickCmpToA64Cond(runtime::detail::QuickOpcode op) noexcept {
    using QO = runtime::detail::QuickOpcode;
    switch (op) {
    case QO::LessInt: return A64Cond::LT;
    case QO::LessOrEqualInt: return A64Cond::LE;
    case QO::GreaterInt: return A64Cond::GT;
    case QO::GreaterOrEqualInt: return A64Cond::GE;
    case QO::EqualInt: return A64Cond::EQ;
    case QO::NotEqualInt: return A64Cond::NE;
    default: return A64Cond::EQ;
    }
}

class A64Emitter {
public:
    enum class PatchKind : std::uint8_t {
        BImm26,
        BCondImm19,
    };

    struct PatchSite {
        std::size_t bytePos = 0;
        PatchKind kind = PatchKind::BImm26;
    };

    struct Label {
        std::size_t boundOffset = std::numeric_limits<std::size_t>::max();
        std::vector<PatchSite> patches;
        [[nodiscard]] bool bound() const noexcept {
            return boundOffset != std::numeric_limits<std::size_t>::max();
        }
    };

    std::uint32_t createLabel() {
        const auto id = static_cast<std::uint32_t>(labels_.size());
        labels_.emplace_back();
        return id;
    }

    void bind(std::uint32_t labelId) {
        auto& lbl = labels_[labelId];
        lbl.boundOffset = buf_.size();
        for (const auto& site : lbl.patches) {
            patchSite(site, lbl.boundOffset);
        }
        lbl.patches.clear();
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] bool ok() const noexcept {
        if (overflowed_ || (buf_.size() % 4u) != 0u) return false;
        for (const auto& lbl : labels_) {
            if (!lbl.patches.empty() && !lbl.bound()) return false;
        }
        return true;
    }

    void emitInst(std::uint32_t word) {
        std::uint8_t tmp[4];
        std::memcpy(tmp, &word, 4);
        buf_.insert(buf_.end(), tmp, tmp + 4);
    }

    // Stack frame prologue/epilogue helpers
    void subSp(std::uint16_t imm12) {
        emitInst(0xD1000000u | ((static_cast<std::uint32_t>(imm12) & 0xFFFu) << 10) | (31u << 5) | 31u);
    }
    void addSp(std::uint16_t imm12) {
        emitInst(0x91000000u | ((static_cast<std::uint32_t>(imm12) & 0xFFFu) << 10) | (31u << 5) | 31u);
    }
    void movFpFromSp() {
        emitInst(0x910003FDu); // add x29, sp, #0
    }
    void stp64Sp(A64Reg rt1, A64Reg rt2, std::int32_t byteOff) {
        const auto imm7 = static_cast<std::uint32_t>((byteOff / 8) & 0x7F);
        emitInst(0xA9000000u | (imm7 << 15) | (regCode(rt2) << 10) | (31u << 5) | regCode(rt1));
    }
    void ldp64Sp(A64Reg rt1, A64Reg rt2, std::int32_t byteOff) {
        const auto imm7 = static_cast<std::uint32_t>((byteOff / 8) & 0x7F);
        emitInst(0xA9400000u | (imm7 << 15) | (regCode(rt2) << 10) | (31u << 5) | regCode(rt1));
    }
    void ret() {
        emitInst(0xD65F03C0u);
    }
    void blr(A64Reg target) {
        emitInst(0xD63F0000u | (regCode(target) << 5));
    }

    // Register moves & wide immediates
    void movReg64(A64Reg dst, A64Reg src) {
        if (dst == src) return;
        emitInst(0xAA0003E0u | (regCode(src) << 16) | regCode(dst));
    }
    void movReg32(A64Reg dst, A64Reg src) {
        if (dst == src) return;
        emitInst(0x2A0003E0u | (regCode(src) << 16) | regCode(dst));
    }
    void movz32(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0x52800000u | (static_cast<std::uint32_t>(hw & 1u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movn32(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0x12800000u | (static_cast<std::uint32_t>(hw & 1u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movk32(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0x72800000u | (static_cast<std::uint32_t>(hw & 1u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movz64(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0xD2800000u | (static_cast<std::uint32_t>(hw & 3u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movn64(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0x92800000u | (static_cast<std::uint32_t>(hw & 3u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movk64(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0xF2800000u | (static_cast<std::uint32_t>(hw & 3u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }

    void movImm32(A64Reg dst, std::uint32_t val) {
        if ((val >> 16u) == 0u) {
            movz32(dst, static_cast<std::uint16_t>(val), 0);
            return;
        }
        const std::uint32_t inv = ~val;
        if ((inv >> 16u) == 0u) {
            movn32(dst, static_cast<std::uint16_t>(inv), 0);
            return;
        }
        movz32(dst, static_cast<std::uint16_t>(val & 0xFFFFu), 0);
        movk32(dst, static_cast<std::uint16_t>(val >> 16u), 1);
    }

    void movImm64(A64Reg dst, std::uint64_t val) {
        if (val == 0u) {
            movz32(dst, 0u, 0);
            return;
        }
        const std::uint64_t inv = ~val;
        if ((inv >> 16u) == 0u) {
            movn64(dst, static_cast<std::uint16_t>(inv & 0xFFFFu), 0);
            return;
        }
        bool first = true;
        for (std::uint8_t hw = 0; hw < 4u; ++hw) {
            const auto chunk = static_cast<std::uint16_t>((val >> (hw * 16u)) & 0xFFFFu);
            if (chunk == 0u) continue;
            if (first) {
                movz64(dst, chunk, hw);
                first = false;
            } else {
                movk64(dst, chunk, hw);
            }
        }
    }

    // Immediate & register 64-bit / 32-bit arithmetic
    void addImm64(A64Reg dst, A64Reg src, std::int64_t imm) {
        if (imm == 0) {
            movReg64(dst, src);
            return;
        }
        if (imm < 0) {
            subImm64(dst, src, -imm);
            return;
        }
        if (imm <= 4095) {
            emitInst(0x91000000u | (static_cast<std::uint32_t>(imm) << 10) | (regCode(src) << 5) | regCode(dst));
            return;
        }
        if (imm <= 0xFFFFFFLL) {
            const auto hi12 = static_cast<std::uint32_t>((imm >> 12) & 0xFFF);
            const auto lo12 = static_cast<std::uint32_t>(imm & 0xFFF);
            emitInst(0x91400000u | (hi12 << 10) | (regCode(src) << 5) | regCode(dst));
            if (lo12 != 0u) {
                emitInst(0x91000000u | (lo12 << 10) | (regCode(dst) << 5) | regCode(dst));
            }
            return;
        }
        const A64Reg scratch = (dst == A64Reg::X10 || src == A64Reg::X10) ? A64Reg::X11 : A64Reg::X10;
        movImm64(scratch, static_cast<std::uint64_t>(imm));
        addReg64(dst, src, scratch, 0);
    }

    void subImm64(A64Reg dst, A64Reg src, std::int64_t imm) {
        if (imm == 0) {
            movReg64(dst, src);
            return;
        }
        if (imm < 0) {
            addImm64(dst, src, -imm);
            return;
        }
        if (imm <= 4095) {
            emitInst(0xD1000000u | (static_cast<std::uint32_t>(imm) << 10) | (regCode(src) << 5) | regCode(dst));
            return;
        }
        const A64Reg scratch = (dst == A64Reg::X10 || src == A64Reg::X10) ? A64Reg::X11 : A64Reg::X10;
        movImm64(scratch, static_cast<std::uint64_t>(imm));
        subReg64(dst, src, scratch);
    }

    // `subs xDst, xSrc, #uimm12` -> sets C=0 (A64Cond::LO) on unsigned borrow!
    void subsImm64(A64Reg dst, A64Reg src, std::uint16_t imm12) {
        emitInst(0xF1000000u | ((static_cast<std::uint32_t>(imm12) & 0xFFFu) << 10) |
                 (regCode(src) << 5) | regCode(dst));
    }

    void subsImm32(A64Reg dst, A64Reg src, std::uint16_t imm12) {
        emitInst(0x71000000u | ((static_cast<std::uint32_t>(imm12) & 0xFFFu) << 10) |
                 (regCode(src) << 5) | regCode(dst));
    }

    void addReg64(A64Reg dst, A64Reg lhs, A64Reg rhs, std::uint8_t lslShift = 0) {
        emitInst(0x8B000000u | (regCode(rhs) << 16) |
                 ((static_cast<std::uint32_t>(lslShift) & 63u) << 10) |
                 (regCode(lhs) << 5) | regCode(dst));
    }

    void subReg64(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0xCB000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }

    void addReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x0B000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void addsReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x2B000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void subReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x4B000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void subsReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x6B000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }

    void mulReg64(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x9B007C00u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void smullReg(A64Reg dst64, A64Reg lhs32, A64Reg rhs32) {
        emitInst(0x9B207C00u | (regCode(rhs32) << 16) | (regCode(lhs32) << 5) | regCode(dst64));
    }
    void sdivReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x1AC00C00u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void msubReg32(A64Reg dst, A64Reg mulLhs, A64Reg mulRhs, A64Reg minuend) {
        emitInst(0x1B008000u | (regCode(mulRhs) << 16) | (regCode(minuend) << 10) |
                 (regCode(mulLhs) << 5) | regCode(dst));
    }

    void sxtw(A64Reg dst64, A64Reg src32) {
        emitInst(0x93407C00u | (regCode(src32) << 5) | regCode(dst64));
    }
    void lslImm64(A64Reg dst, A64Reg src, std::uint8_t shift) {
        if (shift == 0) {
            movReg64(dst, src);
            return;
        }
        const std::uint32_t immr = (64u - (shift & 63u)) & 63u;
        const std::uint32_t imms = 63u - (shift & 63u);
        emitInst(0xD3400000u | (immr << 16) | (imms << 10) | (regCode(src) << 5) | regCode(dst));
    }
    void lsrImm32(A64Reg dst, A64Reg src, std::uint8_t shift) {
        if (shift == 0) {
            movReg32(dst, src);
            return;
        }
        emitInst(0x53007C00u | (static_cast<std::uint32_t>(shift & 31u) << 16) |
                 (regCode(src) << 5) | regCode(dst));
    }
    void andLow24Bits32(A64Reg dst, A64Reg src) {
        // ubfx wd, ws, #0, #24 (ubfm wd, ws, #0, #23)
        emitInst(0x53005C00u | (regCode(src) << 5) | regCode(dst));
    }

    void orrReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x2A000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void eorReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x4A000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void eorReg64(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0xCA000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }

    void cmpReg32(A64Reg lhs, A64Reg rhs) {
        subsReg32(A64Reg::XZR, lhs, rhs);
    }
    void cmpReg64(A64Reg lhs, A64Reg rhs) {
        emitInst(0xEB00001Fu | (regCode(rhs) << 16) | (regCode(lhs) << 5));
    }
    void cmpImm32(A64Reg lhs, std::int32_t imm) {
        if (imm >= 0 && imm <= 4095) {
            subsImm32(A64Reg::XZR, lhs, static_cast<std::uint16_t>(imm));
        } else if (imm < 0 && imm >= -4095) {
            // cmn wn, #-imm (adds wzr, wn, #-imm)
            emitInst(0x3100001Fu | (static_cast<std::uint32_t>(-imm) << 10) | (regCode(lhs) << 5));
        } else {
            const A64Reg sc = (lhs == A64Reg::X10) ? A64Reg::X11 : A64Reg::X10;
            movImm32(sc, static_cast<std::uint32_t>(imm));
            cmpReg32(lhs, sc);
        }
    }
    void cmpImm64(A64Reg lhs, std::int64_t imm) {
        if (imm >= 0 && imm <= 4095) {
            subsImm64(A64Reg::XZR, lhs, static_cast<std::uint16_t>(imm));
        } else if (imm < 0 && imm >= -4095) {
            emitInst(0xB100001Fu | (static_cast<std::uint32_t>(-imm) << 10) | (regCode(lhs) << 5));
        } else {
            const A64Reg sc = (lhs == A64Reg::X10) ? A64Reg::X11 : A64Reg::X10;
            movImm64(sc, static_cast<std::uint64_t>(imm));
            cmpReg64(lhs, sc);
        }
    }

    void cset32(A64Reg dst, A64Cond cc) {
        emitInst(0x1A9F07E0u | (static_cast<std::uint32_t>(invertA64Cond(cc)) << 12) | regCode(dst));
    }

    // Memory Loads & Stores (automatic X9 base materialization when offset exceeds uimm12*scale)
    void loadMem8ZeroExt(A64Reg dst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 1);
        emitInst(0x39400000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst));
    }
    void storeMem8Reg(A64Reg base, std::int32_t disp, A64Reg src) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 1);
        emitInst(0x39000000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(src));
    }
    void storeMem8Imm(A64Reg base, std::int32_t disp, std::uint8_t imm) {
        if (imm == 0u) {
            storeMem8Reg(base, disp, A64Reg::XZR);
        } else {
            movz32(A64Reg::X10, imm, 0);
            storeMem8Reg(base, disp, A64Reg::X10);
        }
    }
    void loadMem16ZeroExt(A64Reg dst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 2);
        emitInst(0x79400000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst));
    }
    void loadMem32(A64Reg dst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xB9400000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst));
    }
    void loadMem32SignExt64(A64Reg dst64, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xB9800000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst64));
    }
    void storeMem32(A64Reg base, std::int32_t disp, A64Reg src) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xB9000000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(src));
    }
    void storeMem32Imm(A64Reg base, std::int32_t disp, std::uint32_t imm) {
        if (imm == 0u) {
            storeMem32(base, disp, A64Reg::XZR);
        } else {
            movImm32(A64Reg::X10, imm);
            storeMem32(base, disp, A64Reg::X10);
        }
    }
    void loadMem64(A64Reg dst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 8);
        emitInst(0xF9400000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst));
    }
    void storeMem64(A64Reg base, std::int32_t disp, A64Reg src) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 8);
        emitInst(0xF9000000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(src));
    }
    void storeMem64Imm(A64Reg base, std::int32_t disp, std::uint64_t imm) {
        if (imm == 0u) {
            storeMem64(base, disp, A64Reg::XZR);
        } else {
            movImm64(A64Reg::X10, imm);
            storeMem64(base, disp, A64Reg::X10);
        }
    }

    // NEON 128-bit & FP scalar loads/stores/ops (V0..V7 caller-saved across all ARM64 ABIs)
    void ldrQ(std::uint8_t vDst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 16);
        emitInst(0x3DC00000u | (uimm12 << 10) | (regCode(b) << 5) | (vDst & 31u));
    }
    void strQ(A64Reg base, std::int32_t disp, std::uint8_t vSrc) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 16);
        emitInst(0x3D800000u | (uimm12 << 10) | (regCode(b) << 5) | (vSrc & 31u));
    }
    void veorZero128(std::uint8_t vReg) {
        const std::uint32_t v = vReg & 31u;
        emitInst(0x6E201C00u | (v << 16) | (v << 5) | v);
    }

    void ldrD(std::uint8_t dDst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 8);
        emitInst(0xFD400000u | (uimm12 << 10) | (regCode(b) << 5) | (dDst & 31u));
    }
    void strD(A64Reg base, std::int32_t disp, std::uint8_t dSrc) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 8);
        emitInst(0xFD000000u | (uimm12 << 10) | (regCode(b) << 5) | (dSrc & 31u));
    }
    void ldrS(std::uint8_t sDst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xBD400000u | (uimm12 << 10) | (regCode(b) << 5) | (sDst & 31u));
    }
    void strS(A64Reg base, std::int32_t disp, std::uint8_t sSrc) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xBD000000u | (uimm12 << 10) | (regCode(b) << 5) | (sSrc & 31u));
    }

    void faddD(std::uint8_t dDst, std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E602800u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5) | dDst);
    }
    void fsubD(std::uint8_t dDst, std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E603800u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5) | dDst);
    }
    void fmulD(std::uint8_t dDst, std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E600800u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5) | dDst);
    }
    void fdivD(std::uint8_t dDst, std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E601800u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5) | dDst);
    }
    void fcmpD(std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E602000u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5));
    }

    void faddS(std::uint8_t sDst, std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E202800u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5) | sDst);
    }
    void fsubS(std::uint8_t sDst, std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E203800u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5) | sDst);
    }
    void fmulS(std::uint8_t sDst, std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E200800u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5) | sDst);
    }
    void fdivS(std::uint8_t sDst, std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E201800u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5) | sDst);
    }
    void fcmpS(std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E202000u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5));
    }

    void fmovDFromX(std::uint8_t dDst, A64Reg src64) {
        emitInst(0x9E670000u | (regCode(src64) << 5) | (dDst & 31u));
    }
    void scvtfDFromX(std::uint8_t dDst, A64Reg src64) {
        emitInst(0x9E620000u | (regCode(src64) << 5) | (dDst & 31u));
    }
    void scvtfSFromX(std::uint8_t sDst, A64Reg src64) {
        emitInst(0x9E220000u | (regCode(src64) << 5) | (sDst & 31u));
    }
    void fcvtDFromS(std::uint8_t dDst, std::uint8_t sSrc) {
        emitInst(0x1E22C000u | (static_cast<std::uint32_t>(sSrc) << 5) | dDst);
    }
    void fcvtSFromD(std::uint8_t sDst, std::uint8_t dSrc) {
        emitInst(0x1E624000u | (static_cast<std::uint32_t>(dSrc) << 5) | sDst);
    }
    void fcvtzsXFromD(A64Reg dst64, std::uint8_t dSrc) {
        emitInst(0x9E780000u | (static_cast<std::uint32_t>(dSrc) << 5) | regCode(dst64));
    }

    // Branches
    void b(std::uint32_t labelId) {
        const std::size_t pos = buf_.size();
        emitInst(0x14000000u);
        recordOrPatch(labelId, PatchSite{pos, PatchKind::BImm26});
    }
    void bcc(A64Cond cc, std::uint32_t labelId) {
        const std::size_t pos = buf_.size();
        emitInst(0x54000000u | static_cast<std::uint32_t>(cc));
        recordOrPatch(labelId, PatchSite{pos, PatchKind::BCondImm19});
    }

private:
    [[nodiscard]] static constexpr std::uint32_t regCode(A64Reg r) noexcept {
        return static_cast<std::uint32_t>(r) & 31u;
    }

    [[nodiscard]] std::pair<A64Reg, std::uint32_t> resolveScaledAddr(
        A64Reg base,
        std::int32_t disp,
        std::int32_t scale) {
        if (disp >= 0 && (disp % scale) == 0 && (disp / scale) <= 4095) {
            return {base, static_cast<std::uint32_t>(disp / scale)};
        }
        addImm64(A64Reg::X9, base, disp);
        return {A64Reg::X9, 0u};
    }

    void recordOrPatch(std::uint32_t labelId, PatchSite site) {
        auto& lbl = labels_[labelId];
        if (lbl.bound()) {
            patchSite(site, lbl.boundOffset);
        } else {
            lbl.patches.push_back(site);
        }
    }

    void patchSite(const PatchSite& site, std::size_t targetByteOffset) {
        const std::ptrdiff_t relBytes =
            static_cast<std::ptrdiff_t>(targetByteOffset) -
            static_cast<std::ptrdiff_t>(site.bytePos);
        if ((relBytes & 3) != 0) {
            overflowed_ = true;
            return;
        }
        const std::ptrdiff_t relWords = relBytes >> 2;
        std::uint32_t inst = 0;
        std::memcpy(&inst, buf_.data() + site.bytePos, 4);
        if (site.kind == PatchKind::BImm26) {
            if (relWords < -(1 << 25) || relWords >= (1 << 25)) {
                overflowed_ = true;
                return;
            }
            inst = (inst & 0xFC000000u) | (static_cast<std::uint32_t>(relWords) & 0x03FFFFFFu);
        } else {
            if (relWords < -(1 << 18) || relWords >= (1 << 18)) {
                overflowed_ = true;
                return;
            }
            inst = (inst & 0xFF00001Fu) | ((static_cast<std::uint32_t>(relWords) & 0x7FFFFu) << 5);
        }
        std::memcpy(buf_.data() + site.bytePos, &inst, 4);
    }

    std::vector<std::uint8_t> buf_;
    std::vector<Label> labels_;
    bool overflowed_ = false;
};

} // namespace

[[nodiscard]] std::vector<std::uint8_t> compileFunctionA64(
    const CompiledFunctionRecord& record) {
    const auto& execFunc = *record.execFunc;
    const std::size_t nInst = execFunc.flatStream.size();

    A64Emitter as;
    std::vector<std::uint32_t> instLabels(nInst);
    for (std::size_t i = 0; i < nInst; ++i) {
        instLabels[i] = as.createLabel();
    }

    const std::uint32_t epilogueOk = as.createLabel();
    const std::uint32_t epilogueBudget = as.createLabel();
    const std::uint32_t epilogueOverflow = as.createLabel();
    const std::uint32_t epilogueDivZero = as.createLabel();
    const std::uint32_t epilogueTypeErr = as.createLabel();
    const std::uint32_t epilogueHelperErr = as.createLabel();
    const std::uint32_t sharedPopRet = as.createLabel();

    std::vector<ColdTrapStub> coldStubs;
    coldStubs.reserve(nInst);

    const auto addColdStub = [&](
        ColdTrapStub::Kind kind,
        std::uint32_t flatIdx,
        std::int32_t restoreBudget = 0,
        std::uint32_t resumeLabel = 0,
        std::uint32_t altResumeLabel = 0) {
        const std::uint32_t lbl = as.createLabel();
        coldStubs.push_back(ColdTrapStub{
            kind, lbl, resumeLabel, altResumeLabel, flatIdx, restoreBudget
        });
        return lbl;
    };

    // Unified AAPCS64 / Apple Darwin ARM64 / Windows ARM64 Prologue (16-byte aligned 64B frame)
    as.subSp(64);
    as.stp64Sp(A64Reg::X29, A64Reg::X30, 0);
    as.movFpFromSp();
    as.stp64Sp(A64Reg::X19, A64Reg::X20, 16);
    as.stp64Sp(A64Reg::X21, A64Reg::X22, 32);
    as.stp64Sp(A64Reg::X23, A64Reg::X24, 48);

    // X0 holds JitAbiContext* on macOS/iOS, Android/Linux, and Windows ARM64
    as.movReg64(A64Reg::X19, A64Reg::X0);
    as.loadMem64(A64Reg::X20, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, frameBase)));
    as.loadMem64(A64Reg::X21, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)));
    as.loadMem64(A64Reg::X22, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)));

    if (execFunc.entryStreamOffset != 0) {
        as.b(instLabels[execFunc.entryStreamOffset]);
    }

    const auto slotOff = [](std::uint16_t slot) noexcept -> std::int32_t {
        return static_cast<std::int32_t>(slot) * 16;
    };

    const auto emitBudgetDeduct = [&](std::uint32_t idx, std::uint8_t span, bool mayNeedPartialStep) {
        as.subsImm64(A64Reg::X21, A64Reg::X21, span);
        if (span > 1u && mayNeedPartialStep) {
            const auto stub = addColdStub(
                ColdTrapStub::Kind::BudgetPartialSuper, idx, static_cast<std::int32_t>(span));
            as.bcc(A64Cond::LO, stub);
        } else {
            const auto stub = addColdStub(ColdTrapStub::Kind::BudgetSimple, idx);
            as.bcc(A64Cond::LO, stub);
        }
    };

    const auto emitCheckTagOrTrap = [&](
        std::uint16_t slot,
        std::uint8_t expectedTag,
        std::uint32_t idx,
        std::int32_t restoreBudgetAfterFail) {
        as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(slot) + 15);
        as.cmpImm32(A64Reg::X6, expectedTag);
        const auto stub = addColdStub(ColdTrapStub::Kind::TypeMismatch, idx, restoreBudgetAfterFail);
        as.bcc(A64Cond::NE, stub);
    };

    const auto emitHelperCallInline = [&](
        std::uint32_t (*helperFn)(JitAbiContext*, std::uint32_t),
        std::uint32_t flatIdx) {
        as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
        as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
        as.movReg64(A64Reg::X0, A64Reg::X19);
        as.movImm32(A64Reg::X1, flatIdx);
        as.movImm64(A64Reg::X16, fnPtrAddress(helperFn));
        as.blr(A64Reg::X16);
        as.loadMem64(A64Reg::X20, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, frameBase)));
        as.loadMem64(A64Reg::X21, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)));
        as.loadMem64(A64Reg::X22, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)));
        as.cmpImm32(A64Reg::X0, 0);
        as.bcc(A64Cond::NE, epilogueHelperErr);
    };

    // Copies `slotCount` 16-byte Value slots from pointer in X3 to pointer in X4 using 128-bit Q0
    const auto emitCopySlots128 = [&](std::uint16_t slotCount) {
        if (slotCount <= 8u) {
            for (std::uint16_t k = 0; k < slotCount; ++k) {
                as.ldrQ(0, A64Reg::X3, static_cast<std::int32_t>(k) * 16);
                as.strQ(A64Reg::X4, static_cast<std::int32_t>(k) * 16, 0);
            }
        } else {
            as.movImm32(A64Reg::X5, slotCount);
            const std::uint32_t loopLbl = as.createLabel();
            as.bind(loopLbl);
            as.ldrQ(0, A64Reg::X3, 0);
            as.strQ(A64Reg::X4, 0, 0);
            as.addImm64(A64Reg::X3, A64Reg::X3, 16);
            as.addImm64(A64Reg::X4, A64Reg::X4, 16);
            as.subsImm32(A64Reg::X5, A64Reg::X5, 1u);
            as.bcc(A64Cond::NE, loopLbl);
        }
    };

    // Operates on W0 (lhs) and W1 (rhs); returns kTagInt64 (result in X0) or kTagBool (0/1 in W0)
    const auto emitQuickIntOpCore = [&](
        runtime::detail::QuickOpcode qop,
        bool checked,
        std::uint32_t idx,
        std::int32_t unexecAfterTrap) -> std::uint8_t {
        using QO = runtime::detail::QuickOpcode;
        switch (qop) {
        case QO::AddInt:
            if (checked) {
                as.addsReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.bcc(A64Cond::VS, ovfStub);
            } else {
                as.addReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            }
            as.sxtw(A64Reg::X0, A64Reg::X0);
            return kTagInt64;
        case QO::SubtractInt:
            if (checked) {
                as.subsReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.bcc(A64Cond::VS, ovfStub);
            } else {
                as.subReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            }
            as.sxtw(A64Reg::X0, A64Reg::X0);
            return kTagInt64;
        case QO::MultiplyInt:
            as.smullReg(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            as.sxtw(A64Reg::X2, A64Reg::X0);
            if (checked) {
                as.cmpReg64(A64Reg::X0, A64Reg::X2);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.bcc(A64Cond::NE, ovfStub);
            } else {
                as.movReg64(A64Reg::X0, A64Reg::X2);
            }
            return kTagInt64;
        case QO::DivideInt: {
            as.cmpImm32(A64Reg::X1, 0);
            const auto div0Stub = addColdStub(ColdTrapStub::Kind::DivisionByZero, idx, unexecAfterTrap);
            as.bcc(A64Cond::EQ, div0Stub);
            const std::uint32_t doDiv = as.createLabel();
            as.cmpImm32(A64Reg::X0, std::numeric_limits<std::int32_t>::min());
            as.bcc(A64Cond::NE, doDiv);
            as.cmpImm32(A64Reg::X1, -1);
            const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
            as.bcc(A64Cond::EQ, ovfStub);
            as.bind(doDiv);
            as.sdivReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            as.sxtw(A64Reg::X0, A64Reg::X0);
            return kTagInt64;
        }
        case QO::RemainderInt: {
            as.cmpImm32(A64Reg::X1, 0);
            const auto div0Stub = addColdStub(ColdTrapStub::Kind::DivisionByZero, idx, unexecAfterTrap);
            as.bcc(A64Cond::EQ, div0Stub);
            const std::uint32_t doRem = as.createLabel();
            const std::uint32_t remDone = as.createLabel();
            as.cmpImm32(A64Reg::X0, std::numeric_limits<std::int32_t>::min());
            as.bcc(A64Cond::NE, doRem);
            as.cmpImm32(A64Reg::X1, -1);
            as.bcc(A64Cond::NE, doRem);
            as.movz32(A64Reg::X0, 0);
            as.b(remDone);
            as.bind(doRem);
            as.sdivReg32(A64Reg::X2, A64Reg::X0, A64Reg::X1);
            as.msubReg32(A64Reg::X0, A64Reg::X2, A64Reg::X1, A64Reg::X0);
            as.sxtw(A64Reg::X0, A64Reg::X0);
            as.bind(remDone);
            return kTagInt64;
        }
        default:
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.cset32(A64Reg::X0, quickCmpToA64Cond(qop));
            return kTagBool;
        }
    };

    const auto emitWriteTaggedResult = [&](std::uint16_t slot, std::uint8_t tag) {
        const std::int32_t off = slotOff(slot);
        if (tag == kTagBool) {
            as.storeMem8Reg(A64Reg::X20, off, A64Reg::X0);
            as.storeMem8Imm(A64Reg::X20, off + 15, kTagBool);
        } else {
            as.storeMem64(A64Reg::X20, off, A64Reg::X0);
            as.storeMem8Imm(A64Reg::X20, off + 15, tag);
        }
    };

    const auto emitCondBranchTargets = [&](
        A64Cond trueCc,
        std::uint32_t idx,
        std::uint32_t trueOff,
        std::uint32_t falseOff) {
        if (falseOff == idx + 1u) {
            as.bcc(trueCc, instLabels[trueOff]);
        } else if (trueOff == idx + 1u) {
            as.bcc(invertA64Cond(trueCc), instLabels[falseOff]);
        } else {
            as.bcc(trueCc, instLabels[trueOff]);
            as.b(instLabels[falseOff]);
        }
    };

    for (std::size_t idx = 0; idx < nInst; ++idx) {
        as.bind(instLabels[idx]);
        const auto& fi = execFunc.flatStream[idx];
        const auto i32Idx = static_cast<std::uint32_t>(idx);
        const auto unexecAfter = static_cast<std::int32_t>(record.unexecutedAfterOp[idx]);
        using FO = runtime::detail::FlatOpcode;
        using QO = runtime::detail::QuickOpcode;

        switch (fi.flatOpcode) {
        case FO::MoveSlot:
        case FO::AliasStructSlice:
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.dstSlot != fi.src0Slot) {
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
                as.strQ(A64Reg::X20, slotOff(fi.dstSlot), 0);
            }
            break;

        case FO::StoreLocalStructFieldInPlace: {
            const auto targetLeafSlot = static_cast<std::uint32_t>(fi.auxImm32 + fi.aux0Slot);
            if (fi.aux1Slot == 0u) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::EQ, slowStub);
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src1Slot));
                as.strQ(A64Reg::X20, static_cast<std::int32_t>(targetLeafSlot * 16u), 0);
                as.bind(doneLbl);
            } else if (const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot);
                       subSc != nullptr && targetLeafSlot + subSc->flatSlotCount <= execFunc.totalFrameSlots) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 13);
                as.cmpImm32(A64Reg::X6, 0);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 12);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(fi.aux1Slot & 0xFFu));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 14);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>((fi.aux1Slot >> 8u) & 0xFFu));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src1Slot) + 8);
                as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.movz32(A64Reg::X6, 0x40u);
                as.orrReg32(A64Reg::X1, A64Reg::X1, A64Reg::X6);
                as.lsrImm32(A64Reg::X6, A64Reg::X0, 24u);
                as.cmpReg32(A64Reg::X6, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.andLow24Bits32(A64Reg::X0, A64Reg::X0);
                as.loadMem64(A64Reg::X6, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.addReg64(A64Reg::X3, A64Reg::X6, A64Reg::X0, 4u);
                as.addImm64(A64Reg::X4, A64Reg::X20, static_cast<std::int64_t>(targetLeafSlot) * 16LL);
                const std::uint32_t skipCopyLbl = as.createLabel();
                as.cmpReg64(A64Reg::X3, A64Reg::X4);
                as.bcc(A64Cond::EQ, skipCopyLbl);
                emitCopySlots128(subSc->flatSlotCount);
                as.bind(skipCopyLbl);
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::LoadStructSliceField: {
            const std::uint16_t subSid = fi.aux1Slot;
            if (subSid == 0u || fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 13);
                as.cmpImm32(A64Reg::X6, 0);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.movz32(A64Reg::X6, 0x40u);
                as.orrReg32(A64Reg::X1, A64Reg::X1, A64Reg::X6);
                as.lsrImm32(A64Reg::X6, A64Reg::X0, 24u);
                as.cmpReg32(A64Reg::X6, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.andLow24Bits32(A64Reg::X0, A64Reg::X0);
                as.addImm64(A64Reg::X0, A64Reg::X0, fi.aux0Slot);
                as.cmpImm32(A64Reg::X0, static_cast<std::int32_t>(runtime::StructStorageHandle::FrameOffsetMask));
                as.bcc(A64Cond::HI, slowStub);
                as.loadMem64(A64Reg::X6, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                if (subSid == 0u) {
                    as.addReg64(A64Reg::X3, A64Reg::X6, A64Reg::X0, 4u);
                    as.ldrQ(0, A64Reg::X3, 0);
                    as.strQ(A64Reg::X20, slotOff(fi.dstSlot), 0);
                } else {
                    as.lslImm64(A64Reg::X1, A64Reg::X1, 24u);
                    as.orrReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
                    as.movImm64(A64Reg::X2, static_cast<std::uint64_t>(fi.integerImmediate));
                    as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot) + 0, A64Reg::X2);
                    as.storeMem32(A64Reg::X20, slotOff(fi.dstSlot) + 8, A64Reg::X0);
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(subSid & 0xFFu));
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 13, 0u);
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((subSid >> 8u) & 0xFFu));
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                }
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::CopyStructSlice: {
            const std::uint16_t sid = fi.aux0Slot;
            const std::uint32_t dstPayOff = fi.auxImm32;
            const auto* sc = runtime::getFlatStructSchema(sid);
            if (sc != nullptr && dstPayOff + sc->flatSlotCount <= execFunc.totalFrameSlots) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 13);
                as.cmpImm32(A64Reg::X6, 0);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 12);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(sid & 0xFFu));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 14);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>((sid >> 8u) & 0xFFu));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.movz32(A64Reg::X6, 0x40u);
                as.orrReg32(A64Reg::X1, A64Reg::X1, A64Reg::X6);
                as.lsrImm32(A64Reg::X6, A64Reg::X0, 24u);
                as.cmpReg32(A64Reg::X6, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.andLow24Bits32(A64Reg::X0, A64Reg::X0);
                as.loadMem32(A64Reg::X8, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, frameOffset32)));
                if (dstPayOff != 0u) {
                    as.movImm32(A64Reg::X6, dstPayOff);
                    as.addReg32(A64Reg::X8, A64Reg::X8, A64Reg::X6);
                }
                as.cmpImm32(A64Reg::X8, static_cast<std::int32_t>(runtime::StructStorageHandle::FrameOffsetMask));
                as.bcc(A64Cond::HI, slowStub);
                as.loadMem64(A64Reg::X6, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.movReg64(A64Reg::X7, A64Reg::X1); // save (0x40 | bankId)
                as.addReg64(A64Reg::X3, A64Reg::X6, A64Reg::X0, 4u);
                as.addImm64(A64Reg::X4, A64Reg::X20, static_cast<std::int64_t>(dstPayOff) * 16LL);
                const std::uint32_t skipCopyLbl = as.createLabel();
                as.cmpReg64(A64Reg::X3, A64Reg::X4);
                as.bcc(A64Cond::EQ, skipCopyLbl);
                emitCopySlots128(sc->flatSlotCount);
                as.bind(skipCopyLbl);
                as.lslImm64(A64Reg::X7, A64Reg::X7, 24u);
                as.movReg64(A64Reg::X0, A64Reg::X8);
                as.orrReg32(A64Reg::X0, A64Reg::X0, A64Reg::X7);
                as.movImm64(A64Reg::X2, static_cast<std::uint64_t>(sc->structTypeId));
                as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot) + 0, A64Reg::X2);
                as.storeMem32(A64Reg::X20, slotOff(fi.dstSlot) + 8, A64Reg::X0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 13, 0u);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::WritebackLocalStackRef:
            emitBudgetDeduct(i32Idx, fi.span, false);
            break;

        case FO::StoreStackRefStructFieldInPlace: {
            if (fi.aux1Slot == 0u) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagObjectRef);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                as.cmpImm32(A64Reg::X0, static_cast<std::int32_t>(runtime::ObjectRef::StackSlotRefHeapId));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.loadMem32(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 4);
                as.cmpReg32(A64Reg::X1, A64Reg::X6);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::EQ, slowStub);
                as.loadMem64(A64Reg::X6, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 0);
                as.addImm64(A64Reg::X0, A64Reg::X0, fi.aux0Slot);
                as.addReg64(A64Reg::X3, A64Reg::X6, A64Reg::X0, 4u);
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src1Slot));
                as.strQ(A64Reg::X3, 0, 0);
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::InitStructSlice:
        case FO::StoreStructSliceField:
        case FO::InitStackRefCell:
        case FO::BindLocalStackRef:
        case FO::LoadStackRefCell:
        case FO::CheckNotNullFast:
        case FO::StoreStackRefCell:
        case FO::NewObjectFast:
        case FO::LoadFieldExactFast:
        case FO::LoadFieldFast:
        case FO::StoreFieldExactFast:
        case FO::StoreFieldFast:
        case FO::GenericBytecode:
            emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            break;

        case FO::SuperMoveParamStoreLocal:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.strQ(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.strQ(A64Reg::X20, slotOff(fi.storeSlot), 0);
            break;

        case FO::SuperMoveParamStoreLocalElided:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.strQ(A64Reg::X20, slotOff(fi.storeSlot), 0);
            break;

        case FO::ConstantInt:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem64Imm(
                A64Reg::X20,
                slotOff(fi.dstSlot),
                static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(fi.integerImmediate))));
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagInt64);
            break;

        case FO::ConstantLong:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem64Imm(A64Reg::X20, slotOff(fi.dstSlot), static_cast<std::uint64_t>(fi.integerImmediate));
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagLong);
            break;

        case FO::ConstantBool:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot), fi.integerImmediate != 0 ? 1u : 0u);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagBool);
            break;

        case FO::ConstantDouble:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem64Imm(A64Reg::X20, slotOff(fi.dstSlot), static_cast<std::uint64_t>(fi.integerImmediate));
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::SuperConstFloat: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            std::uint32_t fBits = 0;
            std::memcpy(&fBits, &fi.integerImmediate, 4);
            as.storeMem32Imm(A64Reg::X20, slotOff(fi.dstSlot), fBits);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::SuperConstFloatStoreLocal: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            std::uint32_t fBits = 0;
            std::memcpy(&fBits, &fi.integerImmediate, 4);
            as.storeMem32Imm(A64Reg::X20, slotOff(fi.storeSlot), fBits);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.storeSlot) + 15, kTagFloat);
            break;
        }

        case FO::AddIntChecked:
        case FO::AddIntUnchecked:
        case FO::SubIntChecked:
        case FO::SubIntUnchecked:
        case FO::MulIntChecked:
        case FO::MulIntUnchecked:
        case FO::DivInt:
        case FO::RemInt:
        case FO::LessInt:
        case FO::LessOrEqualInt:
        case FO::GreaterInt:
        case FO::GreaterOrEqualInt:
        case FO::EqualInt:
        case FO::NotEqualInt:
        case FO::AddIntTrustedChecked:
        case FO::AddIntTrustedUnchecked:
        case FO::SubIntTrustedChecked:
        case FO::SubIntTrustedUnchecked:
        case FO::MulIntTrustedChecked:
        case FO::MulIntTrustedUnchecked:
        case FO::LessIntTrusted: {
            const bool isTrusted =
                fi.flatOpcode >= FO::AddIntTrustedChecked &&
                fi.flatOpcode <= FO::LessIntTrusted;
            const bool isChecked =
                fi.flatOpcode == FO::AddIntChecked ||
                fi.flatOpcode == FO::SubIntChecked ||
                fi.flatOpcode == FO::MulIntChecked ||
                fi.flatOpcode == FO::AddIntTrustedChecked ||
                fi.flatOpcode == FO::SubIntTrustedChecked ||
                fi.flatOpcode == FO::MulIntTrustedChecked;
            const bool mayTrap =
                isChecked || fi.flatOpcode == FO::DivInt || fi.flatOpcode == FO::RemInt;

            QO qop = QO::AddInt;
            switch (fi.flatOpcode) {
            case FO::AddIntChecked: case FO::AddIntUnchecked:
            case FO::AddIntTrustedChecked: case FO::AddIntTrustedUnchecked:
                qop = QO::AddInt; break;
            case FO::SubIntChecked: case FO::SubIntUnchecked:
            case FO::SubIntTrustedChecked: case FO::SubIntTrustedUnchecked:
                qop = QO::SubtractInt; break;
            case FO::MulIntChecked: case FO::MulIntUnchecked:
            case FO::MulIntTrustedChecked: case FO::MulIntTrustedUnchecked:
                qop = QO::MultiplyInt; break;
            case FO::DivInt: qop = QO::DivideInt; break;
            case FO::RemInt: qop = QO::RemainderInt; break;
            case FO::LessInt: case FO::LessIntTrusted: qop = QO::LessInt; break;
            case FO::LessOrEqualInt: qop = QO::LessOrEqualInt; break;
            case FO::GreaterInt: qop = QO::GreaterInt; break;
            case FO::GreaterOrEqualInt: qop = QO::GreaterOrEqualInt; break;
            case FO::EqualInt: qop = QO::EqualInt; break;
            case FO::NotEqualInt: qop = QO::NotEqualInt; break;
            default: break;
            }

            emitBudgetDeduct(i32Idx, fi.span, mayTrap && fi.span > 1u);
            if (!isTrusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem32(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            const std::uint8_t outTag = emitQuickIntOpCore(qop, isChecked, i32Idx, unexecAfter);
            emitWriteTaggedResult(fi.dstSlot, outTag);
            break;
        }

        case FO::NegateIntUnchecked:
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.subReg32(A64Reg::X0, A64Reg::XZR, A64Reg::X0);
            as.sxtw(A64Reg::X0, A64Reg::X0);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;

        case FO::LogicalNot:
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagBool, i32Idx, unexecAfter);
            as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.movz32(A64Reg::X1, 1u);
            as.eorReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;

        case FO::SuperBinaryLocalIntConst:
        case FO::SuperBinaryLocalIntConstStore:
        case FO::SuperBinaryLocalIntConstStoreElided:
        case FO::SuperBinaryLocalIntConstStoreJump: {
            const bool isElided = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreElided);
            const bool isJump = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump);
            const bool hasStore = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStore);
            const bool trusted0 = (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;

            emitBudgetDeduct(i32Idx, fi.span, true);
            if (!trusted0) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            if (!isElided && !isJump) {
                as.sxtw(A64Reg::X2, A64Reg::X0);
                as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot), A64Reg::X2);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagInt64);
                as.storeMem64Imm(
                    A64Reg::X20,
                    slotOff(fi.aux0Slot),
                    static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(fi.integerImmediate))));
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux0Slot) + 15, kTagInt64);
            }
            as.movImm32(A64Reg::X1, static_cast<std::uint32_t>(static_cast<std::int32_t>(fi.integerImmediate)));
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, unexecAfter);
            if (isJump) {
                emitWriteTaggedResult(fi.dstSlot, outTag);
                const std::int64_t brInc =
                    ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2 : 1;
                as.addImm64(A64Reg::X22, A64Reg::X22, brInc);
                as.b(instLabels[fi.trueTargetOffset]);
            } else if (isElided) {
                emitWriteTaggedResult(fi.storeSlot, outTag);
            } else {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
                if (hasStore) {
                    emitWriteTaggedResult(fi.storeSlot, outTag);
                }
            }
            break;
        }

        case FO::SuperBinaryLocalIntLocal:
        case FO::SuperBinaryLocalIntLocalStore:
        case FO::SuperBinaryLocalIntLocalStoreElided:
        case FO::SuperBinaryLocalIntLocalStoreJump:
        case FO::SuperBinaryLocalIntReg:
        case FO::SuperBinaryLocalIntRegStore:
        case FO::SuperBinaryLocalIntRegStoreElided:
        case FO::SuperBinaryLocalIntRegStoreJump: {
            const bool isLocalRhs =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocal ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump;
            const bool isElided =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreElided;
            const bool isJump =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreJump;
            const bool hasStore =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStore;
            const bool trustedBoth =
                (fi.arithFlags & (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                                  runtime::detail::FlatInstruction::FlagTrustedSrc1)) ==
                (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                 runtime::detail::FlatInstruction::FlagTrustedSrc1);

            emitBudgetDeduct(i32Idx, fi.span, true);
            if (!trustedBoth) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem32(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            if (!isElided && !isJump) {
                as.sxtw(A64Reg::X2, A64Reg::X0);
                as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot), A64Reg::X2);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagInt64);
                if (isLocalRhs) {
                    as.sxtw(A64Reg::X2, A64Reg::X1);
                    as.storeMem64(A64Reg::X20, slotOff(fi.aux0Slot), A64Reg::X2);
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux0Slot) + 15, kTagInt64);
                }
            }
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, unexecAfter);
            if (isJump) {
                emitWriteTaggedResult(fi.dstSlot, outTag);
                const std::int64_t brInc =
                    ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2 : 1;
                as.addImm64(A64Reg::X22, A64Reg::X22, brInc);
                as.b(instLabels[fi.trueTargetOffset]);
            } else if (isElided) {
                emitWriteTaggedResult(fi.storeSlot, outTag);
            } else {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
                if (hasStore) {
                    emitWriteTaggedResult(fi.storeSlot, outTag);
                }
            }
            break;
        }

        case FO::SuperBinaryLocalIntRegChainStore:
        case FO::SuperBinaryLocalIntRegChainStoreElided: {
            const bool isElided = (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStoreElided);
            emitBudgetDeduct(i32Idx, fi.span, true);
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 2);
            emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, 2);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem32(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            if (!isElided) {
                as.sxtw(A64Reg::X2, A64Reg::X0);
                as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot), A64Reg::X2);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagInt64);
            }
            const std::uint8_t midTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, 2);
            if (!isElided) {
                emitWriteTaggedResult(fi.aux0Slot, midTag);
            }
            emitCheckTagOrTrap(fi.src2Slot, kTagInt64, i32Idx, 1);
            as.loadMem32(A64Reg::X1, A64Reg::X20, slotOff(fi.src2Slot));
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode2, fi.checked2(), i32Idx, 1);
            if (!isElided) {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
            }
            emitWriteTaggedResult(fi.storeSlot, outTag);
            break;
        }

        case FO::SuperLessLocalConstBranch: {
            const bool trusted0 = (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (!trusted0) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 1);
            }
            as.addImm64(A64Reg::X22, A64Reg::X22, 1);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.cmpImm64(A64Reg::X0, static_cast<std::int64_t>(static_cast<std::int32_t>(fi.integerImmediate)));
            emitCondBranchTargets(
                quickCmpToA64Cond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::SuperLessLocalRegBranch: {
            const bool trustedBoth =
                (fi.arithFlags & (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                                  runtime::detail::FlatInstruction::FlagTrustedSrc1)) ==
                (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                 runtime::detail::FlatInstruction::FlagTrustedSrc1);
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (!trustedBoth) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 1);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, 1);
            }
            as.addImm64(A64Reg::X22, A64Reg::X22, 1);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            emitCondBranchTargets(
                quickCmpToA64Cond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::SuperLessLocalArrayLengthBranch: {
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src1Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLengthBranchSlow,
                i32Idx,
                0,
                instLabels[fi.trueTargetOffset],
                instLabels[fi.falseTargetOffset]);

            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagInt64);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagObjectRef);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src1Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src1Slot) + 8);
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.addImm64(A64Reg::X22, A64Reg::X22, 1);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 24);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            emitCondBranchTargets(
                quickCmpToA64Cond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::ArrayLengthFast: {
            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLengthSlow, i32Idx, 0, doneLbl);

            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagObjectRef);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem32(A64Reg::X0, A64Reg::X19, cacheDisp + 24);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            as.bind(doneLbl);
            break;
        }

        case FO::NewArrayFast:
            emitHelperCallInline(&jitNewArrayHelper, i32Idx);
            break;

        case FO::LoadElementIntFast:
        case FO::LoadElementTypedFast: {
            const bool isTyped = (fi.flatOpcode == FO::LoadElementTypedFast);
            const auto elemType = isTyped
                ? static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1))
                : semantic::PrimitiveType::Int;
            const auto elemTid = isTyped
                ? static_cast<semantic::SymbolId>(fi.integerImmediate)
                : semantic::SymbolId{0};

            std::uint8_t fastEncoding = 0u;
            if (elemTid == 0) {
                if (elemType == semantic::PrimitiveType::Int) fastEncoding = kPackedInt32;
                else if (elemType == semantic::PrimitiveType::Double) fastEncoding = kPackedFloat64;
                else if (elemType == semantic::PrimitiveType::Bool) fastEncoding = kPackedBool;
            }

            if (fastEncoding == 0u) {
                if (isTyped &&
                    elemType == semantic::PrimitiveType::Struct &&
                    execFunc.hasStructPayloads &&
                    fi.dstSlot < execFunc.slotStructSchemaIds.size() &&
                    execFunc.slotStructSchemaIds[fi.dstSlot] != 0u) {
                    const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
                    const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
                    const auto* sc = runtime::getFlatStructSchema(sid);
                    if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg &&
                        static_cast<std::uint32_t>(payOff) + sc->flatSlotCount <= execFunc.totalFrameSlots) {
                        const std::uint32_t doneLbl = as.createLabel();
                        const std::int32_t cacheDisp =
                            static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                            static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
                        const auto slowStub = addColdStub(
                            ColdTrapStub::Kind::ArrayLoadSlow, i32Idx, 0, doneLbl);

                        as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                        as.cmpImm32(A64Reg::X6, kTagObjectRef);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
                        as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
                        as.cmpReg64(A64Reg::X0, A64Reg::X1);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                        as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
                        as.cmpReg32(A64Reg::X0, A64Reg::X1);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 13);
                        as.cmpImm32(A64Reg::X6, 2);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem16ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 30);
                        as.cmpImm32(A64Reg::X6, sid);
                        as.bcc(A64Cond::NE, slowStub);

                        as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                        as.cmpImm32(A64Reg::X6, kTagInt64);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
                        as.loadMem32(A64Reg::X2, A64Reg::X19, cacheDisp + 24);
                        as.cmpReg64(A64Reg::X1, A64Reg::X2);
                        as.bcc(A64Cond::HS, slowStub);

                        emitBudgetDeduct(i32Idx, fi.span, false);
                        as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                        as.movImm64(A64Reg::X6, static_cast<std::uint64_t>(sc->flatSlotCount) * 16ULL);
                        as.mulReg64(A64Reg::X0, A64Reg::X1, A64Reg::X6);
                        as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X0, 0);
                        as.addImm64(A64Reg::X4, A64Reg::X20, static_cast<std::int64_t>(payOff) * 16LL);
                        emitCopySlots128(sc->flatSlotCount);

                        as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                        as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                        as.movz32(A64Reg::X6, 0x40u);
                        as.orrReg32(A64Reg::X1, A64Reg::X1, A64Reg::X6);
                        as.lslImm64(A64Reg::X1, A64Reg::X1, 24u);
                        as.loadMem32(A64Reg::X0, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, frameOffset32)));
                        if (payOff != 0u) {
                            as.movImm32(A64Reg::X6, static_cast<std::uint32_t>(payOff));
                            as.addReg32(A64Reg::X0, A64Reg::X0, A64Reg::X6);
                        }
                        as.orrReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
                        as.movImm64(A64Reg::X2, static_cast<std::uint64_t>(elemTid));
                        as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot) + 0, A64Reg::X2);
                        as.storeMem32(A64Reg::X20, slotOff(fi.dstSlot) + 8, A64Reg::X0);
                        as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                        as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 13, 0u);
                        as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                        as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                        as.bind(doneLbl);
                        break;
                    }
                }
                emitHelperCallInline(&jitLoadElementHelper, i32Idx);
                break;
            }

            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLoadSlow, i32Idx, 0, doneLbl);

            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagObjectRef);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 14);
            as.cmpImm32(A64Reg::X6, fastEncoding);
            as.bcc(A64Cond::NE, slowStub);

            const bool noBce = (fi.arithFlags & runtime::detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u;
            const bool trustedIdx = !isTyped &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            if (!noBce && !trustedIdx) {
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagInt64);
                as.bcc(A64Cond::NE, slowStub);
            }
            as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            if (!noBce) {
                as.loadMem32(A64Reg::X2, A64Reg::X19, cacheDisp + 24);
                as.cmpReg64(A64Reg::X1, A64Reg::X2);
                as.bcc(A64Cond::HS, slowStub);
            }

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
            if (fastEncoding == kPackedInt32) {
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 2u);
                as.loadMem32SignExt64(A64Reg::X0, A64Reg::X3, 0);
                emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            } else if (fastEncoding == kPackedFloat64) {
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 3u);
                as.ldrD(0, A64Reg::X3, 0);
                as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            } else {
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 0u);
                as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X3, 0);
                emitWriteTaggedResult(fi.dstSlot, kTagBool);
            }
            as.bind(doneLbl);
            break;
        }

        case FO::StoreElementStructFieldInPlace: {
            const auto rootSid = static_cast<std::uint16_t>(fi.auxImm32);
            const auto* rootSc = runtime::getFlatStructSchema(rootSid);
            if (fi.aux1Slot == 0u && rootSc != nullptr && fi.aux0Slot < rootSc->flatSlotCount) {
                const std::uint32_t doneLbl = as.createLabel();
                const std::int32_t cacheDisp =
                    static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                    static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
                const auto slowStub = addColdStub(
                    ColdTrapStub::Kind::ArrayStoreSlow, i32Idx, 0, doneLbl);

                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagObjectRef);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
                as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
                as.cmpReg64(A64Reg::X0, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
                as.cmpReg32(A64Reg::X0, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 13);
                as.cmpImm32(A64Reg::X6, 2);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem16ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 30);
                as.cmpImm32(A64Reg::X6, rootSid);
                as.bcc(A64Cond::NE, slowStub);

                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagInt64);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
                as.loadMem32(A64Reg::X2, A64Reg::X19, cacheDisp + 24);
                as.cmpReg64(A64Reg::X1, A64Reg::X2);
                as.bcc(A64Cond::HS, slowStub);

                as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(fi.src2Slot) + 15);
                as.cmpImm32(A64Reg::X0, 0);
                as.bcc(A64Cond::EQ, slowStub);
                as.cmpImm32(A64Reg::X0, static_cast<std::int32_t>(runtime::ValueTag::FirstTraceable));
                as.bcc(A64Cond::HS, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                as.movImm64(A64Reg::X6, rootSc->flatSlotCount);
                as.mulReg64(A64Reg::X0, A64Reg::X1, A64Reg::X6);
                as.addImm64(A64Reg::X0, A64Reg::X0, fi.aux0Slot);
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X0, 4u);
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src2Slot));
                as.strQ(A64Reg::X3, 0, 0);
                as.bind(doneLbl);
                break;
            }
            emitHelperCallInline(&jitStoreElementHelper, i32Idx);
            break;
        }

        case FO::StoreElementStructFast:
        case FO::StoreElementObjFast:
            emitHelperCallInline(&jitStoreElementHelper, i32Idx);
            break;

        case FO::StoreElementIntFast:
        case FO::StoreElementPrimFast: {
            const bool isPrim = (fi.flatOpcode == FO::StoreElementPrimFast);
            const auto elemType = isPrim
                ? static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1))
                : semantic::PrimitiveType::Int;

            std::uint8_t fastEncoding = 0u;
            if (elemType == semantic::PrimitiveType::Int) fastEncoding = kPackedInt32;
            else if (elemType == semantic::PrimitiveType::Double) fastEncoding = kPackedFloat64;
            else if (elemType == semantic::PrimitiveType::Bool) fastEncoding = kPackedBool;

            if (fastEncoding == 0u) {
                emitHelperCallInline(&jitStoreElementHelper, i32Idx);
                break;
            }

            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayStoreSlow, i32Idx, 0, doneLbl);

            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagObjectRef);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 14);
            as.cmpImm32(A64Reg::X6, fastEncoding);
            as.bcc(A64Cond::NE, slowStub);

            const bool noBce = (fi.arithFlags & runtime::detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u;
            const bool trustedIdx = !isPrim &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            const bool trustedVal = !isPrim &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc1) != 0u;
            if (!noBce && !trustedIdx) {
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagInt64);
                as.bcc(A64Cond::NE, slowStub);
            }
            as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            if (!noBce) {
                as.loadMem32(A64Reg::X2, A64Reg::X19, cacheDisp + 24);
                as.cmpReg64(A64Reg::X1, A64Reg::X2);
                as.bcc(A64Cond::HS, slowStub);
            }

            if (fastEncoding == kPackedInt32) {
                if (!trustedVal) {
                    as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src2Slot) + 15);
                    as.cmpImm32(A64Reg::X6, kTagInt64);
                    as.bcc(A64Cond::NE, slowStub);
                }
                as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src2Slot));
                as.sxtw(A64Reg::X6, A64Reg::X0);
                as.cmpReg64(A64Reg::X0, A64Reg::X6);
                as.bcc(A64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 2u);
                as.storeMem32(A64Reg::X3, 0, A64Reg::X0);
            } else if (fastEncoding == kPackedFloat64) {
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src2Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagDouble);
                as.bcc(A64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.ldrD(0, A64Reg::X20, slotOff(fi.src2Slot));
                as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 3u);
                as.strD(A64Reg::X3, 0, 0);
            } else {
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src2Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagBool);
                as.bcc(A64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(fi.src2Slot));
                as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 0u);
                as.storeMem8Reg(A64Reg::X3, 0, A64Reg::X0);
            }
            as.bind(doneLbl);
            break;
        }

        // Scalar Double ALU & Comparisons
        case FO::AddDouble:
        case FO::SubDouble:
        case FO::MulDouble:
        case FO::DivDouble:
        case FO::AddDoubleTrusted:
        case FO::SubDoubleTrusted:
        case FO::MulDoubleTrusted:
        case FO::DivDoubleTrusted: {
            const bool trusted =
                fi.flatOpcode >= FO::AddDoubleTrusted && fi.flatOpcode <= FO::DivDoubleTrusted;
            if (!trusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, unexecAfter);
            }
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrD(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrD(1, A64Reg::X20, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::SubDouble || fi.flatOpcode == FO::SubDoubleTrusted) as.fsubD(0, 0, 1);
            else if (fi.flatOpcode == FO::MulDouble || fi.flatOpcode == FO::MulDoubleTrusted) as.fmulD(0, 0, 1);
            else if (fi.flatOpcode == FO::DivDouble || fi.flatOpcode == FO::DivDoubleTrusted) as.fdivD(0, 0, 1);
            else as.faddD(0, 0, 1);
            as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;
        }

        case FO::LessDouble:
        case FO::LessOrEqualDouble:
        case FO::GreaterDouble:
        case FO::GreaterOrEqualDouble: {
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrD(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrD(1, A64Reg::X20, slotOff(fi.src1Slot));
            as.fcmpD(0, 1);
            // ARM64 FCMP NZCV: NaN sets N=0,Z=0,C=1,V=1 -> MI(<), LS(<=), GT(>), GE(>=) are all 0 on NaN!
            if (fi.flatOpcode == FO::LessDouble) as.cset32(A64Reg::X0, A64Cond::MI);
            else if (fi.flatOpcode == FO::LessOrEqualDouble) as.cset32(A64Reg::X0, A64Cond::LS);
            else if (fi.flatOpcode == FO::GreaterDouble) as.cset32(A64Reg::X0, A64Cond::GT);
            else as.cset32(A64Reg::X0, A64Cond::GE);
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;
        }

        case FO::NegateDouble:
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.movz64(A64Reg::X1, 0x8000u, 3);
            as.eorReg64(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            emitWriteTaggedResult(fi.dstSlot, kTagDouble);
            break;

        case FO::SuperBinaryLocalDoubleConstStore:
        case FO::SuperBinaryLocalDoubleConstStoreElided:
        case FO::SuperBinaryLocalDoubleLocalStore:
        case FO::SuperBinaryLocalDoubleLocalStoreElided: {
            const bool isConst =
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStore ||
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStoreElided;
            const bool isElided =
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalDoubleLocalStoreElided;
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, 1);
            as.ldrD(0, A64Reg::X20, slotOff(fi.src0Slot));
            if (isConst) {
                as.movImm64(A64Reg::X0, static_cast<std::uint64_t>(fi.integerImmediate));
                as.fmovDFromX(1, A64Reg::X0);
            } else {
                emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, 1);
                as.ldrD(1, A64Reg::X20, slotOff(fi.src1Slot));
            }
            if (!isElided) {
                as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
                as.strD(A64Reg::X20, slotOff(fi.aux0Slot), 1);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux0Slot) + 15, kTagDouble);
            }
            if (fi.binaryOpcode1 == QO::SubtractDouble) as.fsubD(0, 0, 1);
            else if (fi.binaryOpcode1 == QO::MultiplyDouble) as.fmulD(0, 0, 1);
            else if (fi.binaryOpcode1 == QO::DivideDouble) as.fdivD(0, 0, 1);
            else as.faddD(0, 0, 1);
            if (!isElided) {
                as.strD(A64Reg::X20, slotOff(fi.aux1Slot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux1Slot) + 15, kTagDouble);
            }
            as.strD(A64Reg::X20, slotOff(fi.storeSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.storeSlot) + 15, kTagDouble);
            break;
        }

        // Scalar Float ALU & Superinstructions
        case FO::AddFloat:
        case FO::SubFloat:
        case FO::MulFloat:
        case FO::DivFloat:
        case FO::AddFloatTrusted:
        case FO::SubFloatTrusted:
        case FO::MulFloatTrusted: {
            const bool trusted =
                fi.flatOpcode >= FO::AddFloatTrusted && fi.flatOpcode <= FO::MulFloatTrusted;
            if (!trusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, unexecAfter);
            }
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrS(1, A64Reg::X20, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::SubFloat || fi.flatOpcode == FO::SubFloatTrusted) as.fsubS(0, 0, 1);
            else if (fi.flatOpcode == FO::MulFloat || fi.flatOpcode == FO::MulFloatTrusted) as.fmulS(0, 0, 1);
            else if (fi.flatOpcode == FO::DivFloat) as.fdivS(0, 0, 1);
            else as.faddS(0, 0, 1);
            as.strS(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::LessFloat:
        case FO::LessOrEqualFloat:
        case FO::GreaterFloat:
        case FO::GreaterOrEqualFloat:
        case FO::EqualFloat:
        case FO::NotEqualFloat: {
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrS(1, A64Reg::X20, slotOff(fi.src1Slot));
            as.fcmpS(0, 1);
            if (fi.flatOpcode == FO::LessFloat) as.cset32(A64Reg::X0, A64Cond::MI);
            else if (fi.flatOpcode == FO::LessOrEqualFloat) as.cset32(A64Reg::X0, A64Cond::LS);
            else if (fi.flatOpcode == FO::GreaterFloat) as.cset32(A64Reg::X0, A64Cond::GT);
            else if (fi.flatOpcode == FO::GreaterOrEqualFloat) as.cset32(A64Reg::X0, A64Cond::GE);
            else if (fi.flatOpcode == FO::EqualFloat) as.cset32(A64Reg::X0, A64Cond::EQ);
            else as.cset32(A64Reg::X0, A64Cond::NE);
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;
        }

        case FO::NegateFloat:
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.movz32(A64Reg::X1, 0x8000u, 1);
            as.eorReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            as.storeMem32(A64Reg::X20, slotOff(fi.dstSlot), A64Reg::X0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;

        case FO::SuperBinaryLocalFloatLocalStore:
        case FO::SuperBinaryLocalFloatLocalStoreElided:
        case FO::SuperBinaryLocalFloatReg:
        case FO::SuperBinaryLocalFloatRegElided:
        case FO::SuperBinaryLocalFloatRegStore:
        case FO::SuperBinaryLocalFloatRegStoreElided:
        case FO::SuperBinaryRegFloatStoreElided: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, 0);
            emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, 0);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrS(1, A64Reg::X20, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
                as.strS(A64Reg::X20, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
                if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore) {
                    as.strS(A64Reg::X20, slotOff(fi.aux0Slot), 1);
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux0Slot) + 15, kTagFloat);
                }
            }
            if (fi.binaryOpcode1 == QO::SubtractFloat) as.fsubS(0, 0, 1);
            else if (fi.binaryOpcode1 == QO::MultiplyFloat) as.fmulS(0, 0, 1);
            else if (fi.binaryOpcode1 == QO::DivideFloat) as.fdivS(0, 0, 1);
            else as.faddS(0, 0, 1);
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegElided ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
                as.strS(A64Reg::X20, slotOff(fi.aux1Slot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux1Slot) + 15, kTagFloat);
            }
            if (fi.flatOpcode != FO::SuperBinaryLocalFloatReg &&
                fi.flatOpcode != FO::SuperBinaryLocalFloatRegElided) {
                as.strS(A64Reg::X20, slotOff(fi.storeSlot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.storeSlot) + 15, kTagFloat);
            }
            break;
        }

        // Fast Scalar Conversions
        case FO::ConvertIntToLongFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            emitWriteTaggedResult(fi.dstSlot, kTagLong);
            break;

        case FO::ConvertIntToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.scvtfDFromX(0, A64Reg::X0);
            as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertLongToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagLong, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.scvtfDFromX(0, A64Reg::X0);
            as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertIntToFloatFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.scvtfSFromX(0, A64Reg::X0);
            as.strS(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;

        case FO::ConvertFloatToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.fcvtDFromS(0, 0);
            as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertDoubleToFloatFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.checked1() && fi.span > 1u);
            as.ldrD(0, A64Reg::X20, slotOff(fi.src0Slot));
            if (fi.checked1()) {
                constexpr double kMaxF = static_cast<double>(std::numeric_limits<float>::max());
                constexpr double kMinF = -kMaxF;
                std::uint64_t maxBits = 0, minBits = 0;
                std::memcpy(&maxBits, &kMaxF, 8);
                std::memcpy(&minBits, &kMinF, 8);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
                as.movImm64(A64Reg::X0, minBits);
                as.fmovDFromX(1, A64Reg::X0);
                as.fcmpD(0, 1);
                // Trap if unordered (VS) or d0 < minF (MI) -> !(d0 >= minF) == invert(GE) == LT!
                as.bcc(A64Cond::LT, ovfStub);
                as.movImm64(A64Reg::X0, maxBits);
                as.fmovDFromX(1, A64Reg::X0);
                as.fcmpD(0, 1);
                // Trap if d0 > maxF (GT)
                as.bcc(A64Cond::GT, ovfStub);
            }
            as.fcvtSFromD(0, 0);
            as.strS(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::ConvertFloatToIntFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.span > 1u);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.fcvtDFromS(0, 0);
            constexpr double kMinI = static_cast<double>(std::numeric_limits<std::int32_t>::min());
            constexpr double kMaxI = static_cast<double>(std::numeric_limits<std::int32_t>::max());
            std::uint64_t minBits = 0, maxBits = 0;
            std::memcpy(&minBits, &kMinI, 8);
            std::memcpy(&maxBits, &kMaxI, 8);
            const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
            as.movImm64(A64Reg::X0, minBits);
            as.fmovDFromX(1, A64Reg::X0);
            as.fcmpD(0, 1);
            as.bcc(A64Cond::LT, ovfStub);
            as.movImm64(A64Reg::X0, maxBits);
            as.fmovDFromX(1, A64Reg::X0);
            as.fcmpD(0, 1);
            as.bcc(A64Cond::GT, ovfStub);
            as.fcvtzsXFromD(A64Reg::X0, 0);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;
        }

        case FO::ConvertLongToIntFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagLong, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.checked1() && fi.span > 1u);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.sxtw(A64Reg::X1, A64Reg::X0);
            if (fi.checked1()) {
                as.cmpReg64(A64Reg::X0, A64Reg::X1);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
                as.bcc(A64Cond::NE, ovfStub);
            }
            as.movReg64(A64Reg::X0, A64Reg::X1);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;
        }

        case FO::InlineCallEnter: {
            emitHelperCallInline(&jitInlineCallEnterHelper, i32Idx);
            const std::size_t fastTarget = idx + 1u + fi.inlineArgCount;
            if (fi.inlineArgCount > 0u && fastTarget < nInst) {
                as.loadMem32(
                    A64Reg::X0,
                    A64Reg::X19,
                    static_cast<std::int32_t>(offsetof(JitAbiContext, nextFlatOffsetOut)));
                as.cmpImm32(A64Reg::X0, 0);
                as.bcc(A64Cond::NE, instLabels[fastTarget]);
            }
            break;
        }

        case FO::InlineReturnVoid:
        case FO::InlineReturnVoidJump: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.integerImmediate != 0) {
                const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                const auto clearBase = static_cast<std::uint16_t>(packed & 0xFFFFu);
                const auto clearCount = static_cast<std::uint32_t>(packed >> 16u);
                as.veorZero128(0);
                for (std::uint32_t k = 0; k < clearCount; ++k) {
                    as.strQ(A64Reg::X20, slotOff(static_cast<std::uint16_t>(clearBase + k)), 0);
                }
            }
            if (fi.flatOpcode == FO::InlineReturnVoidJump) {
                as.b(instLabels[fi.trueTargetOffset]);
            }
            break;
        }

        case FO::InlineReturnScalar:
        case FO::InlineReturnScalarJump: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
            }
            if (fi.integerImmediate != 0) {
                const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                const auto clearBase = static_cast<std::uint16_t>(packed & 0xFFFFu);
                const auto clearCount = static_cast<std::uint32_t>(packed >> 16u);
                as.veorZero128(1);
                for (std::uint32_t k = 0; k < clearCount; ++k) {
                    as.strQ(A64Reg::X20, slotOff(static_cast<std::uint16_t>(clearBase + k)), 1);
                }
            }
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                as.strQ(A64Reg::X20, slotOff(fi.dstSlot), 0);
            }
            if (fi.flatOpcode == FO::InlineReturnScalarJump) {
                as.b(instLabels[fi.trueTargetOffset]);
            }
            break;
        }

        case FO::TermJumpNoArgs:
            emitBudgetDeduct(i32Idx, fi.span, fi.span > 1u);
            as.addImm64(A64Reg::X22, A64Reg::X22, fi.span);
            if (fi.trueTargetOffset != idx + 1u) {
                as.b(instLabels[fi.trueTargetOffset]);
            }
            break;

        case FO::TermBranchBoolNoArgs:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.addImm64(A64Reg::X22, A64Reg::X22, 1);
            as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.cmpImm32(A64Reg::X0, 0);
            emitCondBranchTargets(A64Cond::NE, i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;

        case FO::TermBranchGeneric: {
            const auto& cold = execFunc.flatCold[idx];
            if (cold.blockIndex >= execFunc.blocks.size()) return {};
            const auto& blk = execFunc.blocks[cold.blockIndex];
            const auto& term = blk.bytecode->terminator;

            const auto emitEdgeCopies = [&](const runtime::detail::ExecutionEdge& edge) -> bool {
                if (!edge.hasArguments) return true;
                if (!edge.sources || !edge.destinations ||
                    edge.sources->size() != edge.destinations->size() ||
                    edge.sources->size() > execFunc.edgeScratchCount) {
                    return false;
                }
                const auto& srcs = *edge.sources;
                const auto& dsts = *edge.destinations;
                if (srcs.size() == 1u) {
                    const auto sSlot = static_cast<std::uint16_t>(execFunc.registerOffset + srcs[0]);
                    const auto dSlot = static_cast<std::uint16_t>(execFunc.registerOffset + dsts[0].target);
                    if (sSlot != dSlot) {
                        as.ldrQ(0, A64Reg::X20, slotOff(sSlot));
                        as.strQ(A64Reg::X20, slotOff(dSlot), 0);
                    }
                    return true;
                }
                for (std::size_t k = 0; k < srcs.size(); ++k) {
                    const auto sSlot = static_cast<std::uint16_t>(execFunc.registerOffset + srcs[k]);
                    const auto scSlot = static_cast<std::uint16_t>(execFunc.scratchOffset + k);
                    as.ldrQ(0, A64Reg::X20, slotOff(sSlot));
                    as.strQ(A64Reg::X20, slotOff(scSlot), 0);
                }
                for (std::size_t k = 0; k < srcs.size(); ++k) {
                    const auto scSlot = static_cast<std::uint16_t>(execFunc.scratchOffset + k);
                    const auto dSlot = static_cast<std::uint16_t>(execFunc.registerOffset + dsts[k].target);
                    as.ldrQ(0, A64Reg::X20, slotOff(scSlot));
                    as.strQ(A64Reg::X20, slotOff(dSlot), 0);
                }
                return true;
            };

            emitBudgetDeduct(i32Idx, fi.span, false);
            if (term.kind == bytecode::TerminatorKind::Jump) {
                as.addImm64(A64Reg::X22, A64Reg::X22, 1);
                if (!emitEdgeCopies(blk.branch)) return {};
                if (fi.trueTargetOffset != idx + 1u) {
                    as.b(instLabels[fi.trueTargetOffset]);
                }
            } else if (term.kind == bytecode::TerminatorKind::Branch) {
                const auto condSlot = static_cast<std::uint16_t>(execFunc.registerOffset + term.condition);
                if (condSlot >= execFunc.totalFrameSlots || fi.falseTargetOffset >= nInst) {
                    return {};
                }
                emitCheckTagOrTrap(condSlot, kTagBool, i32Idx, 0);
                as.addImm64(A64Reg::X22, A64Reg::X22, 1);
                const std::uint32_t falsePathLbl = as.createLabel();
                as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(condSlot));
                as.cmpImm32(A64Reg::X0, 0);
                as.bcc(A64Cond::EQ, falsePathLbl);
                if (!emitEdgeCopies(blk.branch)) return {};
                as.b(instLabels[fi.trueTargetOffset]);
                as.bind(falsePathLbl);
                if (!emitEdgeCopies(blk.falseBranch)) return {};
                if (fi.falseTargetOffset != idx + 1u) {
                    as.b(instLabels[fi.falseTargetOffset]);
                }
            } else {
                return {};
            }
            break;
        }

        case FO::TermReturnVoid:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.veorZero128(0);
            as.strQ(A64Reg::X0, 0, 0);
            as.b(epilogueOk);
            break;

        case FO::TermReturnScalarFast:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X0, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.strQ(A64Reg::X0, 0, 0);
            as.b(epilogueOk);
            break;

        case FO::TermReturnGeneric: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            const auto& cold = execFunc.flatCold[idx];
            const auto retReg = execFunc.blocks[cold.blockIndex].bytecode->terminator.value;
            const auto retSlot = static_cast<std::uint16_t>(execFunc.registerOffset + retReg);
            as.ldrQ(0, A64Reg::X20, slotOff(retSlot));
            as.loadMem64(A64Reg::X0, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.strQ(A64Reg::X0, 0, 0);
            as.b(epilogueOk);
            break;
        }

        default:
            return {};
        }
    }

    // Out-of-line Cold Stubs
    for (const auto& stub : coldStubs) {
        as.bind(stub.stubLabel);
        switch (stub.kind) {
        case ColdTrapStub::Kind::BudgetSimple:
            as.storeMem32Imm(
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.b(epilogueBudget);
            break;

        case ColdTrapStub::Kind::BudgetPartialSuper:
            as.addImm64(A64Reg::X21, A64Reg::X21, stub.restoreBudget);
            emitHelperCallInline(&jitPartialSuperBudgetHelper, stub.flatIdx);
            as.b(epilogueBudget);
            break;

        case ColdTrapStub::Kind::IntegerOverflow:
            if (stub.restoreBudget != 0) as.addImm64(A64Reg::X21, A64Reg::X21, stub.restoreBudget);
            as.storeMem32Imm(
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.b(epilogueOverflow);
            break;

        case ColdTrapStub::Kind::DivisionByZero:
            if (stub.restoreBudget != 0) as.addImm64(A64Reg::X21, A64Reg::X21, stub.restoreBudget);
            as.storeMem32Imm(
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.b(epilogueDivZero);
            break;

        case ColdTrapStub::Kind::TypeMismatch:
            if (stub.restoreBudget != 0) as.addImm64(A64Reg::X21, A64Reg::X21, stub.restoreBudget);
            as.storeMem32Imm(
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.b(epilogueTypeErr);
            break;

        case ColdTrapStub::Kind::ArrayLengthSlow:
            emitHelperCallInline(&jitArrayLengthHelper, stub.flatIdx);
            as.b(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayLengthBranchSlow:
            emitHelperCallInline(&jitArrayLengthHelper, stub.flatIdx);
            as.loadMem32(
                A64Reg::X0,
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, branchCondOut)));
            as.cmpImm32(A64Reg::X0, 0);
            as.bcc(A64Cond::NE, stub.resumeLabel);
            as.b(stub.altResumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayLoadSlow:
            emitHelperCallInline(&jitLoadElementHelper, stub.flatIdx);
            as.b(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayStoreSlow:
            emitHelperCallInline(&jitStoreElementHelper, stub.flatIdx);
            as.b(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::FlatStructSlow:
            emitHelperCallInline(&jitFlatStructAndStackRefHelper, stub.flatIdx);
            as.b(stub.resumeLabel);
            break;
        }
    }

    // Shared Exit Epilogues
    as.bind(epilogueOk);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movz32(A64Reg::X0, 0);
    as.b(sharedPopRet);

    as.bind(epilogueBudget);
    as.movz32(A64Reg::X21, 0);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movImm32(A64Reg::X0, static_cast<std::uint32_t>(runtime::ErrorCode::InstructionBudgetExceeded));
    as.storeMem32(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), A64Reg::X0);
    as.b(sharedPopRet);

    as.bind(epilogueOverflow);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movImm32(A64Reg::X0, static_cast<std::uint32_t>(runtime::ErrorCode::IntegerOverflow));
    as.storeMem32(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), A64Reg::X0);
    as.b(sharedPopRet);

    as.bind(epilogueDivZero);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movImm32(A64Reg::X0, static_cast<std::uint32_t>(runtime::ErrorCode::DivisionByZero));
    as.storeMem32(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), A64Reg::X0);
    as.b(sharedPopRet);

    as.bind(epilogueTypeErr);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movImm32(A64Reg::X0, static_cast<std::uint32_t>(runtime::ErrorCode::TypeMismatch));
    as.storeMem32(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), A64Reg::X0);
    as.b(sharedPopRet);

    as.bind(epilogueHelperErr);

    as.bind(sharedPopRet);
    as.ldp64Sp(A64Reg::X23, A64Reg::X24, 48);
    as.ldp64Sp(A64Reg::X21, A64Reg::X22, 32);
    as.ldp64Sp(A64Reg::X19, A64Reg::X20, 16);
    as.ldp64Sp(A64Reg::X29, A64Reg::X30, 0);
    as.addSp(64);
    as.ret();

    if (!as.ok()) return {};
    return as.bytes();
}

// Exact ARMv8-A A64 user-mode ISA simulator for cross-host verification of emitted ARM64 stencils.
// Executes real 32-bit AArch64 instructions against live JitAbiContext + real C++ BLR helpers,
// while strictly enforcing SP 16-byte alignment, X19..X29 callee-save preservation, and X18 immutability!
[[nodiscard]] std::uint32_t executeSimulatedArm64Code(
    const std::vector<std::uint8_t>& code,
    JitAbiContext* abi,
    std::string& outSimDiagnostic) noexcept {
    if (code.empty() || (code.size() % 4u) != 0u) {
        outSimDiagnostic = "ARM64 code buffer is empty or not 4-byte aligned";
        return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
    }

    struct alignas(16) Vec128 {
        std::uint8_t b[16]{};
    };

    alignas(16) std::uint8_t simStack[4096]{};
    std::uint64_t x[31]{};
    Vec128 v[32]{};
    const auto entrySp = reinterpret_cast<std::uintptr_t>(simStack + sizeof(simStack));
    std::uint64_t sp = static_cast<std::uint64_t>(entrySp);

    constexpr std::uint64_t kReturnSentinel = 0xFFFF0000DEADBEEFULL;
    constexpr std::uint64_t kX18Canary = 0xDEAD1818DEAD1818ULL;
    x[0] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(abi));
    x[18] = kX18Canary;
    for (std::size_t r = 19; r <= 29; ++r) {
        x[r] = 0xA640000000000000ULL | r;
    }
    x[30] = kReturnSentinel;

    bool flagN = false, flagZ = false, flagC = false, flagV = false;

    const auto readX = [&](std::uint32_t r) noexcept -> std::uint64_t {
        return (r == 31u) ? 0ULL : x[r];
    };
    const auto readW = [&](std::uint32_t r) noexcept -> std::uint32_t {
        return (r == 31u) ? 0u : static_cast<std::uint32_t>(x[r]);
    };
    const auto writeX = [&](std::uint32_t r, std::uint64_t val) noexcept {
        if (r < 31u) x[r] = val;
    };
    const auto writeW = [&](std::uint32_t r, std::uint32_t val) noexcept {
        if (r < 31u) x[r] = static_cast<std::uint64_t>(val); // ARM64 32-bit write zero-extends to 64 bits
    };
    const auto readBase = [&](std::uint32_t rn) noexcept -> std::uint64_t {
        return (rn == 31u) ? sp : x[rn];
    };

    const auto evalCond = [&](std::uint32_t cond) noexcept -> bool {
        switch (cond & 0xFu) {
        case 0x0: return flagZ;                          // EQ
        case 0x1: return !flagZ;                         // NE
        case 0x2: return flagC;                          // HS / CS
        case 0x3: return !flagC;                         // LO / CC
        case 0x4: return flagN;                          // MI
        case 0x5: return !flagN;                         // PL
        case 0x6: return flagV;                          // VS
        case 0x7: return !flagV;                         // VC
        case 0x8: return flagC && !flagZ;                // HI
        case 0x9: return !flagC || flagZ;                // LS
        case 0xA: return flagN == flagV;                 // GE
        case 0xB: return flagN != flagV;                 // LT
        case 0xC: return !flagZ && (flagN == flagV);     // GT
        case 0xD: return flagZ || (flagN != flagV);      // LE
        default:  return true;                           // AL
        }
    };

    const auto setFlagsSub32 = [&](std::uint32_t a, std::uint32_t b) noexcept -> std::uint32_t {
        const std::uint32_t res = a - b;
        flagN = (res >> 31u) != 0u;
        flagZ = (res == 0u);
        flagC = (a >= b); // ARM64 SUBS: C=1 when NO borrow!
        flagV = (((a ^ b) & (a ^ res)) >> 31u) != 0u;
        return res;
    };
    const auto setFlagsAdd32 = [&](std::uint32_t a, std::uint32_t b) noexcept -> std::uint32_t {
        const std::uint64_t wide = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
        const auto res = static_cast<std::uint32_t>(wide);
        flagN = (res >> 31u) != 0u;
        flagZ = (res == 0u);
        flagC = (wide >> 32u) != 0u;
        flagV = (((~(a ^ b)) & (a ^ res)) >> 31u) != 0u;
        return res;
    };
    const auto setFlagsSub64 = [&](std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {
        const std::uint64_t res = a - b;
        flagN = (res >> 63u) != 0u;
        flagZ = (res == 0u);
        flagC = (a >= b);
        flagV = (((a ^ b) & (a ^ res)) >> 63u) != 0u;
        return res;
    };
    const auto setFlagsAdd64 = [&](std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {
        const std::uint64_t res = a + b;
        flagN = (res >> 63u) != 0u;
        flagZ = (res == 0u);
        flagC = (res < a);
        flagV = (((~(a ^ b)) & (a ^ res)) >> 63u) != 0u;
        return res;
    };

    std::size_t pc = 0;
    while (pc < code.size()) {
        if (x[18] != kX18Canary) {
            outSimDiagnostic = "ARM64 ABI violation: platform-reserved register X18 was modified";
            return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
        }
        std::uint32_t inst = 0;
        std::memcpy(&inst, code.data() + pc, 4);
        std::size_t nextPc = pc + 4u;

        const std::uint32_t rd = inst & 31u;
        const std::uint32_t rn = (inst >> 5u) & 31u;
        const std::uint32_t rm = (inst >> 16u) & 31u;

        // 1. Unconditional B imm26 (0x14000000 .. 0x17FFFFFF)
        if ((inst & 0xFC000000u) == 0x14000000u) {
            std::int32_t imm26 = static_cast<std::int32_t>(inst & 0x03FFFFFFu);
            if ((imm26 & (1 << 25)) != 0) imm26 |= ~0x03FFFFFF;
            nextPc = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(pc) + static_cast<std::ptrdiff_t>(imm26) * 4);
        }
        // 2. Conditional B.cond imm19 (0x54000000)
        else if ((inst & 0xFF000010u) == 0x54000000u) {
            if (evalCond(inst & 0xFu)) {
                std::int32_t imm19 = static_cast<std::int32_t>((inst >> 5u) & 0x7FFFFu);
                if ((imm19 & (1 << 18)) != 0) imm19 |= ~0x7FFFF;
                nextPc = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(pc) + static_cast<std::ptrdiff_t>(imm19) * 4);
            }
        }
        // 3. RET Xn (0xD65F0000)
        else if ((inst & 0xFFFFFC1Fu) == 0xD65F0000u) {
            const std::uint64_t target = readX(rn);
            if (target != kReturnSentinel) {
                outSimDiagnostic = "ARM64 RET target did not match saved LR";
                return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
            }
            if (sp != static_cast<std::uint64_t>(entrySp)) {
                outSimDiagnostic = "ARM64 SP mismatch on function return";
                return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
            }
            for (std::size_t r = 19; r <= 29; ++r) {
                if (x[r] != (0xA640000000000000ULL | r)) {
                    outSimDiagnostic = "ARM64 AAPCS64 callee-saved register X" + std::to_string(r) + " corrupted";
                    return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
                }
            }
            return static_cast<std::uint32_t>(x[0]);
        }
        // 4. BLR Xn (0xD63F0000) -> call real C++ StencilJit helper!
        else if ((inst & 0xFFFFFC1Fu) == 0xD63F0000u) {
            if ((sp & 0xFULL) != 0ULL) {
                outSimDiagnostic = "ARM64 ABI violation: SP not 16-byte aligned at BLR call site";
                return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
            }
            using HelperFn = std::uint32_t (*)(JitAbiContext*, std::uint32_t);
            const auto calleeAddr = static_cast<std::uintptr_t>(readX(rn));
            const auto helper = bitCastFnPtr<HelperFn>(reinterpret_cast<void*>(calleeAddr));
            auto* ctxArg = reinterpret_cast<JitAbiContext*>(static_cast<std::uintptr_t>(x[0]));
            const auto idxArg = static_cast<std::uint32_t>(x[1]);
            const std::uint32_t ret = helper(ctxArg, idxArg);
            // Poison caller-saved registers (except X0 return & platform-reserved X18) to prove ABI discipline!
            for (std::uint32_t r = 1; r <= 17; ++r) x[r] = 0xCCCCCCCCCCCCCCCCULL;
            for (std::uint32_t vr = 0; vr < 8; ++vr) std::memset(v[vr].b, 0xCC, 16);
            writeW(0, ret);
        }
        // 5. STP / LDP 64-bit signed offset (0xA9000000 / 0xA9400000)
        else if ((inst & 0xFFC00000u) == 0xA9000000u || (inst & 0xFFC00000u) == 0xA9400000u) {
            const bool isLoad = ((inst >> 22u) & 1u) != 0u;
            const std::uint32_t rt2 = (inst >> 10u) & 31u;
            std::int32_t imm7 = static_cast<std::int32_t>((inst >> 15u) & 0x7Fu);
            if ((imm7 & 0x40) != 0) imm7 |= ~0x7F;
            const auto addr = static_cast<std::uintptr_t>(
                static_cast<std::int64_t>(readBase(rn)) + static_cast<std::int64_t>(imm7) * 8LL);
            auto* ptr = reinterpret_cast<std::uint8_t*>(addr);
            if (isLoad) {
                std::uint64_t v1 = 0, v2 = 0;
                std::memcpy(&v1, ptr, 8);
                std::memcpy(&v2, ptr + 8, 8);
                writeX(rd, v1);
                writeX(rt2, v2);
            } else {
                const std::uint64_t v1 = readX(rd);
                const std::uint64_t v2 = readX(rt2);
                std::memcpy(ptr, &v1, 8);
                std::memcpy(ptr + 8, &v2, 8);
            }
        }
        // 6. Move Wide Immediate (MOVN / MOVZ / MOVK, 32-bit & 64-bit)
        else if ((inst & 0x1F800000u) == 0x12800000u) {
            const bool sf64 = (inst >> 31u) != 0u;
            const std::uint32_t opc = (inst >> 29u) & 3u; // 0=MOVN, 2=MOVZ, 3=MOVK
            const std::uint32_t hw = (inst >> 21u) & 3u;
            const std::uint64_t imm16 = (inst >> 5u) & 0xFFFFu;
            const std::uint32_t shift = hw * 16u;
            if (opc == 2u) {
                const std::uint64_t val = imm16 << shift;
                if (sf64) writeX(rd, val); else writeW(rd, static_cast<std::uint32_t>(val));
            } else if (opc == 0u) {
                const std::uint64_t val = ~(imm16 << shift);
                if (sf64) writeX(rd, val); else writeW(rd, static_cast<std::uint32_t>(val));
            } else if (opc == 3u) {
                const std::uint64_t mask = ~(0xFFFFULL << shift);
                if (sf64) {
                    writeX(rd, (readX(rd) & mask) | (imm16 << shift));
                } else {
                    writeW(rd, static_cast<std::uint32_t>((readW(rd) & mask) | (imm16 << shift)));
                }
            }
        }
        // 7. ADD / ADDS / SUB / SUBS (immediate, 32-bit & 64-bit)
        else if ((inst & 0x1F000000u) == 0x11000000u) {
            const bool sf64 = (inst >> 31u) != 0u;
            const bool isSub = ((inst >> 30u) & 1u) != 0u;
            const bool setFlags = ((inst >> 29u) & 1u) != 0u;
            const std::uint32_t sh = (inst >> 22u) & 3u;
            const std::uint64_t imm12 = (inst >> 10u) & 0xFFFu;
            const std::uint64_t imm = (sh == 1u) ? (imm12 << 12u) : imm12;
            const std::uint64_t baseVal = readBase(rn);
            if (sf64) {
                if (setFlags) {
                    const std::uint64_t res = isSub ? setFlagsSub64(baseVal, imm) : setFlagsAdd64(baseVal, imm);
                    writeX(rd, res); // rd==31 -> XZR discard!
                } else {
                    const std::uint64_t res = isSub ? (baseVal - imm) : (baseVal + imm);
                    if (rd == 31u) sp = res; else writeX(rd, res);
                }
            } else {
                const auto b32 = static_cast<std::uint32_t>(baseVal);
                const auto i32 = static_cast<std::uint32_t>(imm);
                if (setFlags) {
                    const std::uint32_t res = isSub ? setFlagsSub32(b32, i32) : setFlagsAdd32(b32, i32);
                    writeW(rd, res);
                } else {
                    const std::uint32_t res = isSub ? (b32 - i32) : (b32 + i32);
                    if (rd == 31u) sp = res; else writeW(rd, res);
                }
            }
        }
        // 8. ADD / ADDS / SUB / SUBS (shifted register, 32-bit & 64-bit)
        else if ((inst & 0x1F200000u) == 0x0B000000u) {
            const bool sf64 = (inst >> 31u) != 0u;
            const bool isSub = ((inst >> 30u) & 1u) != 0u;
            const bool setFlags = ((inst >> 29u) & 1u) != 0u;
            const std::uint32_t shiftAmt = (inst >> 10u) & 63u;
            if (sf64) {
                const std::uint64_t lhs = readX(rn);
                const std::uint64_t rhs = readX(rm) << shiftAmt;
                const std::uint64_t res = setFlags
                    ? (isSub ? setFlagsSub64(lhs, rhs) : setFlagsAdd64(lhs, rhs))
                    : (isSub ? (lhs - rhs) : (lhs + rhs));
                writeX(rd, res);
            } else {
                const std::uint32_t lhs = readW(rn);
                const std::uint32_t rhs = readW(rm) << (shiftAmt & 31u);
                const std::uint32_t res = setFlags
                    ? (isSub ? setFlagsSub32(lhs, rhs) : setFlagsAdd32(lhs, rhs))
                    : (isSub ? (lhs - rhs) : (lhs + rhs));
                writeW(rd, res);
            }
        }
        // 9. Logical (shifted register: ORR / EOR / AND, includes MOV reg)
        else if ((inst & 0x1F200000u) == 0x0A000000u) {
            const bool sf64 = (inst >> 31u) != 0u;
            const std::uint32_t opc = (inst >> 29u) & 3u; // 0=AND, 1=ORR, 2=EOR
            if (sf64) {
                const std::uint64_t lhs = readX(rn);
                const std::uint64_t rhs = readX(rm);
                const std::uint64_t res = (opc == 1u) ? (lhs | rhs) : ((opc == 2u) ? (lhs ^ rhs) : (lhs & rhs));
                writeX(rd, res);
            } else {
                const std::uint32_t lhs = readW(rn);
                const std::uint32_t rhs = readW(rm);
                const std::uint32_t res = (opc == 1u) ? (lhs | rhs) : ((opc == 2u) ? (lhs ^ rhs) : (lhs & rhs));
                writeW(rd, res);
            }
        }
        // 10. Bitfield: SXTW (0x93407C00), UBFM 64-bit LSL (0xD3400000), UBFM 32-bit LSR/UBFX (0x53000000)
        else if ((inst & 0xFFFFFC00u) == 0x93407C00u) {
            writeX(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(readW(rn)))));
        }
        else if ((inst & 0xFFC00000u) == 0xD3400000u) {
            const std::uint32_t immr = (inst >> 16u) & 63u;
            const std::uint32_t imms = (inst >> 10u) & 63u;
            const std::uint64_t src = readX(rn);
            if (imms + 1u == immr) {
                const std::uint32_t lsl = 64u - immr;
                writeX(rd, src << lsl);
            } else if (imms == 63u) {
                writeX(rd, src >> immr);
            } else {
                const std::uint32_t width = imms - immr + 1u;
                writeX(rd, (src >> immr) & ((1ULL << width) - 1ULL));
            }
        }
        else if ((inst & 0xFFC00000u) == 0x53000000u) {
            const std::uint32_t immr = (inst >> 16u) & 31u;
            const std::uint32_t imms = (inst >> 10u) & 31u;
            const std::uint32_t src = readW(rn);
            if (imms == 31u) {
                writeW(rd, src >> immr);
            } else if (immr == 0u) {
                const std::uint32_t mask = (imms == 31u) ? 0xFFFFFFFFu : ((1u << (imms + 1u)) - 1u);
                writeW(rd, src & mask);
            } else {
                const std::uint32_t lsl = 32u - immr;
                writeW(rd, src << lsl);
            }
        }
        // 11. Multiply / Divide: MUL Xd (0x9B007C00), SMULL Xd (0x9B207C00), SDIV Wd (0x1AC00C00), MSUB Wd (0x1B008000)
        else if ((inst & 0xFFE0FC00u) == 0x9B007C00u) {
            writeX(rd, readX(rn) * readX(rm));
        }
        else if ((inst & 0xFFE0FC00u) == 0x9B207C00u) {
            const auto a = static_cast<std::int64_t>(static_cast<std::int32_t>(readW(rn)));
            const auto b = static_cast<std::int64_t>(static_cast<std::int32_t>(readW(rm)));
            writeX(rd, static_cast<std::uint64_t>(a * b));
        }
        else if ((inst & 0xFFE0FC00u) == 0x1AC00C00u) {
            const auto a = static_cast<std::int32_t>(readW(rn));
            const auto b = static_cast<std::int32_t>(readW(rm));
            std::int32_t q = 0;
            if (b != 0) {
                q = (a == std::numeric_limits<std::int32_t>::min() && b == -1) ? a : (a / b);
            }
            writeW(rd, static_cast<std::uint32_t>(q));
        }
        else if ((inst & 0xFFE08000u) == 0x1B008000u) {
            const std::uint32_t ra = (inst >> 10u) & 31u;
            const std::uint32_t res = readW(ra) - (readW(rn) * readW(rm));
            writeW(rd, res);
        }
        // 12. Conditional Select Increment: CSINC Wd, WZR, WZR, invCond (CSET Wd, cond)
        else if ((inst & 0xFFFF0FE0u) == 0x1A9F07E0u) {
            const std::uint32_t invCond = (inst >> 12u) & 0xFu;
            writeW(rd, evalCond(invCond) ? 0u : 1u);
        }
        // 13. Unsigned-Immediate GP & SIMD Loads/Stores
        else if ((inst & 0x3B000000u) == 0x39000000u) {
            const std::uint32_t sizeBits = (inst >> 30u) & 3u;
            const bool isSimd = ((inst >> 26u) & 1u) != 0u;
            const std::uint32_t opc = (inst >> 22u) & 3u;
            const std::uint32_t uimm12 = (inst >> 10u) & 0xFFFu;
            const std::uint64_t baseAddr = readBase(rn);

            if (!isSimd) {
                const std::uint64_t addr = baseAddr + (static_cast<std::uint64_t>(uimm12) << sizeBits);
                auto* ptr = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr));
                if (sizeBits == 0u && opc == 1u) { // LDRB Wt
                    writeW(rd, *ptr);
                } else if (sizeBits == 0u && opc == 0u) { // STRB Wt
                    *ptr = static_cast<std::uint8_t>(readW(rd) & 0xFFu);
                } else if (sizeBits == 1u && opc == 1u) { // LDRH Wt
                    std::uint16_t tmp = 0;
                    std::memcpy(&tmp, ptr, 2);
                    writeW(rd, tmp);
                } else if (sizeBits == 2u && opc == 1u) { // LDR Wt
                    std::uint32_t tmp = 0;
                    std::memcpy(&tmp, ptr, 4);
                    writeW(rd, tmp);
                } else if (sizeBits == 2u && opc == 2u) { // LDRSW Xt
                    std::int32_t tmp = 0;
                    std::memcpy(&tmp, ptr, 4);
                    writeX(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(tmp)));
                } else if (sizeBits == 2u && opc == 0u) { // STR Wt
                    const std::uint32_t tmp = readW(rd);
                    std::memcpy(ptr, &tmp, 4);
                } else if (sizeBits == 3u && opc == 1u) { // LDR Xt
                    std::uint64_t tmp = 0;
                    std::memcpy(&tmp, ptr, 8);
                    writeX(rd, tmp);
                } else if (sizeBits == 3u && opc == 0u) { // STR Xt
                    const std::uint64_t tmp = readX(rd);
                    std::memcpy(ptr, &tmp, 8);
                } else {
                    outSimDiagnostic = "Unsupported GP LDR/STR encoding in ARM64 simulator";
                    return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
                }
            } else {
                if (sizeBits == 0u && (opc == 3u || opc == 2u)) { // LDR Qt (opc=3) / STR Qt (opc=2)
                    const std::uint64_t addr = baseAddr + (static_cast<std::uint64_t>(uimm12) * 16ULL);
                    auto* ptr = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr));
                    if (opc == 3u) std::memcpy(v[rd].b, ptr, 16);
                    else std::memcpy(ptr, v[rd].b, 16);
                } else if (sizeBits == 3u && (opc == 1u || opc == 0u)) { // LDR Dt / STR Dt
                    const std::uint64_t addr = baseAddr + (static_cast<std::uint64_t>(uimm12) * 8ULL);
                    auto* ptr = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr));
                    if (opc == 1u) {
                        std::memset(v[rd].b, 0, 16);
                        std::memcpy(v[rd].b, ptr, 8);
                    } else {
                        std::memcpy(ptr, v[rd].b, 8);
                    }
                } else if (sizeBits == 2u && (opc == 1u || opc == 0u)) { // LDR St / STR St
                    const std::uint64_t addr = baseAddr + (static_cast<std::uint64_t>(uimm12) * 4ULL);
                    auto* ptr = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr));
                    if (opc == 1u) {
                        std::memset(v[rd].b, 0, 16);
                        std::memcpy(v[rd].b, ptr, 4);
                    } else {
                        std::memcpy(ptr, v[rd].b, 4);
                    }
                } else {
                    outSimDiagnostic = "Unsupported SIMD LDR/STR encoding in ARM64 simulator";
                    return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
                }
            }
        }
        // 14. NEON EOR Vd.16B, Vn.16B, Vm.16B (0x6E201C00)
        else if ((inst & 0xFFE0FC00u) == 0x6E201C00u) {
            for (std::size_t k = 0; k < 16u; ++k) {
                v[rd].b[k] = static_cast<std::uint8_t>(v[rn].b[k] ^ v[rm].b[k]);
            }
        }
        // 15. Scalar Double FP: FADD / FSUB / FMUL / FDIV / FCMP
        else if ((inst & 0xFFE0FC00u) == 0x1E602800u ||
                 (inst & 0xFFE0FC00u) == 0x1E603800u ||
                 (inst & 0xFFE0FC00u) == 0x1E600800u ||
                 (inst & 0xFFE0FC00u) == 0x1E601800u) {
            double a = 0.0, b = 0.0;
            std::memcpy(&a, v[rn].b, 8);
            std::memcpy(&b, v[rm].b, 8);
            const std::uint32_t op = (inst >> 12u) & 0xFu;
            double r = 0.0;
            if (op == 2u) r = a + b;
            else if (op == 3u) r = a - b;
            else if (op == 0u) r = a * b;
            else r = a / b;
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &r, 8);
        }
        else if ((inst & 0xFFE0FC1Fu) == 0x1E602000u) {
            double a = 0.0, b = 0.0;
            std::memcpy(&a, v[rn].b, 8);
            std::memcpy(&b, v[rm].b, 8);
            if (std::isnan(a) || std::isnan(b)) {
                flagN = false; flagZ = false; flagC = true; flagV = true;
            } else if (a == b) {
                flagN = false; flagZ = true; flagC = true; flagV = false;
            } else if (a < b) {
                flagN = true; flagZ = false; flagC = false; flagV = false;
            } else {
                flagN = false; flagZ = false; flagC = true; flagV = false;
            }
        }
        // 16. Scalar Float FP: FADD / FSUB / FMUL / FDIV / FCMP
        else if ((inst & 0xFFE0FC00u) == 0x1E202800u ||
                 (inst & 0xFFE0FC00u) == 0x1E203800u ||
                 (inst & 0xFFE0FC00u) == 0x1E200800u ||
                 (inst & 0xFFE0FC00u) == 0x1E201800u) {
            float a = 0.0f, b = 0.0f;
            std::memcpy(&a, v[rn].b, 4);
            std::memcpy(&b, v[rm].b, 4);
            const std::uint32_t op = (inst >> 12u) & 0xFu;
            float r = 0.0f;
            if (op == 2u) r = a + b;
            else if (op == 3u) r = a - b;
            else if (op == 0u) r = a * b;
            else r = a / b;
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &r, 4);
        }
        else if ((inst & 0xFFE0FC1Fu) == 0x1E202000u) {
            float a = 0.0f, b = 0.0f;
            std::memcpy(&a, v[rn].b, 4);
            std::memcpy(&b, v[rm].b, 4);
            if (std::isnan(a) || std::isnan(b)) {
                flagN = false; flagZ = false; flagC = true; flagV = true;
            } else if (a == b) {
                flagN = false; flagZ = true; flagC = true; flagV = false;
            } else if (a < b) {
                flagN = true; flagZ = false; flagC = false; flagV = false;
            } else {
                flagN = false; flagZ = false; flagC = true; flagV = false;
            }
        }
        // 17. FP <-> GP Conversions: FMOV D, X; SCVTF D, X; SCVTF S, X; FCVT D, S; FCVT S, D; FCVTZS X, D
        else if ((inst & 0xFFFFFC00u) == 0x9E670000u) {
            const std::uint64_t bits = readX(rn);
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &bits, 8);
        }
        else if ((inst & 0xFFFFFC00u) == 0x9E620000u) {
            const double dv = static_cast<double>(static_cast<std::int64_t>(readX(rn)));
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &dv, 8);
        }
        else if ((inst & 0xFFFFFC00u) == 0x9E220000u) {
            const float fv = static_cast<float>(static_cast<std::int64_t>(readX(rn)));
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &fv, 4);
        }
        else if ((inst & 0xFFFFFC00u) == 0x1E22C000u) {
            float fv = 0.0f;
            std::memcpy(&fv, v[rn].b, 4);
            const double dv = static_cast<double>(fv);
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &dv, 8);
        }
        else if ((inst & 0xFFFFFC00u) == 0x1E624000u) {
            double dv = 0.0;
            std::memcpy(&dv, v[rn].b, 8);
            const float fv = static_cast<float>(dv);
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &fv, 4);
        }
        else if ((inst & 0xFFFFFC00u) == 0x9E780000u) {
            double dv = 0.0;
            std::memcpy(&dv, v[rn].b, 8);
            writeX(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(dv)));
        }
        else {
            std::ostringstream oss;
            oss << "Unrecognized ARM64 instruction word 0x" << std::hex << std::setw(8)
                << std::setfill('0') << inst << " at byte offset " << std::dec << pc;
            outSimDiagnostic = oss.str();
            return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
        }

        pc = nextPc;
    }

    outSimDiagnostic = "ARM64 simulator ran off end of code buffer";
    return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
}

} // namespace realscript::jit::jit_detail

#endif // UINTPTR_MAX == 0xffffffffffffffffULL
