#include "JitInternal.h"

#if UINTPTR_MAX == 0xffffffffffffffffULL

namespace realscript::jit::jit_detail {
namespace {

enum class X64Reg : std::uint8_t {
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8  = 8,
    R9  = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,
};

enum class X64Cond : std::uint8_t {
    O  = 0x0, // jo
    NO = 0x1, // jno
    B  = 0x2, // jb / jc (unsigned <)
    AE = 0x3, // jae / jnc (unsigned >=)
    E  = 0x4, // je / jz
    NE = 0x5, // jne / jnz
    BE = 0x6, // jbe (unsigned <=)
    A  = 0x7, // ja (unsigned >)
    P  = 0xA, // jp (parity / NaN unordered)
    NP = 0xB, // jnp
    L  = 0xC, // jl (signed <)
    GE = 0xD, // jge (signed >=)
    LE = 0xE, // jle (signed <=)
    G  = 0xF, // jg (signed >)
};

[[nodiscard]] constexpr X64Cond invertCond(X64Cond cc) noexcept {
    return static_cast<X64Cond>(static_cast<std::uint8_t>(cc) ^ 1u);
}

[[nodiscard]] constexpr X64Cond quickCmpToCond(runtime::detail::QuickOpcode op) noexcept {
    using QO = runtime::detail::QuickOpcode;
    switch (op) {
    case QO::LessInt: return X64Cond::L;
    case QO::LessOrEqualInt: return X64Cond::LE;
    case QO::GreaterInt: return X64Cond::G;
    case QO::GreaterOrEqualInt: return X64Cond::GE;
    case QO::EqualInt: return X64Cond::E;
    case QO::NotEqualInt: return X64Cond::NE;
    default: return X64Cond::E;
    }
}

class X64Emitter {
public:
    struct Label {
        std::size_t boundOffset = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> patches;
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
        for (const std::size_t patchPos : lbl.patches) {
            patchRel32(patchPos, lbl.boundOffset);
        }
        lbl.patches.clear();
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

    void emit8(std::uint8_t b) { buf_.push_back(b); }
    void emit32(std::uint32_t v) {
        std::uint8_t tmp[4];
        std::memcpy(tmp, &v, 4);
        buf_.insert(buf_.end(), tmp, tmp + 4);
    }
    void emit64(std::uint64_t v) {
        std::uint8_t tmp[8];
        std::memcpy(tmp, &v, 8);
        buf_.insert(buf_.end(), tmp, tmp + 8);
    }

    void emitRex(bool w, std::uint8_t reg, std::uint8_t idx, std::uint8_t rm) {
        const std::uint8_t rex = static_cast<std::uint8_t>(
            0x40u |
            (w ? 0x08u : 0u) |
            ((reg >= 8u) ? 0x04u : 0u) |
            ((idx >= 8u) ? 0x02u : 0u) |
            ((rm >= 8u) ? 0x01u : 0u));
        if (rex != 0x40u) emit8(rex);
    }

    void emitModRMReg(std::uint8_t reg, std::uint8_t rm) {
        emit8(static_cast<std::uint8_t>((3u << 6) | ((reg & 7u) << 3) | (rm & 7u)));
    }

    void emitMem(std::uint8_t regOrDigit, X64Reg base, std::int32_t disp) {
        const std::uint8_t b = static_cast<std::uint8_t>(base) & 7u;
        const std::uint8_t r = regOrDigit & 7u;
        if (disp == 0 && b != 5u) {
            emit8(static_cast<std::uint8_t>((0u << 6) | (r << 3) | b));
            if (b == 4u) emit8(0x24u);
        } else if (disp >= -128 && disp <= 127) {
            emit8(static_cast<std::uint8_t>((1u << 6) | (r << 3) | b));
            if (b == 4u) emit8(0x24u);
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(disp)));
        } else {
            emit8(static_cast<std::uint8_t>((2u << 6) | (r << 3) | b));
            if (b == 4u) emit8(0x24u);
            emit32(static_cast<std::uint32_t>(disp));
        }
    }

    // Indexed SIB [base + index * scale], index != RSP/R12, disp == 0
    void emitSibMem0(std::uint8_t reg, X64Reg base, X64Reg index, std::uint8_t scaleShift) {
        const std::uint8_t b = static_cast<std::uint8_t>(base) & 7u;
        const std::uint8_t ix = static_cast<std::uint8_t>(index) & 7u;
        if (b == 5u) {
            emit8(static_cast<std::uint8_t>((1u << 6) | ((reg & 7u) << 3) | 4u));
            emit8(static_cast<std::uint8_t>(((scaleShift & 3u) << 6) | (ix << 3) | b));
            emit8(0x00u);
        } else {
            emit8(static_cast<std::uint8_t>((0u << 6) | ((reg & 7u) << 3) | 4u));
            emit8(static_cast<std::uint8_t>(((scaleShift & 3u) << 6) | (ix << 3) | b));
        }
    }

    void push(X64Reg r) {
        const auto rv = static_cast<std::uint8_t>(r);
        if (rv >= 8u) emit8(0x41u);
        emit8(static_cast<std::uint8_t>(0x50u + (rv & 7u)));
    }

    void pop(X64Reg r) {
        const auto rv = static_cast<std::uint8_t>(r);
        if (rv >= 8u) emit8(0x41u);
        emit8(static_cast<std::uint8_t>(0x58u + (rv & 7u)));
    }

    void ret() { emit8(0xC3u); }

    void movReg64(X64Reg dst, X64Reg src) {
        emitRex(true, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(dst));
        emit8(0x89u);
        emitModRMReg(static_cast<std::uint8_t>(src), static_cast<std::uint8_t>(dst));
    }

    void xorReg32(X64Reg dst, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(dst));
        emit8(0x31u);
        emitModRMReg(static_cast<std::uint8_t>(src), static_cast<std::uint8_t>(dst));
    }

    void movImm32(X64Reg dst, std::uint32_t imm) {
        const auto dv = static_cast<std::uint8_t>(dst);
        if (imm == 0u) {
            xorReg32(dst, dst);
            return;
        }
        if (dv >= 8u) emit8(0x41u);
        emit8(static_cast<std::uint8_t>(0xB8u + (dv & 7u)));
        emit32(imm);
    }

    void movImm64(X64Reg dst, std::uint64_t imm) {
        if (imm == 0u) {
            xorReg32(dst, dst);
            return;
        }
        if (imm <= 0x7fffffffULL) {
            movImm32(dst, static_cast<std::uint32_t>(imm));
            return;
        }
        const auto s64 = static_cast<std::int64_t>(imm);
        if (fitsInt32(s64)) {
            emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
            emit8(0xC7u);
            emitModRMReg(0, static_cast<std::uint8_t>(dst));
            emit32(static_cast<std::uint32_t>(static_cast<std::int32_t>(s64)));
            return;
        }
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(static_cast<std::uint8_t>(0xB8u + (static_cast<std::uint8_t>(dst) & 7u)));
        emit64(imm);
    }

    void loadMem64(X64Reg dst, X64Reg base, std::int32_t disp) {
        emitRex(true, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(base));
        emit8(0x8Bu);
        emitMem(static_cast<std::uint8_t>(dst), base, disp);
    }

    void storeMem64(X64Reg base, std::int32_t disp, X64Reg src) {
        emitRex(true, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(base));
        emit8(0x89u);
        emitMem(static_cast<std::uint8_t>(src), base, disp);
    }

    void storeMem64Imm32SignExt(X64Reg base, std::int32_t disp, std::int32_t imm) {
        emitRex(true, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0xC7u);
        emitMem(0, base, disp);
        emit32(static_cast<std::uint32_t>(imm));
    }

    void loadMem32(X64Reg dst, X64Reg base, std::int32_t disp) {
        emitRex(false, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(base));
        emit8(0x8Bu);
        emitMem(static_cast<std::uint8_t>(dst), base, disp);
    }

    void storeMem32(X64Reg base, std::int32_t disp, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(base));
        emit8(0x89u);
        emitMem(static_cast<std::uint8_t>(src), base, disp);
    }

    void storeMem32Imm(X64Reg base, std::int32_t disp, std::uint32_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0xC7u);
        emitMem(0, base, disp);
        emit32(imm);
    }

    void loadMem8ZeroExt(X64Reg dst, X64Reg base, std::int32_t disp) {
        emitRex(false, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0xB6u);
        emitMem(static_cast<std::uint8_t>(dst), base, disp);
    }

    // Store low 8-bit of RAX/RCX/RDX/RBX (codes 0..3)
    void storeMem8Reg(X64Reg base, std::int32_t disp, X64Reg byteSrc0To3) {
        emitRex(false, static_cast<std::uint8_t>(byteSrc0To3), 0, static_cast<std::uint8_t>(base));
        emit8(0x88u);
        emitMem(static_cast<std::uint8_t>(byteSrc0To3), base, disp);
    }

    void storeMem8Imm(X64Reg base, std::int32_t disp, std::uint8_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0xC6u);
        emitMem(0, base, disp);
        emit8(imm);
    }

    void cmpMem8Imm(X64Reg base, std::int32_t disp, std::uint8_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0x80u);
        emitMem(7, base, disp);
        emit8(imm);
    }

    void movsxd64From32(X64Reg dst64, X64Reg src32) {
        emitRex(true, static_cast<std::uint8_t>(dst64), 0, static_cast<std::uint8_t>(src32));
        emit8(0x63u);
        emitModRMReg(static_cast<std::uint8_t>(dst64), static_cast<std::uint8_t>(src32));
    }

    void addImm64(X64Reg dst, std::int32_t imm) {
        if (imm == 0) return;
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        if (imm >= -128 && imm <= 127) {
            emit8(0x83u);
            emitModRMReg(0, static_cast<std::uint8_t>(dst));
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x81u);
            emitModRMReg(0, static_cast<std::uint8_t>(dst));
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void subImm64(X64Reg dst, std::int32_t imm) {
        if (imm == 0) return;
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        if (imm >= -128 && imm <= 127) {
            emit8(0x83u);
            emitModRMReg(5, static_cast<std::uint8_t>(dst));
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x81u);
            emitModRMReg(5, static_cast<std::uint8_t>(dst));
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void inc64(X64Reg dst) {
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0xFFu);
        emitModRMReg(0, static_cast<std::uint8_t>(dst));
    }

    void addReg32(X64Reg dst, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(dst));
        emit8(0x01u);
        emitModRMReg(static_cast<std::uint8_t>(src), static_cast<std::uint8_t>(dst));
    }

    void subReg32(X64Reg dst, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(dst));
        emit8(0x29u);
        emitModRMReg(static_cast<std::uint8_t>(src), static_cast<std::uint8_t>(dst));
    }

    void imulReg32(X64Reg dst, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(src));
        emit8(0x0Fu);
        emit8(0xAFu);
        emitModRMReg(static_cast<std::uint8_t>(dst), static_cast<std::uint8_t>(src));
    }

    void negReg32(X64Reg dst) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0xF7u);
        emitModRMReg(3, static_cast<std::uint8_t>(dst));
    }

    void cdq() { emit8(0x99u); }

    void idivReg32(X64Reg divisor) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(divisor));
        emit8(0xF7u);
        emitModRMReg(7, static_cast<std::uint8_t>(divisor));
    }

    void testReg32(X64Reg a, X64Reg b) {
        emitRex(false, static_cast<std::uint8_t>(b), 0, static_cast<std::uint8_t>(a));
        emit8(0x85u);
        emitModRMReg(static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a));
    }

    void cmpReg32(X64Reg lhs, X64Reg rhs) {
        emitRex(false, static_cast<std::uint8_t>(rhs), 0, static_cast<std::uint8_t>(lhs));
        emit8(0x39u);
        emitModRMReg(static_cast<std::uint8_t>(rhs), static_cast<std::uint8_t>(lhs));
    }

    void cmpReg64(X64Reg lhs, X64Reg rhs) {
        emitRex(true, static_cast<std::uint8_t>(rhs), 0, static_cast<std::uint8_t>(lhs));
        emit8(0x39u);
        emitModRMReg(static_cast<std::uint8_t>(rhs), static_cast<std::uint8_t>(lhs));
    }

    void cmpReg32Imm(X64Reg lhs, std::int32_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(lhs));
        if (imm >= -128 && imm <= 127) {
            emit8(0x83u);
            emitModRMReg(7, static_cast<std::uint8_t>(lhs));
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x81u);
            emitModRMReg(7, static_cast<std::uint8_t>(lhs));
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void cmpReg64Mem(X64Reg lhs, X64Reg base, std::int32_t disp) {
        emitRex(true, static_cast<std::uint8_t>(lhs), 0, static_cast<std::uint8_t>(base));
        emit8(0x3Bu); // CMP r64, r/m64 (lhs - [base+disp])
        emitMem(static_cast<std::uint8_t>(lhs), base, disp);
    }

    void cmpReg32Mem(X64Reg lhs, X64Reg base, std::int32_t disp) {
        emitRex(false, static_cast<std::uint8_t>(lhs), 0, static_cast<std::uint8_t>(base));
        emit8(0x3Bu); // CMP r32, r/m32 (lhs - [base+disp])
        emitMem(static_cast<std::uint8_t>(lhs), base, disp);
    }

    void cmpMem64Imm32(X64Reg base, std::int32_t disp, std::int32_t imm) {
        emitRex(true, 0, 0, static_cast<std::uint8_t>(base));
        if (imm >= -128 && imm <= 127) {
            emit8(0x83u);
            emitMem(7, base, disp);
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x81u);
            emitMem(7, base, disp);
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void setccAl(X64Cond cc) {
        emit8(0x0Fu);
        emit8(static_cast<std::uint8_t>(0x90u + static_cast<std::uint8_t>(cc)));
        emit8(0xC0u); // AL
    }

    void setccCl(X64Cond cc) {
        emit8(0x0Fu);
        emit8(static_cast<std::uint8_t>(0x90u + static_cast<std::uint8_t>(cc)));
        emit8(0xC1u); // CL
    }

    // SSE / XMM instructions (mandatory prefix BEFORE REX!)
    void movupsLoad(std::uint8_t xmmDst, X64Reg base, std::int32_t disp) {
        emitRex(false, xmmDst, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x10u);
        emitMem(xmmDst, base, disp);
    }

    void movupsStore(X64Reg base, std::int32_t disp, std::uint8_t xmmSrc) {
        emitRex(false, xmmSrc, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x11u);
        emitMem(xmmSrc, base, disp);
    }

    void movupsLoadSib1(std::uint8_t xmmDst, X64Reg base, X64Reg idx) {
        emitRex(false, xmmDst, static_cast<std::uint8_t>(idx), static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x10u);
        emitSibMem0(xmmDst, base, idx, 0);
    }

    void movupsStoreSib1(X64Reg base, X64Reg idx, std::uint8_t xmmSrc) {
        emitRex(false, xmmSrc, static_cast<std::uint8_t>(idx), static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x11u);
        emitSibMem0(xmmSrc, base, idx, 0);
    }

    void leaDisp64(X64Reg dst, X64Reg base, std::int32_t disp) {
        emitRex(true, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(base));
        emit8(0x8Du);
        emitMem(static_cast<std::uint8_t>(dst), base, disp);
    }

    void leaSib1_64(X64Reg dst, X64Reg base, X64Reg idx) {
        emitRex(true, static_cast<std::uint8_t>(dst), static_cast<std::uint8_t>(idx), static_cast<std::uint8_t>(base));
        emit8(0x8Du);
        emitSibMem0(static_cast<std::uint8_t>(dst), base, idx, 0);
    }

    void shlImm64(X64Reg dst, std::uint8_t shift) {
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0xC1u);
        emitModRMReg(4, static_cast<std::uint8_t>(dst));
        emit8(shift);
    }

    void shrImm32(X64Reg dst, std::uint8_t shift) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0xC1u);
        emitModRMReg(5, static_cast<std::uint8_t>(dst));
        emit8(shift);
    }

    void andImm32(X64Reg dst, std::uint32_t mask) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0x81u);
        emitModRMReg(4, static_cast<std::uint8_t>(dst));
        emit32(mask);
    }

    void orImm32(X64Reg dst, std::uint8_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0x83u);
        emitModRMReg(1, static_cast<std::uint8_t>(dst));
        emit8(imm);
    }

    void imulImm32_64(X64Reg dst, X64Reg src, std::int32_t imm) {
        emitRex(true, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(src));
        if (imm >= -128 && imm <= 127) {
            emit8(0x6Bu);
            emitModRMReg(static_cast<std::uint8_t>(dst), static_cast<std::uint8_t>(src));
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x69u);
            emitModRMReg(static_cast<std::uint8_t>(dst), static_cast<std::uint8_t>(src));
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void cmpMem16Imm(X64Reg base, std::int32_t disp, std::uint16_t imm) {
        emit8(0x66u);
        emitRex(false, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0x81u);
        emitMem(7, base, disp);
        emit8(static_cast<std::uint8_t>(imm & 0xFFu));
        emit8(static_cast<std::uint8_t>((imm >> 8u) & 0xFFu));
    }

    void repMovsq() {
        emit8(0xF3u);
        emit8(0x48u);
        emit8(0xA5u);
    }

    void xorpsSelf(std::uint8_t xmm) {
        emit8(0x0Fu);
        emit8(0x57u);
        emitModRMReg(xmm, xmm);
    }

    void movsdLoad(std::uint8_t xmmDst, X64Reg base, std::int32_t disp) {
        emit8(0xF2u);
        emitRex(false, xmmDst, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x10u);
        emitMem(xmmDst, base, disp);
    }

    void movsdStore(X64Reg base, std::int32_t disp, std::uint8_t xmmSrc) {
        emit8(0xF2u);
        emitRex(false, xmmSrc, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x11u);
        emitMem(xmmSrc, base, disp);
    }

    void movssLoad(std::uint8_t xmmDst, X64Reg base, std::int32_t disp) {
        emit8(0xF3u);
        emitRex(false, xmmDst, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x10u);
        emitMem(xmmDst, base, disp);
    }

    void movssStore(X64Reg base, std::int32_t disp, std::uint8_t xmmSrc) {
        emit8(0xF3u);
        emitRex(false, xmmSrc, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x11u);
        emitMem(xmmSrc, base, disp);
    }

    void movqToXmm(std::uint8_t xmmDst, X64Reg src64) {
        emit8(0x66u);
        emitRex(true, xmmDst, 0, static_cast<std::uint8_t>(src64));
        emit8(0x0Fu);
        emit8(0x6Eu);
        emitModRMReg(xmmDst, static_cast<std::uint8_t>(src64));
    }

    void binarySd(std::uint8_t opByte, std::uint8_t xmmDst, std::uint8_t xmmSrc) {
        emit8(0xF2u);
        emit8(0x0Fu);
        emit8(opByte);
        emitModRMReg(xmmDst, xmmSrc);
    }

    void binarySs(std::uint8_t opByte, std::uint8_t xmmDst, std::uint8_t xmmSrc) {
        emit8(0xF3u);
        emit8(0x0Fu);
        emit8(opByte);
        emitModRMReg(xmmDst, xmmSrc);
    }

    void ucomisd(std::uint8_t xmmLhs, std::uint8_t xmmRhs) {
        emit8(0x66u);
        emit8(0x0Fu);
        emit8(0x2Eu);
        emitModRMReg(xmmLhs, xmmRhs);
    }

    void ucomiss(std::uint8_t xmmLhs, std::uint8_t xmmRhs) {
        emit8(0x0Fu);
        emit8(0x2Eu);
        emitModRMReg(xmmLhs, xmmRhs);
    }

    void cvtsi2sd64(std::uint8_t xmmDst, X64Reg src64) {
        xorpsSelf(xmmDst);
        emit8(0xF2u);
        emitRex(true, xmmDst, 0, static_cast<std::uint8_t>(src64));
        emit8(0x0Fu);
        emit8(0x2Au);
        emitModRMReg(xmmDst, static_cast<std::uint8_t>(src64));
    }

    void cvtsi2ss64(std::uint8_t xmmDst, X64Reg src64) {
        xorpsSelf(xmmDst);
        emit8(0xF3u);
        emitRex(true, xmmDst, 0, static_cast<std::uint8_t>(src64));
        emit8(0x0Fu);
        emit8(0x2Au);
        emitModRMReg(xmmDst, static_cast<std::uint8_t>(src64));
    }

    void cvtss2sd(std::uint8_t xmmDst, std::uint8_t xmmSrc) {
        emit8(0xF3u);
        emit8(0x0Fu);
        emit8(0x5Au);
        emitModRMReg(xmmDst, xmmSrc);
    }

    void cvtsd2ss(std::uint8_t xmmDst, std::uint8_t xmmSrc) {
        emit8(0xF2u);
        emit8(0x0Fu);
        emit8(0x5Au);
        emitModRMReg(xmmDst, xmmSrc);
    }

    void callReg(X64Reg target) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(target));
        emit8(0xFFu);
        emitModRMReg(2, static_cast<std::uint8_t>(target));
    }

    void jmp(std::uint32_t labelId) {
        emit8(0xE9u);
        emitBranchDisplacement(labelId);
    }

    void jcc(X64Cond cc, std::uint32_t labelId) {
        emit8(0x0Fu);
        emit8(static_cast<std::uint8_t>(0x80u + static_cast<std::uint8_t>(cc)));
        emitBranchDisplacement(labelId);
    }

    [[nodiscard]] bool allLabelsBound() const noexcept {
        for (const auto& lbl : labels_) {
            if (!lbl.patches.empty() && !lbl.bound()) return false;
        }
        return true;
    }

private:
    void emitBranchDisplacement(std::uint32_t labelId) {
        const std::size_t patchPos = buf_.size();
        emit32(0u);
        auto& lbl = labels_[labelId];
        if (lbl.bound()) {
            patchRel32(patchPos, lbl.boundOffset);
        } else {
            lbl.patches.push_back(patchPos);
        }
    }

    void patchRel32(std::size_t patchPos, std::size_t targetOffset) {
        const std::ptrdiff_t rel =
            static_cast<std::ptrdiff_t>(targetOffset) -
            static_cast<std::ptrdiff_t>(patchPos + 4u);
        const auto rel32 = static_cast<std::int32_t>(rel);
        std::memcpy(buf_.data() + patchPos, &rel32, 4);
    }

    std::vector<std::uint8_t> buf_;
    std::vector<Label> labels_;
};

} // namespace

[[nodiscard]] std::vector<std::uint8_t> compileFunctionX64(
    const CompiledFunctionRecord& record) {
    const auto& execFunc = *record.execFunc;
    const std::size_t nInst = execFunc.flatStream.size();

    X64Emitter as;
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

    // Function Prologue (16-byte stack aligned for both SysV AMD64 & Win64 ABIs)
    as.push(X64Reg::RBP);
    as.movReg64(X64Reg::RBP, X64Reg::RSP);
    as.push(X64Reg::RBX);
    as.push(X64Reg::RSI);
    as.push(X64Reg::RDI);
    as.push(X64Reg::R12);
    as.push(X64Reg::R13);
    as.push(X64Reg::R14);
    as.push(X64Reg::R15);
    as.subImm64(X64Reg::RSP, 40);

#if defined(_WIN32)
    as.movReg64(X64Reg::R12, X64Reg::RCX);
#else
    as.movReg64(X64Reg::R12, X64Reg::RDI);
#endif
    as.loadMem64(X64Reg::RBX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, frameBase)));
    as.loadMem64(X64Reg::R13, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)));
    as.loadMem64(X64Reg::R14, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)));

    if (execFunc.entryStreamOffset != 0) {
        as.jmp(instLabels[execFunc.entryStreamOffset]);
    }

    const auto slotOff = [](std::uint16_t slot) noexcept -> std::int32_t {
        return static_cast<std::int32_t>(slot) * 16;
    };

    const auto emitBudgetDeduct = [&](std::uint32_t idx, std::uint8_t span, bool mayNeedPartialStep) {
        as.subImm64(X64Reg::R13, static_cast<std::int32_t>(span));
        if (span > 1u && mayNeedPartialStep) {
            const auto stub = addColdStub(
                ColdTrapStub::Kind::BudgetPartialSuper, idx, static_cast<std::int32_t>(span));
            as.jcc(X64Cond::B, stub);
        } else {
            const auto stub = addColdStub(ColdTrapStub::Kind::BudgetSimple, idx);
            as.jcc(X64Cond::B, stub);
        }
    };

    const auto emitCheckTagOrTrap = [&](
        std::uint16_t slot,
        std::uint8_t expectedTag,
        std::uint32_t idx,
        std::int32_t restoreBudgetAfterFail) {
        as.cmpMem8Imm(X64Reg::RBX, slotOff(slot) + 15, expectedTag);
        const auto stub = addColdStub(ColdTrapStub::Kind::TypeMismatch, idx, restoreBudgetAfterFail);
        as.jcc(X64Cond::NE, stub);
    };

    const auto emitHelperCallInline = [&](
        std::uint32_t (*helperFn)(JitAbiContext*, std::uint32_t),
        std::uint32_t flatIdx) {
        as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
        as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
#if defined(_WIN32)
        as.movReg64(X64Reg::RCX, X64Reg::R12);
        as.movImm32(X64Reg::RDX, flatIdx);
#else
        as.movReg64(X64Reg::RDI, X64Reg::R12);
        as.movImm32(X64Reg::RSI, flatIdx);
#endif
        as.movImm64(X64Reg::RAX, fnPtrAddress(helperFn));
        as.callReg(X64Reg::RAX);
        as.loadMem64(X64Reg::RBX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, frameBase)));
        as.loadMem64(X64Reg::R13, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)));
        as.loadMem64(X64Reg::R14, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)));
        as.testReg32(X64Reg::RAX, X64Reg::RAX);
        as.jcc(X64Cond::NE, epilogueHelperErr);
    };

    // Emits integer ALU/comparison on EAX (lhs) and ECX (rhs).
    // Returns output tag (kTagInt64 -> result in RAX; kTagBool -> result 0/1 in AL).
    const auto emitQuickIntOpCore = [&](
        runtime::detail::QuickOpcode qop,
        bool checked,
        std::uint32_t idx,
        std::int32_t unexecAfterTrap) -> std::uint8_t {
        using QO = runtime::detail::QuickOpcode;
        switch (qop) {
        case QO::AddInt:
            as.addReg32(X64Reg::RAX, X64Reg::RCX);
            if (checked) {
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.jcc(X64Cond::O, ovfStub);
            }
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            return kTagInt64;
        case QO::SubtractInt:
            as.subReg32(X64Reg::RAX, X64Reg::RCX);
            if (checked) {
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.jcc(X64Cond::O, ovfStub);
            }
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            return kTagInt64;
        case QO::MultiplyInt:
            as.imulReg32(X64Reg::RAX, X64Reg::RCX);
            if (checked) {
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.jcc(X64Cond::O, ovfStub);
            }
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            return kTagInt64;
        case QO::DivideInt: {
            as.testReg32(X64Reg::RCX, X64Reg::RCX);
            const auto div0Stub = addColdStub(ColdTrapStub::Kind::DivisionByZero, idx, unexecAfterTrap);
            as.jcc(X64Cond::E, div0Stub);
            const std::uint32_t doDiv = as.createLabel();
            as.cmpReg32Imm(X64Reg::RAX, std::numeric_limits<std::int32_t>::min());
            as.jcc(X64Cond::NE, doDiv);
            as.cmpReg32Imm(X64Reg::RCX, -1);
            const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
            as.jcc(X64Cond::E, ovfStub);
            as.bind(doDiv);
            as.cdq();
            as.idivReg32(X64Reg::RCX);
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            return kTagInt64;
        }
        case QO::RemainderInt: {
            as.testReg32(X64Reg::RCX, X64Reg::RCX);
            const auto div0Stub = addColdStub(ColdTrapStub::Kind::DivisionByZero, idx, unexecAfterTrap);
            as.jcc(X64Cond::E, div0Stub);
            const std::uint32_t doRem = as.createLabel();
            const std::uint32_t remDone = as.createLabel();
            as.cmpReg32Imm(X64Reg::RAX, std::numeric_limits<std::int32_t>::min());
            as.jcc(X64Cond::NE, doRem);
            as.cmpReg32Imm(X64Reg::RCX, -1);
            as.jcc(X64Cond::NE, doRem);
            as.xorReg32(X64Reg::RAX, X64Reg::RAX);
            as.jmp(remDone);
            as.bind(doRem);
            as.cdq();
            as.idivReg32(X64Reg::RCX);
            as.movsxd64From32(X64Reg::RAX, X64Reg::RDX);
            as.bind(remDone);
            return kTagInt64;
        }
        default: {
            as.cmpReg32(X64Reg::RAX, X64Reg::RCX);
            as.setccAl(quickCmpToCond(qop));
            return kTagBool;
        }
        }
    };

    const auto emitWriteTaggedResult = [&](std::uint16_t slot, std::uint8_t tag) {
        const std::int32_t off = slotOff(slot);
        if (tag == kTagBool) {
            as.storeMem8Reg(X64Reg::RBX, off, X64Reg::RAX);
            as.storeMem8Imm(X64Reg::RBX, off + 15, kTagBool);
        } else {
            as.storeMem64(X64Reg::RBX, off, X64Reg::RAX);
            as.storeMem8Imm(X64Reg::RBX, off + 15, tag);
        }
    };

    const auto emitCondBranchTargets = [&](
        X64Cond trueCc,
        std::uint32_t idx,
        std::uint32_t trueOff,
        std::uint32_t falseOff) {
        if (falseOff == idx + 1u) {
            as.jcc(trueCc, instLabels[trueOff]);
        } else if (trueOff == idx + 1u) {
            as.jcc(invertCond(trueCc), instLabels[falseOff]);
        } else {
            as.jcc(trueCc, instLabels[trueOff]);
            as.jmp(instLabels[falseOff]);
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
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
                as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            }
            break;

        case FO::StoreLocalStructFieldInPlace: {
            const auto targetLeafSlot = static_cast<std::uint32_t>(fi.auxImm32 + fi.aux0Slot);
            if (fi.aux1Slot == 0u) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                // Ensure RHS is not a FrameSlice/traceable Struct needing detach
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::E, slowStub);
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src1Slot));
                as.movupsStore(X64Reg::RBX, static_cast<std::int32_t>(targetLeafSlot * 16u), 0);
                as.bind(doneLbl);
            } else if (const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot);
                       subSc != nullptr && targetLeafSlot + subSc->flatSlotCount <= execFunc.totalFrameSlots) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 13, 0u);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 12, static_cast<std::uint8_t>(fi.aux1Slot & 0xFFu));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 14, static_cast<std::uint8_t>((fi.aux1Slot >> 8u) & 0xFFu));
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src1Slot) + 8);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.orImm32(X64Reg::RCX, 0x40u);
                as.movReg64(X64Reg::R8, X64Reg::RAX);
                as.shrImm32(X64Reg::R8, 24u);
                as.cmpReg32(X64Reg::R8, X64Reg::RCX);
                as.jcc(X64Cond::NE, slowStub);
                as.andImm32(X64Reg::RAX, 0x00FFFFFFu);
                as.loadMem64(X64Reg::R9, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.shlImm64(X64Reg::RAX, 4u);
                as.leaSib1_64(X64Reg::RSI, X64Reg::R9, X64Reg::RAX);
                as.leaDisp64(X64Reg::RDI, X64Reg::RBX, static_cast<std::int32_t>(targetLeafSlot * 16u));
                const std::uint32_t skipCopyLbl = as.createLabel();
                as.cmpReg64(X64Reg::RSI, X64Reg::RDI);
                as.jcc(X64Cond::E, skipCopyLbl);
                if (subSc->flatSlotCount <= 6u) {
                    for (std::uint16_t k = 0; k < subSc->flatSlotCount; ++k) {
                        as.movupsLoad(0, X64Reg::RSI, static_cast<std::int32_t>(k) * 16);
                        as.movupsStore(X64Reg::RDI, static_cast<std::int32_t>(k) * 16, 0);
                    }
                } else {
                    as.movImm32(X64Reg::RCX, static_cast<std::uint32_t>(subSc->flatSlotCount) * 2u);
                    as.repMovsq();
                }
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
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 13, 0u);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.orImm32(X64Reg::RCX, 0x40u);
                as.movReg64(X64Reg::R8, X64Reg::RAX);
                as.shrImm32(X64Reg::R8, 24u);
                as.cmpReg32(X64Reg::R8, X64Reg::RCX);
                as.jcc(X64Cond::NE, slowStub);
                as.andImm32(X64Reg::RAX, 0x00FFFFFFu);
                as.addImm64(X64Reg::RAX, static_cast<std::int32_t>(fi.aux0Slot));
                as.cmpReg32Imm(X64Reg::RAX, static_cast<std::int32_t>(runtime::StructStorageHandle::FrameOffsetMask));
                as.jcc(X64Cond::A, slowStub);
                as.loadMem64(X64Reg::R8, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                if (subSid == 0u) {
                    as.shlImm64(X64Reg::RAX, 4u);
                    as.movupsLoadSib1(0, X64Reg::R8, X64Reg::RAX);
                    as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
                } else {
                    // Zero-copy AliasSlice nested struct header construction!
                    as.shlImm64(X64Reg::RCX, 24u);
                    as.emitRex(false, static_cast<std::uint8_t>(X64Reg::RCX), 0, static_cast<std::uint8_t>(X64Reg::RAX));
                    as.emit8(0x09u); // OR eax, ecx
                    as.emitModRMReg(static_cast<std::uint8_t>(X64Reg::RCX), static_cast<std::uint8_t>(X64Reg::RAX));
                    as.movImm64(X64Reg::RDX, static_cast<std::uint64_t>(fi.integerImmediate));
                    as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot) + 0, X64Reg::RDX);
                    as.storeMem32(X64Reg::RBX, slotOff(fi.dstSlot) + 8, X64Reg::RAX);
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(subSid & 0xFFu));
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 13, 0u);
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((subSid >> 8u) & 0xFFu));
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
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
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 13, 0u);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.orImm32(X64Reg::RCX, 0x40u);
                as.movReg64(X64Reg::R8, X64Reg::RAX);
                as.shrImm32(X64Reg::R8, 24u);
                as.cmpReg32(X64Reg::R8, X64Reg::RCX);
                as.jcc(X64Cond::NE, slowStub);
                as.andImm32(X64Reg::RAX, 0x00FFFFFFu);
                as.loadMem32(X64Reg::R8, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, frameOffset32)));
                as.addImm64(X64Reg::R8, static_cast<std::int32_t>(dstPayOff));
                as.cmpReg32Imm(X64Reg::R8, static_cast<std::int32_t>(runtime::StructStorageHandle::FrameOffsetMask));
                as.jcc(X64Cond::A, slowStub);
                as.loadMem64(X64Reg::RDX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.movReg64(X64Reg::R9, X64Reg::RCX); // save (0x40 | bankId)
                as.shlImm64(X64Reg::RAX, 4u);
                as.leaSib1_64(X64Reg::RSI, X64Reg::RDX, X64Reg::RAX);
                as.leaDisp64(X64Reg::RDI, X64Reg::RBX, static_cast<std::int32_t>(dstPayOff * 16u));
                const std::uint32_t skipCopyLbl = as.createLabel();
                as.cmpReg64(X64Reg::RSI, X64Reg::RDI);
                as.jcc(X64Cond::E, skipCopyLbl);
                if (sc->flatSlotCount <= 6u) {
                    for (std::uint16_t k = 0; k < sc->flatSlotCount; ++k) {
                        as.movupsLoad(0, X64Reg::RSI, static_cast<std::int32_t>(k) * 16);
                        as.movupsStore(X64Reg::RDI, static_cast<std::int32_t>(k) * 16, 0);
                    }
                } else {
                    as.movImm32(X64Reg::RCX, static_cast<std::uint32_t>(sc->flatSlotCount) * 2u);
                    as.repMovsq();
                }
                as.bind(skipCopyLbl);
                as.shlImm64(X64Reg::R9, 24u);
                as.movReg64(X64Reg::RAX, X64Reg::R8);
                as.emitRex(false, static_cast<std::uint8_t>(X64Reg::R9), 0, static_cast<std::uint8_t>(X64Reg::RAX));
                as.emit8(0x09u); // OR eax, r9d
                as.emitModRMReg(static_cast<std::uint8_t>(X64Reg::R9), static_cast<std::uint8_t>(X64Reg::RAX));
                as.movImm64(X64Reg::RDX, static_cast<std::uint64_t>(sc->structTypeId));
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot) + 0, X64Reg::RDX);
                as.storeMem32(X64Reg::RBX, slotOff(fi.dstSlot) + 8, X64Reg::RAX);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 13, 0u);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
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
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                as.cmpReg32Imm(X64Reg::RAX, static_cast<std::int32_t>(runtime::ObjectRef::StackSlotRefHeapId));
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.cmpReg32Mem(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src0Slot) + 4);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::E, slowStub);
                as.loadMem64(X64Reg::R8, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 0);
                as.addImm64(X64Reg::RAX, static_cast<std::int32_t>(fi.aux0Slot));
                as.shlImm64(X64Reg::RAX, 4u);
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src1Slot));
                as.movupsStoreSib1(X64Reg::R8, X64Reg::RAX, 0);
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
            as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.movupsStore(X64Reg::RBX, slotOff(fi.storeSlot), 0);
            break;

        case FO::SuperMoveParamStoreLocalElided:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movupsStore(X64Reg::RBX, slotOff(fi.storeSlot), 0);
            break;

        case FO::ConstantInt:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem64Imm32SignExt(
                X64Reg::RBX, slotOff(fi.dstSlot), static_cast<std::int32_t>(fi.integerImmediate));
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagInt64);
            break;

        case FO::ConstantLong:
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fitsInt32(fi.integerImmediate)) {
                as.storeMem64Imm32SignExt(
                    X64Reg::RBX, slotOff(fi.dstSlot), static_cast<std::int32_t>(fi.integerImmediate));
            } else {
                as.movImm64(X64Reg::RAX, static_cast<std::uint64_t>(fi.integerImmediate));
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RAX);
            }
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagLong);
            break;

        case FO::ConstantBool:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot), fi.integerImmediate != 0 ? 1u : 0u);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagBool);
            break;

        case FO::ConstantDouble: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            const auto uBits = static_cast<std::uint64_t>(fi.integerImmediate);
            if (uBits == 0u) {
                as.storeMem64Imm32SignExt(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            } else {
                as.movImm64(X64Reg::RAX, uBits);
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RAX);
            }
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;
        }

        case FO::SuperConstFloat: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            std::uint32_t fBits = 0;
            std::memcpy(&fBits, &fi.integerImmediate, 4);
            as.storeMem32Imm(X64Reg::RBX, slotOff(fi.dstSlot), fBits);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::SuperConstFloatStoreLocal: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            std::uint32_t fBits = 0;
            std::memcpy(&fBits, &fi.integerImmediate, 4);
            as.storeMem32Imm(X64Reg::RBX, slotOff(fi.storeSlot), fBits);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.storeSlot) + 15, kTagFloat);
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
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem32(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            const std::uint8_t outTag = emitQuickIntOpCore(qop, isChecked, i32Idx, unexecAfter);
            emitWriteTaggedResult(fi.dstSlot, outTag);
            break;
        }

        case FO::NegateIntUnchecked:
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.negReg32(X64Reg::RAX);
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;

        case FO::LogicalNot:
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagBool, i32Idx, unexecAfter);
            as.loadMem8ZeroExt(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.emit8(0x34u); // xor al, 1
            as.emit8(0x01u);
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
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            if (!isElided && !isJump) {
                as.movsxd64From32(X64Reg::RDX, X64Reg::RAX);
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RDX);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagInt64);
                as.storeMem64Imm32SignExt(
                    X64Reg::RBX, slotOff(fi.aux0Slot), static_cast<std::int32_t>(fi.integerImmediate));
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux0Slot) + 15, kTagInt64);
            }
            as.movImm32(X64Reg::RCX, static_cast<std::uint32_t>(static_cast<std::int32_t>(fi.integerImmediate)));
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, unexecAfter);
            if (isJump) {
                emitWriteTaggedResult(fi.dstSlot, outTag);
                const std::int32_t brInc =
                    ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2 : 1;
                as.addImm64(X64Reg::R14, brInc);
                as.jmp(instLabels[fi.trueTargetOffset]);
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
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem32(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            if (!isElided && !isJump) {
                as.movsxd64From32(X64Reg::RDX, X64Reg::RAX);
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RDX);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagInt64);
                if (isLocalRhs) {
                    as.movsxd64From32(X64Reg::RDX, X64Reg::RCX);
                    as.storeMem64(X64Reg::RBX, slotOff(fi.aux0Slot), X64Reg::RDX);
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux0Slot) + 15, kTagInt64);
                }
            }
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, unexecAfter);
            if (isJump) {
                emitWriteTaggedResult(fi.dstSlot, outTag);
                const std::int32_t brInc =
                    ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2 : 1;
                as.addImm64(X64Reg::R14, brInc);
                as.jmp(instLabels[fi.trueTargetOffset]);
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
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem32(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            if (!isElided) {
                as.movsxd64From32(X64Reg::RDX, X64Reg::RAX);
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RDX);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagInt64);
            }
            const std::uint8_t midTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, 2);
            if (!isElided) {
                emitWriteTaggedResult(fi.aux0Slot, midTag);
            }
            emitCheckTagOrTrap(fi.src2Slot, kTagInt64, i32Idx, 1);
            as.loadMem32(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src2Slot));
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
            as.inc64(X64Reg::R14);
            as.cmpMem64Imm32(
                X64Reg::RBX, slotOff(fi.src0Slot), static_cast<std::int32_t>(fi.integerImmediate));
            emitCondBranchTargets(
                quickCmpToCond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
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
            as.inc64(X64Reg::R14);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src1Slot));
            emitCondBranchTargets(
                quickCmpToCond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
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

            // Verify lhs is Int64 & array hits inline cache
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagInt64);
            as.jcc(X64Cond::NE, slowStub);
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagObjectRef);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src1Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src1Slot) + 8);
            as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
            as.jcc(X64Cond::NE, slowStub);

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.inc64(X64Reg::R14);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem32(X64Reg::RCX, X64Reg::R12, cacheDisp + 24); // zero-extended uint32 length
            as.cmpReg64(X64Reg::RAX, X64Reg::RCX);
            emitCondBranchTargets(
                quickCmpToCond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::ArrayLengthFast: {
            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLengthSlow, i32Idx, 0, doneLbl);

            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
            as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
            as.jcc(X64Cond::NE, slowStub);

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem32(X64Reg::RAX, X64Reg::R12, cacheDisp + 24);
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

                        // 1. Inline FlatStruct[] (isPacked == 2u) cache hit check
                        as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
                        as.jcc(X64Cond::NE, slowStub);
                        as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
                        as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
                        as.jcc(X64Cond::NE, slowStub);
                        as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                        as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
                        as.jcc(X64Cond::NE, slowStub);
                        as.cmpMem8Imm(X64Reg::R12, cacheDisp + 13, 2u);
                        as.jcc(X64Cond::NE, slowStub);
                        as.cmpMem16Imm(X64Reg::R12, cacheDisp + 30, sid);
                        as.jcc(X64Cond::NE, slowStub);

                        // 2. Index validation & bounds check
                        as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagInt64);
                        as.jcc(X64Cond::NE, slowStub);
                        as.loadMem64(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
                        as.loadMem32(X64Reg::RDX, X64Reg::R12, cacheDisp + 24);
                        as.cmpReg64(X64Reg::RCX, X64Reg::RDX);
                        as.jcc(X64Cond::AE, slowStub);

                        // 3. Direct hardware slice copy from FlatStruct[] buffer -> flat frame payload!
                        emitBudgetDeduct(i32Idx, fi.span, false);
                        as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16); // Value* data
                        as.imulImm32_64(X64Reg::RAX, X64Reg::RCX, static_cast<std::int32_t>(sc->flatSlotCount) * 16);
                        as.leaSib1_64(X64Reg::RSI, X64Reg::RDX, X64Reg::RAX);
                        as.leaDisp64(X64Reg::RDI, X64Reg::RBX, static_cast<std::int32_t>(payOff) * 16);
                        if (sc->flatSlotCount <= 6u) {
                            for (std::uint16_t k = 0; k < sc->flatSlotCount; ++k) {
                                as.movupsLoad(0, X64Reg::RSI, static_cast<std::int32_t>(k) * 16);
                                as.movupsStore(X64Reg::RDI, static_cast<std::int32_t>(k) * 16, 0);
                            }
                        } else {
                            as.movImm32(X64Reg::RCX, static_cast<std::uint32_t>(sc->flatSlotCount) * 2u);
                            as.repMovsq();
                        }
                        as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                        as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                        as.orImm32(X64Reg::RCX, 0x40u);
                        as.shlImm64(X64Reg::RCX, 24u);
                        as.loadMem32(X64Reg::RAX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, frameOffset32)));
                        if (payOff != 0u) {
                            as.addImm64(X64Reg::RAX, static_cast<std::int32_t>(payOff));
                        }
                        as.emitRex(false, static_cast<std::uint8_t>(X64Reg::RCX), 0, static_cast<std::uint8_t>(X64Reg::RAX));
                        as.emit8(0x09u); // OR eax, ecx
                        as.emitModRMReg(static_cast<std::uint8_t>(X64Reg::RCX), static_cast<std::uint8_t>(X64Reg::RAX));
                        as.movImm64(X64Reg::RDX, static_cast<std::uint64_t>(elemTid));
                        as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot) + 0, X64Reg::RDX);
                        as.storeMem32(X64Reg::RBX, slotOff(fi.dstSlot) + 8, X64Reg::RAX);
                        as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                        as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 13, 0u);
                        as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                        as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
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

            // 1. Inline PackedArrayFastView cache hit check
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
            as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
            as.jcc(X64Cond::NE, slowStub);
            as.cmpMem8Imm(X64Reg::R12, cacheDisp + 14, fastEncoding);
            as.jcc(X64Cond::NE, slowStub);

            // 2. Index validation & bounds check (unsigned cmp handles both < 0 and >= length)
            const bool noBce = (fi.arithFlags & runtime::detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u;
            const bool trustedIdx = !isTyped &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            if (!noBce && !trustedIdx) {
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagInt64);
                as.jcc(X64Cond::NE, slowStub);
            }
            as.loadMem64(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            if (!noBce) {
                as.loadMem32(X64Reg::RDX, X64Reg::R12, cacheDisp + 24);
                as.cmpReg64(X64Reg::RCX, X64Reg::RDX);
                as.jcc(X64Cond::AE, slowStub);
            }

            // 3. Exact budget deduction + direct unboxed payload read
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16); // void* rawData
            if (fastEncoding == kPackedInt32) {
                // movsxd rax, dword ptr [rdx + rcx*4]
                as.emitRex(true, 0, 0, 0);
                as.emit8(0x63u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 2);
                emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            } else if (fastEncoding == kPackedFloat64) {
                // movsd xmm0, qword ptr [rdx + rcx*8]
                as.emit8(0xF2u);
                as.emit8(0x0Fu);
                as.emit8(0x10u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 3);
                as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            } else {
                // movzx eax, byte ptr [rdx + rcx*1]
                as.emit8(0x0Fu);
                as.emit8(0xB6u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 0);
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

                // 1. Inline FlatStruct[] (isPacked == 2u) cache hit check
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
                as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::R12, cacheDisp + 13, 2u);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem16Imm(X64Reg::R12, cacheDisp + 30, rootSid);
                as.jcc(X64Cond::NE, slowStub);

                // 2. Index validation & bounds check
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagInt64);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem64(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
                as.loadMem32(X64Reg::RDX, X64Reg::R12, cacheDisp + 24);
                as.cmpReg64(X64Reg::RCX, X64Reg::RDX);
                as.jcc(X64Cond::AE, slowStub);

                // 3. Ensure RHS is a non-traceable, non-pool, non-FrameSlice scalar (1 <= tag < FirstTraceable=23)
                as.loadMem8ZeroExt(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src2Slot) + 15);
                as.testReg32(X64Reg::RAX, X64Reg::RAX);
                as.jcc(X64Cond::E, slowStub);
                as.cmpReg32Imm(X64Reg::RAX, static_cast<std::int32_t>(runtime::ValueTag::FirstTraceable));
                as.jcc(X64Cond::AE, slowStub);

                // 4. Direct inline 128-bit XMM leaf store into FlatStruct[] element slot!
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16); // Value* data
                as.imulImm32_64(X64Reg::RAX, X64Reg::RCX, static_cast<std::int32_t>(rootSc->flatSlotCount));
                as.addImm64(X64Reg::RAX, static_cast<std::int32_t>(fi.aux0Slot));
                as.shlImm64(X64Reg::RAX, 4u);
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src2Slot));
                as.movupsStoreSib1(X64Reg::RDX, X64Reg::RAX, 0);
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

            // 1. Inline PackedArrayFastView cache hit check
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
            as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
            as.jcc(X64Cond::NE, slowStub);
            as.cmpMem8Imm(X64Reg::R12, cacheDisp + 14, fastEncoding);
            as.jcc(X64Cond::NE, slowStub);

            // 2. Index validation & bounds check
            const bool noBce = (fi.arithFlags & runtime::detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u;
            const bool trustedIdx = !isPrim &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            const bool trustedVal = !isPrim &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc1) != 0u;
            if (!noBce && !trustedIdx) {
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagInt64);
                as.jcc(X64Cond::NE, slowStub);
            }
            as.loadMem64(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            if (!noBce) {
                as.loadMem32(X64Reg::RDX, X64Reg::R12, cacheDisp + 24);
                as.cmpReg64(X64Reg::RCX, X64Reg::RDX);
                as.jcc(X64Cond::AE, slowStub);
            }

            // 3. Value tag & range check before mutating rawData
            if (fastEncoding == kPackedInt32) {
                if (!trustedVal) {
                    as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src2Slot) + 15, kTagInt64);
                    as.jcc(X64Cond::NE, slowStub);
                }
                as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src2Slot));
                as.movsxd64From32(X64Reg::R8, X64Reg::RAX);
                as.cmpReg64(X64Reg::RAX, X64Reg::R8);
                as.jcc(X64Cond::NE, slowStub); // >32-bit host int64 widens via slowStub!

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16);
                // mov dword ptr [rdx + rcx*4], eax
                as.emit8(0x89u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 2);
            } else if (fastEncoding == kPackedFloat64) {
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src2Slot) + 15, kTagDouble);
                as.jcc(X64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src2Slot));
                as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16);
                // movsd qword ptr [rdx + rcx*8], xmm0
                as.emit8(0xF2u);
                as.emit8(0x0Fu);
                as.emit8(0x11u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 3);
            } else {
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src2Slot) + 15, kTagBool);
                as.jcc(X64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem8ZeroExt(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src2Slot));
                as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16);
                // mov byte ptr [rdx + rcx*1], al
                as.emit8(0x88u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 0);
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
            as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movsdLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            std::uint8_t sseOp = 0x58u;
            if (fi.flatOpcode == FO::SubDouble || fi.flatOpcode == FO::SubDoubleTrusted) sseOp = 0x5Cu;
            else if (fi.flatOpcode == FO::MulDouble || fi.flatOpcode == FO::MulDoubleTrusted) sseOp = 0x59u;
            else if (fi.flatOpcode == FO::DivDouble || fi.flatOpcode == FO::DivDoubleTrusted) sseOp = 0x5Eu;
            as.binarySd(sseOp, 0, 1);
            as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;
        }

        case FO::LessDouble:
        case FO::LessOrEqualDouble:
        case FO::GreaterDouble:
        case FO::GreaterOrEqualDouble: {
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movsdLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::LessDouble) {
                as.ucomisd(1, 0);
                as.setccAl(X64Cond::A);
            } else if (fi.flatOpcode == FO::LessOrEqualDouble) {
                as.ucomisd(1, 0);
                as.setccAl(X64Cond::AE);
            } else if (fi.flatOpcode == FO::GreaterDouble) {
                as.ucomisd(0, 1);
                as.setccAl(X64Cond::A);
            } else {
                as.ucomisd(0, 1);
                as.setccAl(X64Cond::AE);
            }
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;
        }

        case FO::NegateDouble:
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movImm64(X64Reg::RCX, 0x8000000000000000ULL);
            as.emitRex(true, static_cast<std::uint8_t>(X64Reg::RCX), 0, static_cast<std::uint8_t>(X64Reg::RAX));
            as.emit8(0x31u); // xor rax, rcx
            as.emitModRMReg(static_cast<std::uint8_t>(X64Reg::RCX), static_cast<std::uint8_t>(X64Reg::RAX));
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
            as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            if (isConst) {
                as.movImm64(X64Reg::RAX, static_cast<std::uint64_t>(fi.integerImmediate));
                as.movqToXmm(1, X64Reg::RAX);
            } else {
                emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, 1);
                as.movsdLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            }
            if (!isElided) {
                as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
                as.movsdStore(X64Reg::RBX, slotOff(fi.aux0Slot), 1);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux0Slot) + 15, kTagDouble);
            }
            std::uint8_t sseOp = 0x58u;
            if (fi.binaryOpcode1 == QO::SubtractDouble) sseOp = 0x5Cu;
            else if (fi.binaryOpcode1 == QO::MultiplyDouble) sseOp = 0x59u;
            else if (fi.binaryOpcode1 == QO::DivideDouble) sseOp = 0x5Eu;
            as.binarySd(sseOp, 0, 1);
            if (!isElided) {
                as.movsdStore(X64Reg::RBX, slotOff(fi.aux1Slot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux1Slot) + 15, kTagDouble);
            }
            as.movsdStore(X64Reg::RBX, slotOff(fi.storeSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.storeSlot) + 15, kTagDouble);
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
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movssLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            std::uint8_t sseOp = 0x58u;
            if (fi.flatOpcode == FO::SubFloat || fi.flatOpcode == FO::SubFloatTrusted) sseOp = 0x5Cu;
            else if (fi.flatOpcode == FO::MulFloat || fi.flatOpcode == FO::MulFloatTrusted) sseOp = 0x59u;
            else if (fi.flatOpcode == FO::DivFloat) sseOp = 0x5Eu;
            as.binarySs(sseOp, 0, 1);
            as.movssStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
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
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movssLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::LessFloat) {
                as.ucomiss(1, 0);
                as.setccAl(X64Cond::A);
            } else if (fi.flatOpcode == FO::LessOrEqualFloat) {
                as.ucomiss(1, 0);
                as.setccAl(X64Cond::AE);
            } else if (fi.flatOpcode == FO::GreaterFloat) {
                as.ucomiss(0, 1);
                as.setccAl(X64Cond::A);
            } else if (fi.flatOpcode == FO::GreaterOrEqualFloat) {
                as.ucomiss(0, 1);
                as.setccAl(X64Cond::AE);
            } else if (fi.flatOpcode == FO::EqualFloat) {
                as.ucomiss(0, 1);
                as.setccAl(X64Cond::E);
                as.setccCl(X64Cond::NP);
                as.emit8(0x20u); // and al, cl
                as.emit8(0xC8u);
            } else {
                as.ucomiss(0, 1);
                as.setccAl(X64Cond::NE);
                as.setccCl(X64Cond::P);
                as.emit8(0x08u); // or al, cl
                as.emit8(0xC8u);
            }
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;
        }

        case FO::NegateFloat:
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.emit8(0x35u); // xor eax, 0x80000000
            as.emit32(0x80000000u);
            as.storeMem32(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RAX);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
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
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movssLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
                as.movssStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
                if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore) {
                    as.movssStore(X64Reg::RBX, slotOff(fi.aux0Slot), 1);
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux0Slot) + 15, kTagFloat);
                }
            }
            std::uint8_t sseOp = 0x58u;
            if (fi.binaryOpcode1 == QO::SubtractFloat) sseOp = 0x5Cu;
            else if (fi.binaryOpcode1 == QO::MultiplyFloat) sseOp = 0x59u;
            else if (fi.binaryOpcode1 == QO::DivideFloat) sseOp = 0x5Eu;
            as.binarySs(sseOp, 0, 1);
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegElided ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
                as.movssStore(X64Reg::RBX, slotOff(fi.aux1Slot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux1Slot) + 15, kTagFloat);
            }
            if (fi.flatOpcode != FO::SuperBinaryLocalFloatReg &&
                fi.flatOpcode != FO::SuperBinaryLocalFloatRegElided) {
                as.movssStore(X64Reg::RBX, slotOff(fi.storeSlot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.storeSlot) + 15, kTagFloat);
            }
            break;
        }

        // Fast Scalar Conversions
        case FO::ConvertIntToLongFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            emitWriteTaggedResult(fi.dstSlot, kTagLong);
            break;

        case FO::ConvertIntToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtsi2sd64(0, X64Reg::RAX);
            as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertLongToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagLong, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtsi2sd64(0, X64Reg::RAX);
            as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertIntToFloatFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtsi2ss64(0, X64Reg::RAX);
            as.movssStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;

        case FO::ConvertFloatToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtss2sd(0, 0);
            as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertDoubleToFloatFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.checked1() && fi.span > 1u);
            as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            if (fi.checked1()) {
                constexpr double kMaxF = static_cast<double>(std::numeric_limits<float>::max());
                constexpr double kMinF = -kMaxF;
                std::uint64_t maxBits = 0, minBits = 0;
                std::memcpy(&maxBits, &kMaxF, 8);
                std::memcpy(&minBits, &kMinF, 8);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
                as.movImm64(X64Reg::RAX, minBits);
                as.movqToXmm(1, X64Reg::RAX);
                as.ucomisd(0, 1);
                as.jcc(X64Cond::P, ovfStub);
                as.jcc(X64Cond::B, ovfStub);
                as.movImm64(X64Reg::RAX, maxBits);
                as.movqToXmm(1, X64Reg::RAX);
                as.ucomisd(1, 0);
                as.jcc(X64Cond::B, ovfStub);
            }
            as.cvtsd2ss(0, 0);
            as.movssStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::ConvertFloatToIntFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.span > 1u);
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtss2sd(0, 0);
            constexpr double kMinI = static_cast<double>(std::numeric_limits<std::int32_t>::min());
            constexpr double kMaxI = static_cast<double>(std::numeric_limits<std::int32_t>::max());
            std::uint64_t minBits = 0, maxBits = 0;
            std::memcpy(&minBits, &kMinI, 8);
            std::memcpy(&maxBits, &kMaxI, 8);
            const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
            as.movImm64(X64Reg::RAX, minBits);
            as.movqToXmm(1, X64Reg::RAX);
            as.ucomisd(0, 1);
            as.jcc(X64Cond::P, ovfStub);
            as.jcc(X64Cond::B, ovfStub);
            as.movImm64(X64Reg::RAX, maxBits);
            as.movqToXmm(1, X64Reg::RAX);
            as.ucomisd(1, 0);
            as.jcc(X64Cond::B, ovfStub);
            // cvttsd2si rax, xmm0
            as.emit8(0xF2u);
            as.emitRex(true, 0, 0, 0);
            as.emit8(0x0Fu);
            as.emit8(0x2Cu);
            as.emitModRMReg(0, 0);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;
        }

        case FO::ConvertLongToIntFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagLong, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.checked1() && fi.span > 1u);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movsxd64From32(X64Reg::RCX, X64Reg::RAX);
            if (fi.checked1()) {
                as.cmpReg64(X64Reg::RAX, X64Reg::RCX);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
                as.jcc(X64Cond::NE, ovfStub);
            }
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;
        }

        // Unfolded Leaf Function Inlining Opcodes
        case FO::InlineCallEnter: {
            emitHelperCallInline(&jitInlineCallEnterHelper, i32Idx);
            const std::size_t fastTarget = idx + 1u + fi.inlineArgCount;
            if (fi.inlineArgCount > 0u && fastTarget < nInst) {
                as.loadMem32(
                    X64Reg::RAX,
                    X64Reg::R12,
                    static_cast<std::int32_t>(offsetof(JitAbiContext, nextFlatOffsetOut)));
                as.testReg32(X64Reg::RAX, X64Reg::RAX);
                as.jcc(X64Cond::NE, instLabels[fastTarget]);
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
                as.xorpsSelf(0);
                for (std::uint32_t k = 0; k < clearCount; ++k) {
                    as.movupsStore(X64Reg::RBX, slotOff(static_cast<std::uint16_t>(clearBase + k)), 0);
                }
            }
            if (fi.flatOpcode == FO::InlineReturnVoidJump) {
                as.jmp(instLabels[fi.trueTargetOffset]);
            }
            break;
        }

        case FO::InlineReturnScalar:
        case FO::InlineReturnScalarJump: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            }
            if (fi.integerImmediate != 0) {
                const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                const auto clearBase = static_cast<std::uint16_t>(packed & 0xFFFFu);
                const auto clearCount = static_cast<std::uint32_t>(packed >> 16u);
                as.xorpsSelf(1);
                for (std::uint32_t k = 0; k < clearCount; ++k) {
                    as.movupsStore(X64Reg::RBX, slotOff(static_cast<std::uint16_t>(clearBase + k)), 1);
                }
            }
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            }
            if (fi.flatOpcode == FO::InlineReturnScalarJump) {
                as.jmp(instLabels[fi.trueTargetOffset]);
            }
            break;
        }

        // Terminators
        case FO::TermJumpNoArgs:
            emitBudgetDeduct(i32Idx, fi.span, fi.span > 1u);
            as.addImm64(X64Reg::R14, static_cast<std::int32_t>(fi.span));
            if (fi.trueTargetOffset != idx + 1u) {
                as.jmp(instLabels[fi.trueTargetOffset]);
            }
            break;

        case FO::TermBranchBoolNoArgs:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.inc64(X64Reg::R14);
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot), 0u);
            emitCondBranchTargets(X64Cond::NE, i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
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
                        as.movupsLoad(0, X64Reg::RBX, slotOff(sSlot));
                        as.movupsStore(X64Reg::RBX, slotOff(dSlot), 0);
                    }
                    return true;
                }
                for (std::size_t k = 0; k < srcs.size(); ++k) {
                    const auto sSlot = static_cast<std::uint16_t>(execFunc.registerOffset + srcs[k]);
                    const auto scSlot = static_cast<std::uint16_t>(execFunc.scratchOffset + k);
                    as.movupsLoad(0, X64Reg::RBX, slotOff(sSlot));
                    as.movupsStore(X64Reg::RBX, slotOff(scSlot), 0);
                }
                for (std::size_t k = 0; k < srcs.size(); ++k) {
                    const auto scSlot = static_cast<std::uint16_t>(execFunc.scratchOffset + k);
                    const auto dSlot = static_cast<std::uint16_t>(execFunc.registerOffset + dsts[k].target);
                    as.movupsLoad(0, X64Reg::RBX, slotOff(scSlot));
                    as.movupsStore(X64Reg::RBX, slotOff(dSlot), 0);
                }
                return true;
            };

            emitBudgetDeduct(i32Idx, fi.span, false);
            if (term.kind == bytecode::TerminatorKind::Jump) {
                as.inc64(X64Reg::R14);
                if (!emitEdgeCopies(blk.branch)) return {};
                if (fi.trueTargetOffset != idx + 1u) {
                    as.jmp(instLabels[fi.trueTargetOffset]);
                }
            } else if (term.kind == bytecode::TerminatorKind::Branch) {
                const auto condSlot = static_cast<std::uint16_t>(execFunc.registerOffset + term.condition);
                if (condSlot >= execFunc.totalFrameSlots ||
                    fi.falseTargetOffset >= nInst) {
                    return {};
                }
                emitCheckTagOrTrap(condSlot, kTagBool, i32Idx, 0);
                as.inc64(X64Reg::R14);
                const std::uint32_t falsePathLbl = as.createLabel();
                as.cmpMem8Imm(X64Reg::RBX, slotOff(condSlot), 0u);
                as.jcc(X64Cond::E, falsePathLbl);
                if (!emitEdgeCopies(blk.branch)) return {};
                as.jmp(instLabels[fi.trueTargetOffset]);
                as.bind(falsePathLbl);
                if (!emitEdgeCopies(blk.falseBranch)) return {};
                if (fi.falseTargetOffset != idx + 1u) {
                    as.jmp(instLabels[fi.falseTargetOffset]);
                }
            } else {
                return {};
            }
            break;
        }

        case FO::TermReturnVoid:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.xorpsSelf(0);
            as.movupsStore(X64Reg::RAX, 0, 0);
            as.jmp(epilogueOk);
            break;

        case FO::TermReturnScalarFast:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem64(X64Reg::RAX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.movupsStore(X64Reg::RAX, 0, 0);
            as.jmp(epilogueOk);
            break;

        case FO::TermReturnGeneric: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            const auto& cold = execFunc.flatCold[idx];
            const auto retReg = execFunc.blocks[cold.blockIndex].bytecode->terminator.value;
            const auto retSlot = static_cast<std::uint16_t>(execFunc.registerOffset + retReg);
            as.movupsLoad(0, X64Reg::RBX, slotOff(retSlot));
            as.loadMem64(X64Reg::RAX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.movupsStore(X64Reg::RAX, 0, 0);
            as.jmp(epilogueOk);
            break;
        }

        default:
            return {};
        }
    }

    // ========================================================================
    // Out-of-line Cold Stubs (keeps hot loop I-cache footprint maximally dense)
    // ========================================================================
    for (const auto& stub : coldStubs) {
        as.bind(stub.stubLabel);
        switch (stub.kind) {
        case ColdTrapStub::Kind::BudgetSimple:
            as.storeMem32Imm(
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.jmp(epilogueBudget);
            break;

        case ColdTrapStub::Kind::BudgetPartialSuper:
            as.addImm64(X64Reg::R13, stub.restoreBudget);
            emitHelperCallInline(&jitPartialSuperBudgetHelper, stub.flatIdx);
            as.jmp(epilogueBudget);
            break;

        case ColdTrapStub::Kind::IntegerOverflow:
            if (stub.restoreBudget != 0) as.addImm64(X64Reg::R13, stub.restoreBudget);
            as.storeMem32Imm(
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.jmp(epilogueOverflow);
            break;

        case ColdTrapStub::Kind::DivisionByZero:
            if (stub.restoreBudget != 0) as.addImm64(X64Reg::R13, stub.restoreBudget);
            as.storeMem32Imm(
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.jmp(epilogueDivZero);
            break;

        case ColdTrapStub::Kind::TypeMismatch:
            if (stub.restoreBudget != 0) as.addImm64(X64Reg::R13, stub.restoreBudget);
            as.storeMem32Imm(
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.jmp(epilogueTypeErr);
            break;

        case ColdTrapStub::Kind::ArrayLengthSlow:
            emitHelperCallInline(&jitArrayLengthHelper, stub.flatIdx);
            as.jmp(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayLengthBranchSlow:
            emitHelperCallInline(&jitArrayLengthHelper, stub.flatIdx);
            as.loadMem32(
                X64Reg::RAX,
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, branchCondOut)));
            as.testReg32(X64Reg::RAX, X64Reg::RAX);
            as.jcc(X64Cond::NE, stub.resumeLabel);
            as.jmp(stub.altResumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayLoadSlow:
            emitHelperCallInline(&jitLoadElementHelper, stub.flatIdx);
            as.jmp(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayStoreSlow:
            emitHelperCallInline(&jitStoreElementHelper, stub.flatIdx);
            as.jmp(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::FlatStructSlow:
            emitHelperCallInline(&jitFlatStructAndStackRefHelper, stub.flatIdx);
            as.jmp(stub.resumeLabel);
            break;
        }
    }

    // Shared Exit Epilogues
    as.bind(epilogueOk);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.xorReg32(X64Reg::RAX, X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueBudget);
    as.xorReg32(X64Reg::R13, X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.movImm32(X64Reg::RAX, static_cast<std::uint32_t>(runtime::ErrorCode::InstructionBudgetExceeded));
    as.storeMem32(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueOverflow);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.movImm32(X64Reg::RAX, static_cast<std::uint32_t>(runtime::ErrorCode::IntegerOverflow));
    as.storeMem32(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueDivZero);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.movImm32(X64Reg::RAX, static_cast<std::uint32_t>(runtime::ErrorCode::DivisionByZero));
    as.storeMem32(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueTypeErr);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.movImm32(X64Reg::RAX, static_cast<std::uint32_t>(runtime::ErrorCode::TypeMismatch));
    as.storeMem32(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueHelperErr);
    // Helper already synced remainingBudget/branchesTaken and populated session.error; EAX has ErrorCode

    as.bind(sharedPopRet);
    as.addImm64(X64Reg::RSP, 40);
    as.pop(X64Reg::R15);
    as.pop(X64Reg::R14);
    as.pop(X64Reg::R13);
    as.pop(X64Reg::R12);
    as.pop(X64Reg::RDI);
    as.pop(X64Reg::RSI);
    as.pop(X64Reg::RBX);
    as.pop(X64Reg::RBP);
    as.ret();

    if (!as.allLabelsBound()) return {};
    return as.bytes();
}

// ============================================================================

} // namespace realscript::jit::jit_detail

#endif // UINTPTR_MAX == 0xffffffffffffffffULL
