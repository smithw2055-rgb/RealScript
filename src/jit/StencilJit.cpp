#include "JitInternal.h"

#include <chrono>

namespace realscript::jit {
using namespace jit_detail;

namespace {

thread_local StencilTlsScratch g_stencilTlsScratch;

// P2-A Tier 1: Pre-Bound Direct-Threaded C++ Stencil Thunks (Portable Fallback)
// ============================================================================

[[nodiscard]] runtime::Value& slotAt(JitAbiContext* ctx, std::uint32_t byteOff) noexcept {
    return *reinterpret_cast<runtime::Value*>(
        reinterpret_cast<std::uint8_t*>(ctx->frameBase) + byteOff);
}

[[nodiscard]] StencilStepStatus failThreadedBudget(
    JitAbiContext* ctx,
    const ThreadedStencilOp* op) noexcept {
    if (op->span > 1u) {
        (void)jitPartialSuperBudgetHelper(ctx, op->flatIdx);
        return StencilStepStatus::Error;
    }
    ctx->remainingBudget = 0;
    (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    return StencilStepStatus::Error;
}

[[nodiscard]] bool evalQuickIntStencil(
    JitAbiContext* ctx,
    const ThreadedStencilOp* op,
    runtime::detail::QuickOpcode qop,
    bool checked,
    std::int64_t lhs,
    std::int64_t rhs,
    std::uint8_t unexecAfter,
    runtime::Value& outVal) noexcept {
    using QO = runtime::detail::QuickOpcode;
    switch (qop) {
    case QO::AddInt: {
        const std::int64_t r = lhs + rhs;
        if (checked && !fitsInt32(r)) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::IntegerOverflow, "integer overflow");
            return false;
        }
        outVal.storeInt64(checked ? r : wrapInt32Val(r));
        return true;
    }
    case QO::SubtractInt: {
        const std::int64_t r = lhs - rhs;
        if (checked && !fitsInt32(r)) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::IntegerOverflow, "integer overflow");
            return false;
        }
        outVal.storeInt64(checked ? r : wrapInt32Val(r));
        return true;
    }
    case QO::MultiplyInt: {
        const std::int64_t r = lhs * rhs;
        if (checked && !fitsInt32(r)) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::IntegerOverflow, "integer overflow");
            return false;
        }
        outVal.storeInt64(checked ? r : wrapInt32Val(r));
        return true;
    }
    case QO::DivideInt:
        if (rhs == 0) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::DivisionByZero, "division by zero");
            return false;
        }
        if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::IntegerOverflow, "integer division overflow");
            return false;
        }
        outVal.storeInt64(lhs / rhs);
        return true;
    case QO::RemainderInt:
        if (rhs == 0) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::DivisionByZero, "remainder by zero");
            return false;
        }
        outVal.storeInt64((lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) ? 0 : (lhs % rhs));
        return true;
    case QO::LessInt: outVal.storeBool(lhs < rhs); return true;
    case QO::LessOrEqualInt: outVal.storeBool(lhs <= rhs); return true;
    case QO::GreaterInt: outVal.storeBool(lhs > rhs); return true;
    case QO::GreaterOrEqualInt: outVal.storeBool(lhs >= rhs); return true;
    case QO::EqualInt: outVal.storeBool(lhs == rhs); return true;
    case QO::NotEqualInt: outVal.storeBool(lhs != rhs); return true;
    default:
        (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::InvalidProgram, "invalid quick int op");
        return false;
    }
}

StencilStepStatus th_MoveSlot(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    slotAt(ctx, pc->dstOff) = slotAt(ctx, pc->src0Off);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_SuperMoveParamStore(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    const runtime::Value v = slotAt(ctx, pc->src0Off);
    if (pc->auxByte != 0u) slotAt(ctx, pc->dstOff) = v;
    slotAt(ctx, pc->storeOff) = v;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstInt(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    slotAt(ctx, pc->dstOff).storeInt64(pc->imm64);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstLong(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    slotAt(ctx, pc->dstOff).storeLong(pc->imm64);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstBool(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    slotAt(ctx, pc->dstOff).storeBool(pc->imm64 != 0);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstDouble(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    double d = 0.0;
    std::memcpy(&d, &pc->imm64, sizeof(double));
    slotAt(ctx, pc->dstOff).storeDouble(d);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstFloat(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    float f = 0.0f;
    std::memcpy(&f, &pc->imm64, sizeof(float));
    slotAt(ctx, pc->auxByte != 0u ? pc->storeOff : pc->dstOff).storeFloat(f);
    ++pc;
    return StencilStepStatus::Continue;
}

template <runtime::detail::QuickOpcode Op, bool Checked, bool Trusted>
StencilStepStatus th_BinaryInt(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    std::int64_t a = 0, b = 0;
    if constexpr (Trusted) {
        a = slotAt(ctx, pc->src0Off).asInt64Unchecked();
        b = slotAt(ctx, pc->src1Off).asInt64Unchecked();
    } else {
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(a) || !slotAt(ctx, pc->src1Off).tryGetInt64(b)) {
            ctx->remainingBudget += pc->unexecutedOnTrap;
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
            return StencilStepStatus::Error;
        }
    }
    if (!evalQuickIntStencil(ctx, pc, Op, Checked, a, b, pc->unexecutedOnTrap, slotAt(ctx, pc->dstOff))) {
        return StencilStepStatus::Error;
    }
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_SuperBinaryIntGeneric(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    using FO = runtime::detail::FlatOpcode;

    std::int64_t lhs = 0;
    if (!slotAt(ctx, pc->src0Off).tryGetInt64(lhs)) {
        ctx->remainingBudget += pc->unexecutedOnTrap;
        (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
        return StencilStepStatus::Error;
    }

    if (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStore ||
        fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStoreElided) {
        std::int64_t rhs1 = 0, rhs2 = 0;
        if (!slotAt(ctx, pc->src1Off).tryGetInt64(rhs1)) {
            ctx->remainingBudget += 2u;
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
            return StencilStepStatus::Error;
        }
        if (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStore) {
            slotAt(ctx, pc->dstOff).storeInt64(lhs);
        }
        runtime::Value mid{};
        if (!evalQuickIntStencil(ctx, pc, fi.binaryOpcode1, fi.checked1(), lhs, rhs1, 2u, mid)) {
            return StencilStepStatus::Error;
        }
        if (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStore) {
            slotAt(ctx, pc->aux0Off) = mid;
        }
        if (!slotAt(ctx, pc->src2Off).tryGetInt64(rhs2)) {
            ctx->remainingBudget += 1u;
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
            return StencilStepStatus::Error;
        }
        runtime::Value out{};
        if (!evalQuickIntStencil(ctx, pc, fi.binaryOpcode2, fi.checked2(), mid.asInt64Unchecked(), rhs2, 1u, out)) {
            return StencilStepStatus::Error;
        }
        if (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStore) {
            slotAt(ctx, pc->aux1Off) = out;
        }
        slotAt(ctx, pc->storeOff) = out;
        ++pc;
        return StencilStepStatus::Continue;
    }

    const bool isConst =
        fi.flatOpcode == FO::SuperBinaryLocalIntConst ||
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStore ||
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreElided ||
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump;
    std::int64_t rhs = pc->imm64;
    if (!isConst && !slotAt(ctx, pc->src1Off).tryGetInt64(rhs)) {
        ctx->remainingBudget += pc->unexecutedOnTrap;
        (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
        return StencilStepStatus::Error;
    }

    const bool isJump =
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump ||
        fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump ||
        fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreJump;
    const bool isElided =
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreElided ||
        fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreElided ||
        fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreElided;

    if (!isElided && !isJump) {
        slotAt(ctx, pc->dstOff).storeInt64(lhs);
        if (isConst || fi.flatOpcode == FO::SuperBinaryLocalIntLocal || fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore) {
            slotAt(ctx, pc->aux0Off).storeInt64(rhs);
        }
    }

    runtime::Value out{};
    if (!evalQuickIntStencil(ctx, pc, fi.binaryOpcode1, fi.checked1(), lhs, rhs, pc->unexecutedOnTrap, out)) {
        return StencilStepStatus::Error;
    }
    if (isJump) {
        slotAt(ctx, pc->dstOff) = out;
        ctx->branchesTaken += ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2u : 1u;
        pc = pc->trueTarget;
        return StencilStepStatus::Continue;
    }
    if (isElided) {
        slotAt(ctx, pc->storeOff) = out;
    } else {
        slotAt(ctx, pc->aux1Off) = out;
        if (fi.flatOpcode == FO::SuperBinaryLocalIntConstStore ||
            fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore ||
            fi.flatOpcode == FO::SuperBinaryLocalIntRegStore) {
            slotAt(ctx, pc->storeOff) = out;
        }
    }
    ++pc;
    return StencilStepStatus::Continue;
}

template <runtime::detail::QuickOpcode CmpOp, bool IsConst>
StencilStepStatus th_SuperLessBranch(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    std::int64_t lhs = 0, rhs = pc->imm64;
    if (!slotAt(ctx, pc->src0Off).tryGetInt64(lhs) ||
        (!IsConst && !slotAt(ctx, pc->src1Off).tryGetInt64(rhs))) {
        ctx->remainingBudget -= (pc->span - 1u);
        (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int in branch");
        return StencilStepStatus::Error;
    }
    ctx->remainingBudget -= pc->span;
    ++ctx->branchesTaken;
    bool cond = false;
    using QO = runtime::detail::QuickOpcode;
    if constexpr (CmpOp == QO::LessInt) cond = (lhs < rhs);
    else if constexpr (CmpOp == QO::LessOrEqualInt) cond = (lhs <= rhs);
    else if constexpr (CmpOp == QO::GreaterInt) cond = (lhs > rhs);
    else if constexpr (CmpOp == QO::GreaterOrEqualInt) cond = (lhs >= rhs);
    else if constexpr (CmpOp == QO::EqualInt) cond = (lhs == rhs);
    else cond = (lhs != rhs);
    pc = cond ? pc->trueTarget : pc->falseTarget;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ArrayLengthBranch(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitArrayLengthHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    pc = (ctx->branchCondOut != 0u) ? pc->trueTarget : pc->falseTarget;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ArrayLength(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitArrayLengthHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_NewArray(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitNewArrayHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_FlatStructOrStackRef(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitFlatStructAndStackRefHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_LoadElement(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    // Fast inline packed int32 / double hit path before calling helper
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
    const auto& arrVal = slotAt(ctx, pc->src0Off);
    if (arrVal.rawTag() == runtime::ValueTag::ObjectRef && ce.isPacked != 0u) {
        std::uint64_t sg = 0;
        std::memcpy(&sg, &arrVal, 8);
        if (sg == ce.slotAndGen && arrVal.u_.objectRef.heapId == ce.heapId) {
            std::int64_t idx = 0;
            if (slotAt(ctx, pc->src1Off).tryGetInt64(idx) &&
                static_cast<std::uint64_t>(idx) < ce.length) {
                if (ce.encoding == kPackedInt32 &&
                    (fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementIntFast ||
                     (static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1)) == semantic::PrimitiveType::Int &&
                      fi.integerImmediate == 0))) {
                    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
                    ctx->remainingBudget -= pc->span;
                    slotAt(ctx, pc->dstOff).storeInt64(
                        static_cast<std::int64_t>(static_cast<const std::int32_t*>(ce.rawData)[idx]));
                    ++pc;
                    return StencilStepStatus::Continue;
                }
                if (ce.encoding == kPackedFloat64 &&
                    fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementTypedFast &&
                    static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1)) == semantic::PrimitiveType::Double) {
                    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
                    ctx->remainingBudget -= pc->span;
                    slotAt(ctx, pc->dstOff).storeDouble(static_cast<const double*>(ce.rawData)[idx]);
                    ++pc;
                    return StencilStepStatus::Continue;
                }
            }
        }
    }
    if (jitLoadElementHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_StoreElement(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
    const auto& arrVal = slotAt(ctx, pc->src0Off);
    if (arrVal.rawTag() == runtime::ValueTag::ObjectRef && ce.isPacked != 0u) {
        std::uint64_t sg = 0;
        std::memcpy(&sg, &arrVal, 8);
        if (sg == ce.slotAndGen && arrVal.u_.objectRef.heapId == ce.heapId) {
            std::int64_t idx = 0;
            if (slotAt(ctx, pc->src1Off).tryGetInt64(idx) &&
                static_cast<std::uint64_t>(idx) < ce.length) {
                const auto& val = slotAt(ctx, pc->src2Off);
                std::int64_t iv = 0;
                if (ce.encoding == kPackedInt32 && val.tryGetInt64(iv) && fitsInt32(iv)) {
                    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
                    ctx->remainingBudget -= pc->span;
                    static_cast<std::int32_t*>(ce.rawData)[idx] = static_cast<std::int32_t>(iv);
                    ++pc;
                    return StencilStepStatus::Continue;
                }
                double dv = 0.0;
                if (ce.encoding == kPackedFloat64 && val.tryGetDouble(dv)) {
                    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
                    ctx->remainingBudget -= pc->span;
                    static_cast<double*>(ce.rawData)[idx] = dv;
                    ++pc;
                    return StencilStepStatus::Continue;
                }
            }
        }
    }
    if (jitStoreElementHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ScalarMisc(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    using FO = runtime::detail::FlatOpcode;
    using QO = runtime::detail::QuickOpcode;

    const auto failType = [&]() noexcept {
        ctx->remainingBudget -= (pc->span - pc->unexecutedOnTrap);
        (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "scalar type mismatch");
        return StencilStepStatus::Error;
    };

    switch (fi.flatOpcode) {
    case FO::NegateIntUnchecked: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeInt64(v == std::numeric_limits<std::int32_t>::min() ? v : -v);
        break;
    }
    case FO::LogicalNot: {
        bool b = false;
        if (!slotAt(ctx, pc->src0Off).tryGetBool(b)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeBool(!b);
        break;
    }
    case FO::AddDouble: case FO::SubDouble: case FO::MulDouble: case FO::DivDouble:
    case FO::LessDouble: case FO::LessOrEqualDouble: case FO::GreaterDouble: case FO::GreaterOrEqualDouble:
    case FO::AddDoubleTrusted: case FO::SubDoubleTrusted: case FO::MulDoubleTrusted: case FO::DivDoubleTrusted: {
        double a = 0.0, b = 0.0;
        if (!slotAt(ctx, pc->src0Off).tryGetDouble(a) || !slotAt(ctx, pc->src1Off).tryGetDouble(b)) return failType();
        ctx->remainingBudget -= pc->span;
        switch (fi.flatOpcode) {
        case FO::AddDouble: case FO::AddDoubleTrusted: slotAt(ctx, pc->dstOff).storeDouble(a + b); break;
        case FO::SubDouble: case FO::SubDoubleTrusted: slotAt(ctx, pc->dstOff).storeDouble(a - b); break;
        case FO::MulDouble: case FO::MulDoubleTrusted: slotAt(ctx, pc->dstOff).storeDouble(a * b); break;
        case FO::DivDouble: case FO::DivDoubleTrusted: slotAt(ctx, pc->dstOff).storeDouble(a / b); break;
        case FO::LessDouble: slotAt(ctx, pc->dstOff).storeBool(a < b); break;
        case FO::LessOrEqualDouble: slotAt(ctx, pc->dstOff).storeBool(a <= b); break;
        case FO::GreaterDouble: slotAt(ctx, pc->dstOff).storeBool(a > b); break;
        default: slotAt(ctx, pc->dstOff).storeBool(a >= b); break;
        }
        break;
    }
    case FO::NegateDouble: {
        double d = 0.0;
        if (!slotAt(ctx, pc->src0Off).tryGetDouble(d)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeDouble(-d);
        break;
    }
    case FO::SuperBinaryLocalDoubleConstStore:
    case FO::SuperBinaryLocalDoubleConstStoreElided:
    case FO::SuperBinaryLocalDoubleLocalStore:
    case FO::SuperBinaryLocalDoubleLocalStoreElided: {
        double a = 0.0, b = 0.0;
        if (!slotAt(ctx, pc->src0Off).tryGetDouble(a)) return failType();
        const bool isConst = (fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStore ||
                              fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStoreElided);
        if (isConst) std::memcpy(&b, &pc->imm64, 8);
        else if (!slotAt(ctx, pc->src1Off).tryGetDouble(b)) return failType();
        double out = 0.0;
        if (fi.binaryOpcode1 == QO::AddDouble) out = a + b;
        else if (fi.binaryOpcode1 == QO::SubtractDouble) out = a - b;
        else if (fi.binaryOpcode1 == QO::MultiplyDouble) out = a * b;
        else out = a / b;
        ctx->remainingBudget -= pc->span;
        if (fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStore ||
            fi.flatOpcode == FO::SuperBinaryLocalDoubleLocalStore) {
            slotAt(ctx, pc->dstOff).storeDouble(a);
            slotAt(ctx, pc->aux0Off).storeDouble(b);
            slotAt(ctx, pc->aux1Off).storeDouble(out);
        }
        slotAt(ctx, pc->storeOff).storeDouble(out);
        break;
    }
    case FO::AddFloat: case FO::SubFloat: case FO::MulFloat: case FO::DivFloat:
    case FO::LessFloat: case FO::LessOrEqualFloat: case FO::GreaterFloat: case FO::GreaterOrEqualFloat:
    case FO::EqualFloat: case FO::NotEqualFloat:
    case FO::AddFloatTrusted: case FO::SubFloatTrusted: case FO::MulFloatTrusted: {
        float a = 0.0f, b = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(a) || !slotAt(ctx, pc->src1Off).tryGetFloat(b)) return failType();
        ctx->remainingBudget -= pc->span;
        switch (fi.flatOpcode) {
        case FO::AddFloat: case FO::AddFloatTrusted: slotAt(ctx, pc->dstOff).storeFloat(a + b); break;
        case FO::SubFloat: case FO::SubFloatTrusted: slotAt(ctx, pc->dstOff).storeFloat(a - b); break;
        case FO::MulFloat: case FO::MulFloatTrusted: slotAt(ctx, pc->dstOff).storeFloat(a * b); break;
        case FO::DivFloat: slotAt(ctx, pc->dstOff).storeFloat(a / b); break;
        case FO::LessFloat: slotAt(ctx, pc->dstOff).storeBool(a < b); break;
        case FO::LessOrEqualFloat: slotAt(ctx, pc->dstOff).storeBool(a <= b); break;
        case FO::GreaterFloat: slotAt(ctx, pc->dstOff).storeBool(a > b); break;
        case FO::GreaterOrEqualFloat: slotAt(ctx, pc->dstOff).storeBool(a >= b); break;
        case FO::EqualFloat: slotAt(ctx, pc->dstOff).storeBool(a == b); break;
        default: slotAt(ctx, pc->dstOff).storeBool(a != b); break;
        }
        break;
    }
    case FO::NegateFloat: {
        float f = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(f)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeFloat(-f);
        break;
    }
    case FO::SuperBinaryLocalFloatLocalStore:
    case FO::SuperBinaryLocalFloatLocalStoreElided:
    case FO::SuperBinaryLocalFloatReg:
    case FO::SuperBinaryLocalFloatRegElided:
    case FO::SuperBinaryLocalFloatRegStore:
    case FO::SuperBinaryLocalFloatRegStoreElided:
    case FO::SuperBinaryRegFloatStoreElided: {
        float a = 0.0f, b = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(a) || !slotAt(ctx, pc->src1Off).tryGetFloat(b)) return failType();
        float out = 0.0f;
        if (fi.binaryOpcode1 == QO::AddFloat) out = a + b;
        else if (fi.binaryOpcode1 == QO::SubtractFloat) out = a - b;
        else if (fi.binaryOpcode1 == QO::MultiplyFloat) out = a * b;
        else out = a / b;
        ctx->remainingBudget -= pc->span;
        if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
            slotAt(ctx, pc->dstOff).storeFloat(a);
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore) slotAt(ctx, pc->aux0Off).storeFloat(b);
        }
        if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatRegElided ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
            slotAt(ctx, pc->aux1Off).storeFloat(out);
        }
        if (fi.flatOpcode != FO::SuperBinaryLocalFloatReg && fi.flatOpcode != FO::SuperBinaryLocalFloatRegElided) {
            slotAt(ctx, pc->storeOff).storeFloat(out);
        }
        break;
    }
    case FO::ConvertIntToLongFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeLong(v);
        break;
    }
    case FO::ConvertIntToDoubleFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeDouble(static_cast<double>(v));
        break;
    }
    case FO::ConvertLongToDoubleFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetLong(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeDouble(static_cast<double>(v));
        break;
    }
    case FO::ConvertIntToFloatFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeFloat(static_cast<float>(v));
        break;
    }
    case FO::ConvertFloatToDoubleFast: {
        float f = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(f)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeDouble(static_cast<double>(f));
        break;
    }
    case FO::ConvertDoubleToFloatFast: {
        double d = 0.0;
        if (!slotAt(ctx, pc->src0Off).tryGetDouble(d)) return failType();
        if (fi.checked1() &&
            !(d >= -static_cast<double>(std::numeric_limits<float>::max()) &&
              d <= static_cast<double>(std::numeric_limits<float>::max()))) {
            ctx->remainingBudget -= (pc->span - pc->unexecutedOnTrap);
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::IntegerOverflow, "numeric conversion overflow");
            return StencilStepStatus::Error;
        }
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeFloat(static_cast<float>(d));
        break;
    }
    case FO::ConvertFloatToIntFast: {
        float f = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(f)) return failType();
        const double d = static_cast<double>(f);
        if (!(d >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
              d <= static_cast<double>(std::numeric_limits<std::int32_t>::max()))) {
            ctx->remainingBudget -= (pc->span - pc->unexecutedOnTrap);
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::IntegerOverflow, "numeric conversion overflow");
            return StencilStepStatus::Error;
        }
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeInt64(static_cast<std::int64_t>(d));
        break;
    }
    case FO::ConvertLongToIntFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetLong(v)) return failType();
        if (fi.checked1() && !fitsInt32(v)) {
            ctx->remainingBudget -= (pc->span - pc->unexecutedOnTrap);
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::IntegerOverflow, "numeric conversion overflow");
            return StencilStepStatus::Error;
        }
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeInt64(fi.checked1() ? v : wrapInt32Val(v));
        break;
    }
    default:
        return failType();
    }
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_InlineCallEnter(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitInlineCallEnterHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    if (ctx->nextFlatOffsetOut != 0u) {
        pc += (1u + pc->auxByte);
    } else {
        ++pc;
    }
    return StencilStepStatus::Continue;
}

StencilStepStatus th_InlineReturn(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    using FO = runtime::detail::FlatOpcode;
    runtime::Value retVal{};
    const bool hasScalar =
        (fi.flatOpcode == FO::InlineReturnScalar || fi.flatOpcode == FO::InlineReturnScalarJump) &&
        fi.dstSlot != runtime::detail::InvalidCompactReg;
    if (hasScalar) retVal = slotAt(ctx, pc->src0Off);
    if (pc->imm64 != 0) {
        const auto packed = static_cast<std::uint64_t>(pc->imm64);
        std::memset(
            static_cast<void*>(ctx->frameBase + (packed & 0xFFFFu)),
            0,
            static_cast<std::size_t>(packed >> 16u) * sizeof(runtime::Value));
    }
    if (hasScalar) slotAt(ctx, pc->dstOff) = retVal;
    if (fi.flatOpcode == FO::InlineReturnVoidJump || fi.flatOpcode == FO::InlineReturnScalarJump) {
        pc = pc->trueTarget;
    } else {
        ++pc;
    }
    return StencilStepStatus::Continue;
}

StencilStepStatus th_Jump(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    ctx->branchesTaken += pc->span;
    pc = pc->trueTarget;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_BranchBool(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    ++ctx->branchesTaken;
    pc = slotAt(ctx, pc->src0Off).asBoolUnchecked() ? pc->trueTarget : pc->falseTarget;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_BranchGeneric(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& cold = execFunc.flatCold[pc->flatIdx];
    const auto& blk = execFunc.blocks[cold.blockIndex];
    const auto& term = blk.bytecode->terminator;
    const runtime::detail::ExecutionEdge* edge = &blk.branch;
    const ThreadedStencilOp* nextPc = pc->trueTarget;
    if (term.kind == bytecode::TerminatorKind::Branch) {
        const std::size_t condSlot = execFunc.registerOffset + term.condition;
        bool b = false;
        if (condSlot >= execFunc.totalFrameSlots || !ctx->frameBase[condSlot].tryGetBool(b)) {
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "branch condition expected bool");
            return StencilStepStatus::Error;
        }
        if (!b) {
            edge = &blk.falseBranch;
            nextPc = pc->falseTarget;
        }
    }
    ++ctx->branchesTaken;
    if (edge->hasArguments && edge->sources && edge->destinations) {
        const auto& srcs = *edge->sources;
        const auto& dsts = *edge->destinations;
        runtime::Value* const regPtr = ctx->frameBase + execFunc.registerOffset;
        runtime::Value* const scratchPtr = ctx->frameBase + execFunc.scratchOffset;
        for (std::size_t k = 0; k < srcs.size(); ++k) {
            scratchPtr[k] = regPtr[srcs[k]];
        }
        for (std::size_t k = 0; k < srcs.size(); ++k) {
            regPtr[dsts[k].target] = scratchPtr[k];
        }
    }
    pc = nextPc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ReturnVoid(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    *ctx->returnSlot = runtime::Value{};
    return StencilStepStatus::ReturnOk;
}

StencilStepStatus th_ReturnValue(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    *ctx->returnSlot = slotAt(ctx, pc->src0Off);
    return StencilStepStatus::ReturnOk;
}

void buildThreadedStream(CompiledFunctionRecord& rec) {
    const auto& execFunc = *rec.execFunc;
    const std::size_t n = execFunc.flatStream.size();
    rec.threadedStream.resize(n);

    const auto toOff = [](std::uint16_t slot) noexcept -> std::uint32_t {
        return slot == runtime::detail::InvalidCompactReg
            ? 0u
            : static_cast<std::uint32_t>(slot) * 16u;
    };

    for (std::size_t i = 0; i < n; ++i) {
        const auto& fi = execFunc.flatStream[i];
        auto& op = rec.threadedStream[i];
        op.imm64 = fi.integerImmediate;
        op.dstOff = toOff(fi.dstSlot);
        op.src0Off = toOff(fi.src0Slot);
        op.src1Off = toOff(fi.src1Slot);
        op.src2Off = toOff(fi.src2Slot);
        op.aux0Off = toOff(fi.aux0Slot);
        op.aux1Off = toOff(fi.aux1Slot);
        op.storeOff = toOff(fi.storeSlot);
        op.flatIdx = static_cast<std::uint32_t>(i);
        op.span = fi.span;
        op.unexecutedOnTrap = rec.unexecutedAfterOp[i];
        op.flags = fi.arithFlags;

        using FO = runtime::detail::FlatOpcode;
        using QO = runtime::detail::QuickOpcode;
        switch (fi.flatOpcode) {
        case FO::MoveSlot:
        case FO::AliasStructSlice:
            op.handler = &th_MoveSlot;
            break;
        case FO::InitStructSlice:
        case FO::CopyStructSlice:
        case FO::LoadStructSliceField:
        case FO::StoreStructSliceField:
        case FO::StoreLocalStructFieldInPlace:
        case FO::InitStackRefCell:
        case FO::BindLocalStackRef:
        case FO::WritebackLocalStackRef:
        case FO::LoadStackRefCell:
        case FO::CheckNotNullFast:
        case FO::StoreStackRefCell:
        case FO::StoreStackRefStructFieldInPlace:
        case FO::NewObjectFast:
        case FO::LoadFieldExactFast:
        case FO::LoadFieldFast:
        case FO::StoreFieldExactFast:
        case FO::StoreFieldFast:
        case FO::GenericBytecode:
            op.handler = &th_FlatStructOrStackRef;
            break;
        case FO::SuperMoveParamStoreLocal:
            op.auxByte = 1u;
            op.handler = &th_SuperMoveParamStore;
            break;
        case FO::SuperMoveParamStoreLocalElided:
            op.auxByte = 0u;
            op.handler = &th_SuperMoveParamStore;
            break;
        case FO::ConstantInt: op.handler = &th_ConstInt; break;
        case FO::ConstantLong: op.handler = &th_ConstLong; break;
        case FO::ConstantBool: op.handler = &th_ConstBool; break;
        case FO::ConstantDouble: op.handler = &th_ConstDouble; break;
        case FO::SuperConstFloat:
            op.auxByte = 0u;
            op.handler = &th_ConstFloat;
            break;
        case FO::SuperConstFloatStoreLocal:
            op.auxByte = 1u;
            op.handler = &th_ConstFloat;
            break;
        case FO::AddIntChecked: op.handler = &th_BinaryInt<QO::AddInt, true, false>; break;
        case FO::AddIntUnchecked: op.handler = &th_BinaryInt<QO::AddInt, false, false>; break;
        case FO::SubIntChecked: op.handler = &th_BinaryInt<QO::SubtractInt, true, false>; break;
        case FO::SubIntUnchecked: op.handler = &th_BinaryInt<QO::SubtractInt, false, false>; break;
        case FO::MulIntChecked: op.handler = &th_BinaryInt<QO::MultiplyInt, true, false>; break;
        case FO::MulIntUnchecked: op.handler = &th_BinaryInt<QO::MultiplyInt, false, false>; break;
        case FO::DivInt: op.handler = &th_BinaryInt<QO::DivideInt, true, false>; break;
        case FO::RemInt: op.handler = &th_BinaryInt<QO::RemainderInt, true, false>; break;
        case FO::LessInt: op.handler = &th_BinaryInt<QO::LessInt, false, false>; break;
        case FO::LessOrEqualInt: op.handler = &th_BinaryInt<QO::LessOrEqualInt, false, false>; break;
        case FO::GreaterInt: op.handler = &th_BinaryInt<QO::GreaterInt, false, false>; break;
        case FO::GreaterOrEqualInt: op.handler = &th_BinaryInt<QO::GreaterOrEqualInt, false, false>; break;
        case FO::EqualInt: op.handler = &th_BinaryInt<QO::EqualInt, false, false>; break;
        case FO::NotEqualInt: op.handler = &th_BinaryInt<QO::NotEqualInt, false, false>; break;
        case FO::AddIntTrustedChecked: op.handler = &th_BinaryInt<QO::AddInt, true, true>; break;
        case FO::AddIntTrustedUnchecked: op.handler = &th_BinaryInt<QO::AddInt, false, true>; break;
        case FO::SubIntTrustedChecked: op.handler = &th_BinaryInt<QO::SubtractInt, true, true>; break;
        case FO::SubIntTrustedUnchecked: op.handler = &th_BinaryInt<QO::SubtractInt, false, true>; break;
        case FO::MulIntTrustedChecked: op.handler = &th_BinaryInt<QO::MultiplyInt, true, true>; break;
        case FO::MulIntTrustedUnchecked: op.handler = &th_BinaryInt<QO::MultiplyInt, false, true>; break;
        case FO::LessIntTrusted: op.handler = &th_BinaryInt<QO::LessInt, false, true>; break;

        case FO::SuperBinaryLocalIntConst:
        case FO::SuperBinaryLocalIntConstStore:
        case FO::SuperBinaryLocalIntConstStoreElided:
        case FO::SuperBinaryLocalIntConstStoreJump:
        case FO::SuperBinaryLocalIntLocal:
        case FO::SuperBinaryLocalIntLocalStore:
        case FO::SuperBinaryLocalIntLocalStoreElided:
        case FO::SuperBinaryLocalIntLocalStoreJump:
        case FO::SuperBinaryLocalIntReg:
        case FO::SuperBinaryLocalIntRegStore:
        case FO::SuperBinaryLocalIntRegStoreElided:
        case FO::SuperBinaryLocalIntRegStoreJump:
        case FO::SuperBinaryLocalIntRegChainStore:
        case FO::SuperBinaryLocalIntRegChainStoreElided:
            op.handler = &th_SuperBinaryIntGeneric;
            if (fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreJump) {
                op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            }
            break;

        case FO::SuperLessLocalConstBranch:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = &rec.threadedStream[fi.falseTargetOffset];
            switch (fi.binaryOpcode1) {
            case QO::LessInt: op.handler = &th_SuperLessBranch<QO::LessInt, true>; break;
            case QO::LessOrEqualInt: op.handler = &th_SuperLessBranch<QO::LessOrEqualInt, true>; break;
            case QO::GreaterInt: op.handler = &th_SuperLessBranch<QO::GreaterInt, true>; break;
            case QO::GreaterOrEqualInt: op.handler = &th_SuperLessBranch<QO::GreaterOrEqualInt, true>; break;
            case QO::EqualInt: op.handler = &th_SuperLessBranch<QO::EqualInt, true>; break;
            default: op.handler = &th_SuperLessBranch<QO::NotEqualInt, true>; break;
            }
            break;

        case FO::SuperLessLocalRegBranch:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = &rec.threadedStream[fi.falseTargetOffset];
            switch (fi.binaryOpcode1) {
            case QO::LessInt: op.handler = &th_SuperLessBranch<QO::LessInt, false>; break;
            case QO::LessOrEqualInt: op.handler = &th_SuperLessBranch<QO::LessOrEqualInt, false>; break;
            case QO::GreaterInt: op.handler = &th_SuperLessBranch<QO::GreaterInt, false>; break;
            case QO::GreaterOrEqualInt: op.handler = &th_SuperLessBranch<QO::GreaterOrEqualInt, false>; break;
            case QO::EqualInt: op.handler = &th_SuperLessBranch<QO::EqualInt, false>; break;
            default: op.handler = &th_SuperLessBranch<QO::NotEqualInt, false>; break;
            }
            break;

        case FO::SuperLessLocalArrayLengthBranch:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = &rec.threadedStream[fi.falseTargetOffset];
            op.handler = &th_ArrayLengthBranch;
            break;

        case FO::ArrayLengthFast: op.handler = &th_ArrayLength; break;
        case FO::NewArrayFast: op.handler = &th_NewArray; break;
        case FO::LoadElementIntFast:
        case FO::LoadElementTypedFast:
            op.handler = &th_LoadElement;
            break;
        case FO::StoreElementIntFast:
        case FO::StoreElementPrimFast:
        case FO::StoreElementStructFast:
        case FO::StoreElementObjFast:
        case FO::StoreElementStructFieldInPlace:
            op.handler = &th_StoreElement;
            break;

        case FO::InlineCallEnter:
            op.auxByte = fi.inlineArgCount;
            op.handler = &th_InlineCallEnter;
            break;
        case FO::InlineReturnVoid:
        case FO::InlineReturnVoidJump:
        case FO::InlineReturnScalar:
        case FO::InlineReturnScalarJump:
            if (fi.flatOpcode == FO::InlineReturnVoidJump ||
                fi.flatOpcode == FO::InlineReturnScalarJump) {
                op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            }
            op.handler = &th_InlineReturn;
            break;

        case FO::TermJumpNoArgs:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.handler = &th_Jump;
            break;
        case FO::TermBranchBoolNoArgs:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = &rec.threadedStream[fi.falseTargetOffset];
            op.handler = &th_BranchBool;
            break;
        case FO::TermBranchGeneric:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = (fi.falseTargetOffset < n)
                ? &rec.threadedStream[fi.falseTargetOffset]
                : nullptr;
            op.handler = &th_BranchGeneric;
            break;
        case FO::TermReturnVoid:
            op.handler = &th_ReturnVoid;
            break;
        case FO::TermReturnScalarFast:
            op.handler = &th_ReturnValue;
            break;
        case FO::TermReturnGeneric: {
            const auto& cold = execFunc.flatCold[i];
            const auto retReg = execFunc.blocks[cold.blockIndex].bytecode->terminator.value;
            op.src0Off = static_cast<std::uint32_t>(execFunc.registerOffset + retReg) * 16u;
            op.handler = &th_ReturnValue;
            break;
        }
        default:
            op.handler = &th_ScalarMisc;
            break;
        }
    }
}

[[nodiscard]] bool validateJitEntryArguments(
    const bytecode::Function& function,
    runtime::ManagedHeap* heap,
    const std::vector<runtime::Value>& arguments,
    bool& outAllIntArgsFit32,
    runtime::ErrorCode& outCode,
    std::string& outMsg) {
    outAllIntArgsFit32 = true;
    if (arguments.size() != function.parameterTypes.size()) {
        outCode = runtime::ErrorCode::InvalidArguments;
        outMsg = "argument count does not match function signature";
        return false;
    }
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        const auto expectedType = function.parameterTypes[i];
        const auto expectedTid = i < function.parameterTypeIds.size()
            ? function.parameterTypeIds[i]
            : semantic::SymbolId{0};
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&arg)) {
            if (!heap || !heap->isAlive(*ref)) {
                outCode = runtime::ErrorCode::InvalidObjectReference;
                outMsg = "argument contains an invalid managed object reference";
                return false;
            }
        }
        if (runtime::valueType(arg) != expectedType) {
            outCode = runtime::ErrorCode::TypeMismatch;
            outMsg = "argument type does not match function signature";
            return false;
        }
        if (expectedType == semantic::PrimitiveType::Int) {
            if (!fitsInt32(arg.asInt64Unchecked())) {
                outAllIntArgsFit32 = false;
            }
        } else if (expectedType == semantic::PrimitiveType::Array) {
            if (!std::holds_alternative<runtime::NullArray>(arg) && expectedTid != 0) {
                const auto* ref = std::get_if<runtime::ObjectRef>(&arg);
                const auto actualTid = (ref && heap)
                    ? heap->arrayTypeId(*ref)
                    : std::optional<semantic::SymbolId>{};
                if (!actualTid || *actualTid != expectedTid) {
                    outCode = runtime::ErrorCode::TypeMismatch;
                    outMsg = "argument has the wrong runtime array type";
                    return false;
                }
            }
        } else if (expectedType == semantic::PrimitiveType::Struct) {
            if (expectedTid != 0 && runtime::structValueTypeId(arg) != expectedTid) {
                outCode = runtime::ErrorCode::TypeMismatch;
                outMsg = "argument has the wrong runtime struct type";
                return false;
            }
        } else if (expectedType == semantic::PrimitiveType::Object ||
                   expectedType == semantic::PrimitiveType::Enum ||
                   expectedType == semantic::PrimitiveType::Handle) {
            // Let full signature checker in Interpreter handle complex exact object/handle hierarchy if non-null
            if (expectedTid != 0) {
                outAllIntArgsFit32 = false;
            }
        }
    }
    return true;
}

} // namespace

// ============================================================================
// StencilModule & StencilJit Implementation
// ============================================================================

struct StencilModule::Impl {
    std::shared_ptr<const runtime::ProgramImage> program;
    std::shared_ptr<runtime::ManagedHeap> heap;
    std::shared_ptr<const runtime::BindingRegistry> bindings;
    runtime::ExternalFunction externalResolver;
    std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime;
    StencilJitOptions options{};

    ExecutableCodeArena primaryArena;
    mutable std::vector<ExecutableCodeArena> lazyTierArenas;
    mutable std::mutex tierPromotionMutex;

    std::unordered_map<semantic::SymbolId, std::unique_ptr<CompiledFunctionRecord>> records;
    mutable std::atomic<std::size_t> nativeCount{0};
    mutable std::atomic<std::size_t> threadedCount{0};
    mutable std::atomic<std::size_t> fallbackCount{0};
    mutable std::atomic<std::size_t> codeBytes{0};
    bool nativeBackendAvailable = false;

    explicit Impl(
        std::shared_ptr<const runtime::ProgramImage> prog,
        StencilJitOptions opts)
        : program(std::move(prog)),
          heap(std::make_shared<runtime::ManagedHeap>()),
          asyncRuntime(std::make_shared<runtime::AsyncRuntimeContext>(heap.get())),
          options(opts),
          primaryArena(opts.arenaBlockBytes) {}

    bool promoteFunctionToNativeLocked(CompiledFunctionRecord& rec, ExecutableCodeArena* batchArena) const {
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
        if (!nativeBackendAvailable || !rec.eligibleForNative) return false;
        if (rec.tier.load(std::memory_order_relaxed) == StencilTier::NativeMachineCode) return true;

#if defined(__aarch64__) || defined(_M_ARM64)
        auto machineBytes = compileFunctionA64(rec);
#else
        auto machineBytes = compileFunctionX64(rec);
#endif
        if (machineBytes.empty()) return false;

        if (batchArena != nullptr) {
            std::uint8_t* dst = batchArena->emit(machineBytes.data(), machineBytes.size(), 16);
            if (!dst) return false;
            rec.rawCodePtr = dst;
            rec.rawCodeSize = machineBytes.size();
            rec.nativeEntry.store(bitCastFnPtr<NativeJitEntryFn>(dst), std::memory_order_release);
            rec.tier.store(StencilTier::NativeMachineCode, std::memory_order_release);
            return true;
        }

        ExecutableCodeArena singleArena(std::max<std::size_t>(machineBytes.size() + 64u, ExecutableCodeArena::systemPageSize()));
        std::uint8_t* dst = singleArena.emit(machineBytes.data(), machineBytes.size(), 16);
        if (!dst || !singleArena.seal()) return false;

        rec.rawCodePtr = dst;
        rec.rawCodeSize = machineBytes.size();
        const StencilTier prevTier = rec.tier.load(std::memory_order_relaxed);
        rec.nativeEntry.store(bitCastFnPtr<NativeJitEntryFn>(dst), std::memory_order_release);
        rec.tier.store(StencilTier::NativeMachineCode, std::memory_order_release);
        lazyTierArenas.push_back(std::move(singleArena));

        if (prevTier == StencilTier::ThreadedStencils) {
            threadedCount.fetch_sub(1, std::memory_order_relaxed);
        } else if (prevTier == StencilTier::InterpreterFallback) {
            fallbackCount.fetch_sub(1, std::memory_order_relaxed);
        }
        nativeCount.fetch_add(1, std::memory_order_relaxed);
        codeBytes.fetch_add(machineBytes.size(), std::memory_order_relaxed);
        return true;
#else
        (void)rec;
        (void)batchArena;
        return false;
#endif
    }

    runtime::ExecutionResult invokeFallbackInterpreter(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments,
        runtime::ExecutionOptions execOptions) const {
        runtime::Interpreter interp(program, heap);
        if (bindings) interp.setBindingRegistry(bindings);
        if (externalResolver) interp.setExternalResolver(externalResolver);
        if (asyncRuntime) interp.setAsyncRuntime(asyncRuntime);
        return interp.invoke(symbolId, arguments, std::move(execOptions));
    }

    runtime::ExecutionResult invokeBySymbol(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments,
        runtime::ExecutionOptions execOptions,
        bool forceSimulateArm64 = false,
        std::size_t baseFrameOffset = 0) const {
        if (!program) {
            runtime::ExecutionResult invalid;
            invalid.error.code = runtime::ErrorCode::InvalidProgram;
            invalid.error.message = "StencilJit module has no linked program image";
            return invalid;
        }

        const auto recIt = records.find(symbolId);
        if (recIt == records.end() || !recIt->second || !recIt->second->location) {
            runtime::ExecutionResult missing;
            missing.error.code = runtime::ErrorCode::FunctionNotFound;
            missing.error.message = "entry function was not found";
            return missing;
        }

        const bool hasTraceOrDebugOrStrictEvents =
            execOptions.determinism.mode != runtime::DeterminismMode::Off ||
            execOptions.profile != nullptr ||
            (execOptions.trace && static_cast<bool>(execOptions.trace)) ||
            execOptions.debugger != nullptr;
        const bool activeGcCycle =
            execOptions.limits.gcWorkBudget != 0 &&
            heap != nullptr &&
            (heap->phase() != runtime::GcPhase::Idle || heap->collectionRequested());

        if (hasTraceOrDebugOrStrictEvents || activeGcCycle) {
            return invokeFallbackInterpreter(symbolId, arguments, std::move(execOptions));
        }

        CompiledFunctionRecord& rec = *recIt->second;
        StencilTier currentTier = rec.tier.load(std::memory_order_acquire);

        if (options.hotnessThreshold > 0 &&
            currentTier != StencilTier::NativeMachineCode &&
            rec.eligibleForNative &&
            nativeBackendAvailable) {
            const std::uint32_t calls = rec.callCount.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (calls >= options.hotnessThreshold) {
                std::lock_guard<std::mutex> lock(tierPromotionMutex);
                (void)promoteFunctionToNativeLocked(rec, nullptr);
                currentTier = rec.tier.load(std::memory_order_acquire);
            }
        }

        if (currentTier == StencilTier::InterpreterFallback) {
            return invokeFallbackInterpreter(symbolId, arguments, std::move(execOptions));
        }

        const auto& fnLoc = *rec.location;
        const auto& bcFunc = *fnLoc.function;
        const auto& execFunc = *rec.execFunc;

        if (execOptions.limits.recursionLimit == 0) {
            return invokeFallbackInterpreter(symbolId, arguments, std::move(execOptions));
        }

        bool allIntArgsFit32 = true;
        runtime::ErrorCode argErrCode = runtime::ErrorCode::None;
        std::string argErrMsg;
        if (!validateJitEntryArguments(bcFunc, heap.get(), arguments, allIntArgsFit32, argErrCode, argErrMsg)) {
            runtime::ExecutionResult argFail;
            argFail.error.code = argErrCode;
            argFail.error.message = std::move(argErrMsg);
            runtime::DeterminismSession det(std::move(execOptions.determinism));
            (void)det.finish(false, runtime::Value{}, argFail.error, argFail.statistics);
            argFail.determinismDigest = det.digest();
            return argFail;
        }
        if (!allIntArgsFit32) {
            return invokeFallbackInterpreter(symbolId, arguments, std::move(execOptions));
        }

        runtime::detail::ActiveMutatorScope activeMutatorScope;

        std::vector<runtime::Value> localFallbackArena;
        bool usingTls = false;
        std::vector<runtime::Value>* arenaPtr = &localFallbackArena;
        if (!g_stencilTlsScratch.inUse) {
            g_stencilTlsScratch.inUse = true;
            usingTls = true;
            arenaPtr = &g_stencilTlsScratch.values;
        }
        struct TlsReleaseGuard {
            bool active = false;
            ~TlsReleaseGuard() noexcept {
                if (active) {
                    if (g_stencilTlsScratch.values.size() > 4096) {
                        g_stencilTlsScratch.values.resize(256);
                    }
                    g_stencilTlsScratch.inUse = false;
                }
            }
        } tlsGuard{usingTls};

        const std::size_t frameSlots = execFunc.totalFrameSlots;
        if (baseFrameOffset > runtime::StructStorageHandle::FrameOffsetMask ||
            frameSlots > runtime::StructStorageHandle::FrameOffsetMask - baseFrameOffset) {
            runtime::ExecutionResult invalid;
            invalid.error.code = runtime::ErrorCode::InvalidProgram;
            invalid.error.message = "frameOffset exceeds 24-bit FlatStruct slice address space";
            return invalid;
        }
        const std::size_t totalSlots = baseFrameOffset + frameSlots;
        if (arenaPtr->size() < totalSlots) {
            arenaPtr->resize(std::max<std::size_t>(totalSlots, 256));
        }
        if (baseFrameOffset > 0) {
            std::memset(
                static_cast<void*>(arenaPtr->data()),
                0,
                baseFrameOffset * sizeof(runtime::Value));
        }

        runtime::Value* frameBase = arenaPtr->data() + baseFrameOffset;
        const std::size_t argCount = arguments.size();
        for (std::size_t i = 0; i < argCount; ++i) {
            frameBase[i] = arguments[i];
        }
        if (frameSlots > argCount) {
            std::memset(
                static_cast<void*>(frameBase + argCount),
                0,
                (frameSlots - argCount) * sizeof(runtime::Value));
        }

        JitRuntimeSession session{};
        session.program = program.get();
        session.heap = heap.get();
        session.limits = execOptions.limits;
        session.stats.functionCalls = 1;
        session.stats.maximumCallDepth = 1;
        session.values = arenaPtr;
        session.frameOffset = baseFrameOffset;
        session.valueTop = totalSlots;
        session.callDepth = 1;
        session.valuesBase = arenaPtr->data();
        session.frameOffset32 = static_cast<std::uint32_t>(baseFrameOffset);
        session.flatBankId = runtime::detail::acquireFlatStackBank(arenaPtr);
        struct JitFlatBankGuard {
            std::uint8_t& bankId;
            ~JitFlatBankGuard() noexcept {
                if (bankId != 0u) {
                    runtime::detail::releaseFlatStackBank(bankId);
                    bankId = 0u;
                }
            }
        } jitBankGuard{session.flatBankId};

        if (execFunc.hasStructPayloads && session.flatBankId != 0u) {
            for (std::size_t i = 0; i < argCount; ++i) {
                const auto pSlot = static_cast<std::uint16_t>(execFunc.paramOffset + i);
                if (pSlot < execFunc.slotStructSchemaIds.size() && execFunc.slotStructSchemaIds[pSlot] != 0u) {
                    const std::uint16_t sid = execFunc.slotStructSchemaIds[pSlot];
                    const std::uint16_t payOff = execFunc.slotPayloadOffsets[pSlot];
                    const auto* sc = runtime::getFlatStructSchema(sid);
                    if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg) {
                        const runtime::Value incoming = frameBase[pSlot];
                        if (runtime::extractStructLeavesToBuffer(incoming, *sc, frameBase + payOff)) {
                            frameBase[pSlot] = runtime::StructValue::makeFrameSlice(
                                sc->structTypeId,
                                session.flatBankId,
                                static_cast<std::uint32_t>(baseFrameOffset + payOff),
                                sid);
                        } else if (incoming.rawTag() == runtime::ValueTag::Monostate) {
                            std::memcpy(
                                static_cast<void*>(frameBase + payOff),
                                static_cast<const void*>(sc->prototypeLeaves.data()),
                                static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                            frameBase[pSlot] = runtime::StructValue::makeFrameSlice(
                                sc->structTypeId,
                                session.flatBankId,
                                static_cast<std::uint32_t>(baseFrameOffset + payOff),
                                sid);
                        }
                    }
                }
            }
            const std::size_t nLocals = bcFunc.localTypes.size();
            for (std::size_t l = 0; l < nLocals; ++l) {
                const auto lSlot = static_cast<std::uint16_t>(execFunc.localOffset + l);
                if (lSlot < execFunc.slotStructSchemaIds.size() && execFunc.slotStructSchemaIds[lSlot] != 0u) {
                    const std::uint16_t sid = execFunc.slotStructSchemaIds[lSlot];
                    const std::uint16_t payOff = execFunc.slotPayloadOffsets[lSlot];
                    const auto* sc = runtime::getFlatStructSchema(sid);
                    if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg) {
                        std::memcpy(
                            static_cast<void*>(frameBase + payOff),
                            static_cast<const void*>(sc->prototypeLeaves.data()),
                            static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                        frameBase[lSlot] = runtime::StructValue::makeFrameSlice(
                            sc->structTypeId,
                            session.flatBankId,
                            static_cast<std::uint32_t>(baseFrameOffset + payOff),
                            sid);
                    }
                }
            }
        }
        session.shadowStack.pushFrame(nullptr, nullptr, arenaPtr, &session.valueTop);

        runtime::Value retValue{};
        JitAbiContext abi{};
        abi.frameBase = frameBase;
        abi.remainingBudget = execOptions.limits.instructionBudget;
        abi.branchesTaken = 0;
        abi.returnSlot = &retValue;
        abi.errorFlatIndex = 0;
        abi.errorCodeRaw = 0;
        abi.session = &session;
        abi.compiledFn = &rec;
        invalidateJitArrayCaches(abi);

        std::uint32_t rawStatus = 0u;
        std::string simDiag;
        if (forceSimulateArm64) {
#if UINTPTR_MAX == 0xffffffffffffffffULL
            const auto arm64Code = compileFunctionA64(rec);
            if (arm64Code.empty()) {
                simDiag = "compileFunctionA64 returned empty machine code";
                rawStatus = static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
            } else {
                rawStatus = executeSimulatedArm64Code(arm64Code, &abi, simDiag);
            }
#else
            simDiag = "ARM64 StencilJit requires 64-bit host pointers";
            rawStatus = static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
#endif
        } else if (currentTier == StencilTier::NativeMachineCode) {
            const NativeJitEntryFn fn = rec.nativeEntry.load(std::memory_order_acquire);
            rawStatus = fn(&abi);
        } else {
            const ThreadedStencilOp* pc = rec.threadedStream.data() + execFunc.entryStreamOffset;
            StencilStepStatus st = StencilStepStatus::Continue;
            while ((st = pc->handler(&abi, pc)) == StencilStepStatus::Continue) {}
            rawStatus = (st == StencilStepStatus::ReturnOk) ? 0u : abi.errorCodeRaw;
        }

        retValue = runtime::detachFrameSliceValue(retValue);
        session.shadowStack.popFrame();

        const std::uint64_t executed =
            execOptions.limits.instructionBudget >= abi.remainingBudget
                ? (execOptions.limits.instructionBudget - abi.remainingBudget)
                : execOptions.limits.instructionBudget;
        session.stats.instructionsExecuted = executed;
        session.stats.branchesTaken = abi.branchesTaken;

        bool succeeded = (rawStatus == 0u);
        if (!succeeded && session.error.code == runtime::ErrorCode::None) {
            const auto errCode = static_cast<runtime::ErrorCode>(rawStatus);
            const char* msg = "runtime error in StencilJit execution";
            switch (errCode) {
            case runtime::ErrorCode::InstructionBudgetExceeded:
                msg = "instruction budget exceeded";
                break;
            case runtime::ErrorCode::IntegerOverflow:
                msg = "integer overflow";
                break;
            case runtime::ErrorCode::DivisionByZero:
                msg = "division by zero";
                break;
            case runtime::ErrorCode::TypeMismatch:
                msg = "expected matching scalar type in StencilJit instruction";
                break;
            default:
                break;
            }
            populateJitErrorDiagnostics(
                session,
                fnLoc,
                execFunc,
                abi.errorFlatIndex,
                errCode,
                simDiag.empty() ? std::string(msg) : simDiag);
        }

        runtime::DeterminismSession det(std::move(execOptions.determinism));
        succeeded = det.finish(succeeded, retValue, session.error, session.stats);

        runtime::ExecutionResult out;
        out.succeeded = succeeded;
        out.value = std::move(retValue);
        out.error = std::move(session.error);
        out.instructionsExecuted = executed;
        out.statistics = session.stats;
        out.determinismDigest = det.digest();
        out.replayEntriesConsumed = det.replayEntriesConsumed();
        return out;
    }
};

StencilModule::StencilModule() = default;
StencilModule::StencilModule(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StencilModule::~StencilModule() = default;
StencilModule::StencilModule(StencilModule&&) noexcept = default;
StencilModule& StencilModule::operator=(StencilModule&&) noexcept = default;

bool StencilModule::valid() const noexcept {
    return impl_ && impl_->program != nullptr;
}

const runtime::ProgramImage* StencilModule::program() const noexcept {
    return impl_ ? impl_->program.get() : nullptr;
}

std::shared_ptr<const runtime::ProgramImage> StencilModule::programSnapshot() const noexcept {
    return impl_ ? impl_->program : nullptr;
}

void StencilModule::setBindings(std::shared_ptr<const runtime::BindingRegistry> bindings) {
    if (impl_) impl_->bindings = std::move(bindings);
}

void StencilModule::setBindingRegistry(std::shared_ptr<const runtime::BindingRegistry> bindings) {
    setBindings(std::move(bindings));
}

void StencilModule::setExternalResolver(runtime::ExternalFunction resolver) {
    if (impl_) impl_->externalResolver = std::move(resolver);
}

void StencilModule::setHeap(std::shared_ptr<runtime::ManagedHeap> heap) {
    if (!impl_) return;
    impl_->heap = heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>();
    if (impl_->asyncRuntime) {
        impl_->asyncRuntime->setHeap(impl_->heap.get());
    }
}

std::shared_ptr<runtime::ManagedHeap> StencilModule::heap() const noexcept {
    return impl_ ? impl_->heap : nullptr;
}

void StencilModule::setAsyncRuntime(std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime) {
    if (!impl_) return;
    impl_->asyncRuntime = asyncRuntime
        ? std::move(asyncRuntime)
        : std::make_shared<runtime::AsyncRuntimeContext>(impl_->heap.get());
    if (impl_->heap && impl_->asyncRuntime->awaitables().heap() == nullptr) {
        impl_->asyncRuntime->setHeap(impl_->heap.get());
    }
}

std::shared_ptr<runtime::AsyncRuntimeContext> StencilModule::asyncRuntime() const noexcept {
    return impl_ ? impl_->asyncRuntime : nullptr;
}

runtime::ExecutionResult StencilModule::invoke(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult invalid;
        invalid.error.code = runtime::ErrorCode::InvalidProgram;
        invalid.error.message = "StencilJit module is not loaded";
        return invalid;
    }
    if (const auto symId = impl_->program->findFunction(qualifiedName)) {
        return impl_->invokeBySymbol(*symId, arguments, std::move(options));
    }
    runtime::ExecutionResult missing;
    missing.error.code = runtime::ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

runtime::ExecutionResult StencilModule::invoke(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::Limits limits) const {
    runtime::ExecutionOptions opts;
    opts.limits = limits;
    return invoke(qualifiedName, arguments, std::move(opts));
}

runtime::ExecutionResult StencilModule::invoke(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult invalid;
        invalid.error.code = runtime::ErrorCode::InvalidProgram;
        invalid.error.message = "StencilJit module is not loaded";
        return invalid;
    }
    return impl_->invokeBySymbol(symbolId, arguments, std::move(options));
}

runtime::ExecutionResult StencilModule::invoke(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::Limits limits) const {
    runtime::ExecutionOptions opts;
    opts.limits = limits;
    return invoke(symbolId, arguments, std::move(opts));
}

std::size_t StencilModule::totalFunctionCount() const noexcept {
    return impl_ ? impl_->records.size() : 0;
}

std::size_t StencilModule::nativeFunctionCount() const noexcept {
    return impl_ ? impl_->nativeCount.load(std::memory_order_relaxed) : 0;
}

std::size_t StencilModule::threadedFunctionCount() const noexcept {
    return impl_ ? impl_->threadedCount.load(std::memory_order_relaxed) : 0;
}

std::size_t StencilModule::fallbackFunctionCount() const noexcept {
    return impl_ ? impl_->fallbackCount.load(std::memory_order_relaxed) : 0;
}

std::size_t StencilModule::machineCodeBytes() const noexcept {
    return impl_ ? impl_->codeBytes.load(std::memory_order_relaxed) : 0;
}

bool StencilModule::nativeBackendActive() const noexcept {
    return impl_ && impl_->nativeBackendAvailable;
}

StencilTier StencilModule::functionTier(semantic::SymbolId symbolId) const noexcept {
    if (!impl_) return StencilTier::InterpreterFallback;
    const auto it = impl_->records.find(symbolId);
    return it == impl_->records.end()
        ? StencilTier::InterpreterFallback
        : it->second->tier.load(std::memory_order_acquire);
}

StencilTier StencilModule::functionTier(const std::string& qualifiedName) const noexcept {
    if (!valid()) return StencilTier::InterpreterFallback;
    const auto sym = impl_->program->findFunction(qualifiedName);
    return sym ? functionTier(*sym) : StencilTier::InterpreterFallback;
}

StencilTargetArch StencilModule::hostTargetArch() const noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return StencilTargetArch::Arm64;
#elif defined(__x86_64__) || defined(_M_X64)
    return StencilTargetArch::X86_64;
#else
    return StencilTargetArch::Host;
#endif
}

std::vector<std::uint8_t> StencilModule::inspectMachineCode(
    const std::string& qualifiedName) const {
    return inspectMachineCode(qualifiedName, StencilTargetArch::Host);
}

std::vector<std::uint8_t> StencilModule::inspectMachineCode(
    const std::string& qualifiedName,
    StencilTargetArch targetArch) const {
    if (!valid()) return {};
    const auto sym = impl_->program->findFunction(qualifiedName);
    if (!sym) return {};
    const auto it = impl_->records.find(*sym);
    if (it == impl_->records.end() || !it->second) {
        return {};
    }
    const CompiledFunctionRecord& rec = *it->second;
    if (targetArch == StencilTargetArch::Host || targetArch == hostTargetArch()) {
        if (rec.rawCodePtr && rec.rawCodeSize > 0) {
            return std::vector<std::uint8_t>(rec.rawCodePtr, rec.rawCodePtr + rec.rawCodeSize);
        }
    }
#if UINTPTR_MAX == 0xffffffffffffffffULL
    if (!rec.eligibleForNative || rec.execFunc == nullptr) {
        return {};
    }
    if (targetArch == StencilTargetArch::Arm64) {
        return compileFunctionA64(rec);
    }
    if (targetArch == StencilTargetArch::X86_64) {
        return compileFunctionX64(rec);
    }
#endif
    return {};
}

runtime::ExecutionResult StencilModule::invokeSimulatedArm64(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult invalid;
        invalid.error.code = runtime::ErrorCode::InvalidProgram;
        invalid.error.message = "StencilJit module is not loaded";
        return invalid;
    }
    if (const auto symId = impl_->program->findFunction(qualifiedName)) {
        return impl_->invokeBySymbol(*symId, arguments, std::move(options), true);
    }
    runtime::ExecutionResult missing;
    missing.error.code = runtime::ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

runtime::ExecutionResult StencilModule::invokeWithBaseFrameOffset(
    const std::string& qualifiedName,
    std::size_t baseFrameOffset,
    bool simulateArm64,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult invalid;
        invalid.error.code = runtime::ErrorCode::InvalidProgram;
        invalid.error.message = "StencilJit module is not loaded";
        return invalid;
    }
    if (const auto symId = impl_->program->findFunction(qualifiedName)) {
        return impl_->invokeBySymbol(*symId, arguments, std::move(options), simulateArm64, baseFrameOffset);
    }
    runtime::ExecutionResult missing;
    missing.error.code = runtime::ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

bool StencilJit::nativeAvailable() noexcept {
    return ValueLayoutProbe::verify() &&
           ExecutableCodeArena::nativeCodeExecutionSupported();
}

StencilCompileResult StencilJit::compile(
    std::vector<bytecode::Module> modules,
    StencilJitOptions options) const {
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(modules), linkErr);
    if (!linked) {
        StencilCompileResult failed;
        failed.error = linkErr.message.empty()
            ? "failed to link bytecode modules for StencilJit"
            : linkErr.message;
        return failed;
    }
    return compile(std::make_shared<const runtime::ProgramImage>(std::move(*linked)), options);
}

StencilCompileResult StencilJit::compile(
    std::shared_ptr<const runtime::ProgramImage> program,
    StencilJitOptions options) const {
    const auto tStart = std::chrono::steady_clock::now();
    StencilCompileResult result;

    if (!program) {
        result.error = "StencilJit::compile received a null ProgramImage";
        return result;
    }
    if (!ValueLayoutProbe::verify()) {
        result.error = "runtime::Value ABI layout verification failed";
        return result;
    }

    auto impl = std::make_unique<StencilModule::Impl>(program, options);
    const bool canEmitNative =
        options.enableNativeCodegen && ExecutableCodeArena::nativeCodeExecutionSupported();
    impl->nativeBackendAvailable = canEmitNative;

    const auto& mirror = runtime::detail::executionMirror(*program);
    std::vector<CompiledFunctionRecord*> eagerNativeQueue;

    for (const auto& module : program->modules()) {
        for (const auto& func : module.functions) {
            const auto* fnLoc = program->resolveFunction(func.symbolId);
            if (!fnLoc) continue;
            const auto* execFunc = runtime::detail::findExecutionFunction(mirror, func);

            auto rec = std::make_unique<CompiledFunctionRecord>();
            rec->symbolId = func.symbolId;
            rec->location = fnLoc;
            rec->execFunc = execFunc;

            if (execFunc != nullptr && isFunctionEligibleForStencilJit(*execFunc)) {
                rec->unexecutedAfterOp.resize(execFunc->flatStream.size(), 0u);
                for (std::size_t idx = 0; idx < execFunc->flatStream.size(); ++idx) {
                    rec->unexecutedAfterOp[idx] = computeUnexecutedAfterOp(*execFunc, idx);
                }
                rec->eligibleForNative = true;
                rec->eligibleForThreaded = options.enableThreadedFallback;

                if (canEmitNative && options.hotnessThreshold == 0) {
                    eagerNativeQueue.push_back(rec.get());
                } else if (options.enableThreadedFallback) {
                    buildThreadedStream(*rec);
                    rec->tier.store(StencilTier::ThreadedStencils, std::memory_order_relaxed);
                } else {
                    rec->tier.store(StencilTier::InterpreterFallback, std::memory_order_relaxed);
                }
            } else {
                rec->tier.store(StencilTier::InterpreterFallback, std::memory_order_relaxed);
            }

            impl->records.emplace(func.symbolId, std::move(rec));
        }
    }

    if (!eagerNativeQueue.empty()) {
        for (auto* rec : eagerNativeQueue) {
            if (!impl->promoteFunctionToNativeLocked(*rec, &impl->primaryArena)) {
                if (options.enableThreadedFallback) {
                    buildThreadedStream(*rec);
                    rec->tier.store(StencilTier::ThreadedStencils, std::memory_order_relaxed);
                } else {
                    rec->tier.store(StencilTier::InterpreterFallback, std::memory_order_relaxed);
                }
            }
        }
        if (impl->primaryArena.allocatedBytes() > 0 && !impl->primaryArena.seal()) {
            // Restricted W^X environment at runtime: gracefully downgrade all functions to ThreadedStencils / Fallback!
            impl->nativeBackendAvailable = false;
            for (auto* rec : eagerNativeQueue) {
                rec->nativeEntry.store(nullptr, std::memory_order_relaxed);
                rec->rawCodePtr = nullptr;
                rec->rawCodeSize = 0;
                if (options.enableThreadedFallback) {
                    buildThreadedStream(*rec);
                    rec->tier.store(StencilTier::ThreadedStencils, std::memory_order_relaxed);
                } else {
                    rec->tier.store(StencilTier::InterpreterFallback, std::memory_order_relaxed);
                }
            }
        }
    }

    std::size_t nNative = 0;
    std::size_t nThreaded = 0;
    std::size_t nFallback = 0;
    for (const auto& [sym, rec] : impl->records) {
        (void)sym;
        switch (rec->tier.load(std::memory_order_relaxed)) {
        case StencilTier::NativeMachineCode: ++nNative; break;
        case StencilTier::ThreadedStencils: ++nThreaded; break;
        case StencilTier::InterpreterFallback: ++nFallback; break;
        }
    }
    const std::size_t codeSize = impl->nativeBackendAvailable
        ? impl->primaryArena.allocatedBytes()
        : 0u;
    impl->nativeCount.store(nNative, std::memory_order_relaxed);
    impl->threadedCount.store(nThreaded, std::memory_order_relaxed);
    impl->fallbackCount.store(nFallback, std::memory_order_relaxed);
    impl->codeBytes.store(codeSize, std::memory_order_relaxed);

    const auto tStop = std::chrono::steady_clock::now();
    result.compileTimeMicroseconds =
        std::chrono::duration<double, std::micro>(tStop - tStart).count();
    result.totalFunctionCount = impl->records.size();
    result.nativeFunctionCount = nNative;
    result.threadedFunctionCount = nThreaded;
    result.fallbackFunctionCount = nFallback;
    result.machineCodeBytes = codeSize;
    result.nativeBackendActive = impl->nativeBackendAvailable;
    result.module = std::shared_ptr<StencilModule>(new StencilModule(std::move(impl)));
    return result;
}

} // namespace realscript::jit
