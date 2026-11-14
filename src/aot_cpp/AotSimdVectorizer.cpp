#include "AotCppInternal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace realscript::aot::detail {

namespace {

bool isPureVectorizableBodyInstruction(const mir::Instruction& instruction) noexcept {
    switch (instruction.opcode) {
    case mir::Opcode::ConstantInt:
    case mir::Opcode::ConstantDouble:
    case mir::Opcode::ConstantBool:
    case mir::Opcode::LoadLocal:
    case mir::Opcode::StoreLocal:
    case mir::Opcode::ConvertIntToLong:
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt:
    case mir::Opcode::NegateInt:
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong:
    case mir::Opcode::NegateLong:
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble:
    case mir::Opcode::NegateDouble:
    case mir::Opcode::LogicalNot:
    case mir::Opcode::Equal:
    case mir::Opcode::NotEqual:
    case mir::Opcode::LessInt:
    case mir::Opcode::LessOrEqualInt:
    case mir::Opcode::GreaterInt:
    case mir::Opcode::GreaterOrEqualInt:
    case mir::Opcode::LessLong:
    case mir::Opcode::LessOrEqualLong:
    case mir::Opcode::GreaterLong:
    case mir::Opcode::GreaterOrEqualLong:
    case mir::Opcode::LessDouble:
    case mir::Opcode::LessOrEqualDouble:
    case mir::Opcode::GreaterDouble:
    case mir::Opcode::GreaterOrEqualDouble:
    case mir::Opcode::NewStruct:
    case mir::Opcode::LoadStructField:
    case mir::Opcode::StoreStructField:
        return true;
    default:
        return false;
    }
}

bool isScalarNumericArrayElemType(semantic::PrimitiveType type) noexcept {
    switch (type) {
    case semantic::PrimitiveType::Bool:
    case semantic::PrimitiveType::Byte:
    case semantic::PrimitiveType::SByte:
    case semantic::PrimitiveType::Short:
    case semantic::PrimitiveType::UShort:
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::UInt:
    case semantic::PrimitiveType::Long:
    case semantic::PrimitiveType::ULong:
    case semantic::PrimitiveType::Float:
    case semantic::PrimitiveType::Double:
        return true;
    default:
        return false;
    }
}

bool isSubwordIntType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Byte ||
        type == semantic::PrimitiveType::SByte ||
        type == semantic::PrimitiveType::Short ||
        type == semantic::PrimitiveType::UShort;
}

std::string packedElemCppType(semantic::PrimitiveType type) {
    switch (type) {
    case semantic::PrimitiveType::Bool:   return "std::uint8_t";
    case semantic::PrimitiveType::Byte:   return "std::uint8_t";
    case semantic::PrimitiveType::SByte:  return "std::int8_t";
    case semantic::PrimitiveType::Short:  return "std::int16_t";
    case semantic::PrimitiveType::UShort: return "std::uint16_t";
    case semantic::PrimitiveType::Int:    return "std::int32_t";
    case semantic::PrimitiveType::UInt:   return "std::uint32_t";
    case semantic::PrimitiveType::Long:   return "std::int64_t";
    case semantic::PrimitiveType::ULong:  return "std::uint64_t";
    case semantic::PrimitiveType::Float:  return "float";
    case semantic::PrimitiveType::Double: return "double";
    default:                              return "std::int32_t";
    }
}

std::string packedElemEncodingExpr(semantic::PrimitiveType type) {
    switch (type) {
    case semantic::PrimitiveType::Bool:   return "runtime::PackedElemEncoding::Bool";
    case semantic::PrimitiveType::Byte:   return "runtime::PackedElemEncoding::Byte";
    case semantic::PrimitiveType::SByte:  return "runtime::PackedElemEncoding::SByte";
    case semantic::PrimitiveType::Short:  return "runtime::PackedElemEncoding::Short";
    case semantic::PrimitiveType::UShort: return "runtime::PackedElemEncoding::UShort";
    case semantic::PrimitiveType::Int:    return "runtime::PackedElemEncoding::Int32";
    case semantic::PrimitiveType::UInt:   return "runtime::PackedElemEncoding::UInt32";
    case semantic::PrimitiveType::Long:   return "runtime::PackedElemEncoding::Int64";
    case semantic::PrimitiveType::ULong:  return "runtime::PackedElemEncoding::UInt64";
    case semantic::PrimitiveType::Float:  return "runtime::PackedElemEncoding::Float32";
    case semantic::PrimitiveType::Double: return "runtime::PackedElemEncoding::Float64";
    default:                              return "runtime::PackedElemEncoding::None";
    }
}

bool supportsSimdConvertNumeric(
    semantic::PrimitiveType from,
    semantic::PrimitiveType to,
    bool checked) noexcept {
    if (!isScalarNumericArrayElemType(from) || !isScalarNumericArrayElemType(to)) {
        return false;
    }
    if (from == semantic::PrimitiveType::Bool || to == semantic::PrimitiveType::Bool) {
        return from == to;
    }
    if (from == to) {
        return true;
    }
    if (from == semantic::PrimitiveType::Float && to == semantic::PrimitiveType::Double) {
        return true;
    }
    if (from == semantic::PrimitiveType::Double && to == semantic::PrimitiveType::Float) {
        return !checked;
    }
    // Avoid unchecked float->int C++ UB on NaN/Inf/out-of-range vs runtime::tryConvertNumeric.
    if (from == semantic::PrimitiveType::Float || from == semantic::PrimitiveType::Double) {
        return false;
    }
    if (!checked) {
        return true;
    }
    // Checked integer->float/double and widening integer->integer conversions never overflow:
    if (to == semantic::PrimitiveType::Float || to == semantic::PrimitiveType::Double) {
        return true;
    }
    if (from == semantic::PrimitiveType::Byte &&
        (to == semantic::PrimitiveType::UShort || to == semantic::PrimitiveType::Short ||
         to == semantic::PrimitiveType::Int || to == semantic::PrimitiveType::UInt ||
         to == semantic::PrimitiveType::Long || to == semantic::PrimitiveType::ULong)) {
        return true;
    }
    if (from == semantic::PrimitiveType::SByte &&
        (to == semantic::PrimitiveType::Short || to == semantic::PrimitiveType::Int ||
         to == semantic::PrimitiveType::Long)) {
        return true;
    }
    if (from == semantic::PrimitiveType::Short &&
        (to == semantic::PrimitiveType::Int || to == semantic::PrimitiveType::Long)) {
        return true;
    }
    if (from == semantic::PrimitiveType::UShort &&
        (to == semantic::PrimitiveType::Int || to == semantic::PrimitiveType::UInt ||
         to == semantic::PrimitiveType::Long || to == semantic::PrimitiveType::ULong)) {
        return true;
    }
    if (from == semantic::PrimitiveType::Int && to == semantic::PrimitiveType::Long) {
        return true;
    }
    if (from == semantic::PrimitiveType::UInt &&
        (to == semantic::PrimitiveType::Long || to == semantic::PrimitiveType::ULong)) {
        return true;
    }
    return false;
}

struct PackedLoopContextMaps {
    std::unordered_map<mir::BlockId, std::size_t> blockIndexById;
    std::vector<std::vector<bool>> dominates;
    std::unordered_map<mir::ValueId, std::int64_t> constantInts;
    std::unordered_map<mir::ValueId, mir::BlockId> valueDefBlock;
    std::unordered_map<mir::ValueId, const mir::Instruction*> valueDefInst;
    std::unordered_map<mir::ValueId, std::size_t> paramIndexByValue;

    [[nodiscard]] bool strictlyDominates(mir::BlockId defBlk, mir::BlockId useBlk) const noexcept {
        if (defBlk == useBlk) return false;
        const auto dIt = blockIndexById.find(defBlk);
        const auto uIt = blockIndexById.find(useBlk);
        if (dIt == blockIndexById.end() || uIt == blockIndexById.end()) return false;
        return dominates[uIt->second][dIt->second];
    }
};

PackedLoopContextMaps buildPackedLoopContextMaps(const mir::Function& function) {
    PackedLoopContextMaps ctx;
    const std::size_t n = function.blocks.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& blk = function.blocks[i];
        ctx.blockIndexById.emplace(blk.id, i);
        for (const auto& bp : blk.parameters) {
            if (bp.value >= 0) ctx.valueDefBlock[bp.value] = blk.id;
        }
        for (const auto& inst : blk.instructions) {
            if (inst.result >= 0) {
                ctx.valueDefBlock[inst.result] = blk.id;
                ctx.valueDefInst[inst.result] = &inst;
            }
            if (inst.opcode == mir::Opcode::ConstantInt &&
                inst.result >= 0 &&
                inst.resultType != semantic::PrimitiveType::Enum) {
                ctx.constantInts[inst.result] = inst.integerImmediate;
            } else if (inst.opcode == mir::Opcode::Parameter && inst.result >= 0) {
                ctx.paramIndexByValue[inst.result] = static_cast<std::size_t>(inst.integerImmediate);
            }
        }
    }

    std::vector<std::vector<std::size_t>> preds(n);
    const auto addEdge = [&](std::size_t fromIdx, mir::BlockId toId) {
        const auto it = ctx.blockIndexById.find(toId);
        if (it != ctx.blockIndexById.end()) {
            preds[it->second].push_back(fromIdx);
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto& term = function.blocks[i].terminator;
        if (term.kind == mir::TerminatorKind::Jump) {
            addEdge(i, term.target);
        } else if (term.kind == mir::TerminatorKind::Branch) {
            addEdge(i, term.target);
            addEdge(i, term.falseTarget);
        }
    }

    ctx.dominates.assign(n, std::vector<bool>(n, true));
    if (n > 0) {
        std::fill(ctx.dominates[0].begin(), ctx.dominates[0].end(), false);
        ctx.dominates[0][0] = true;
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t i = 1; i < n; ++i) {
                if (preds[i].empty()) {
                    std::vector<bool> emptySet(n, false);
                    emptySet[i] = true;
                    if (ctx.dominates[i] != emptySet) {
                        ctx.dominates[i] = std::move(emptySet);
                        changed = true;
                    }
                    continue;
                }
                std::vector<bool> next(n, true);
                for (const std::size_t p : preds[i]) {
                    for (std::size_t k = 0; k < n; ++k) {
                        if (!ctx.dominates[p][k]) next[k] = false;
                    }
                }
                next[i] = true;
                if (next != ctx.dominates[i]) {
                    ctx.dominates[i] = std::move(next);
                    changed = true;
                }
            }
        }
    }
    return ctx;
}

struct PackedAffineTerm {
    bool subtract = false;
    std::string canonicalKey;
    std::string cppExpr;
};

struct PackedOffsetSpec {
    std::int64_t constDelta = 0;
    std::vector<PackedAffineTerm> terms;
    std::string canonicalKey;
    std::string cppOffsetExpr;

    [[nodiscard]] bool isZero() const noexcept {
        return constDelta == 0 && terms.empty();
    }
};

struct PackedArrayBindingInfo {
    std::string canonicalKey;
    semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Error;
    bool isRead = false;
    bool isWrite = false;
    std::string precomputedCacheName;
    std::string arrayValueCppExpr;
    std::vector<PackedOffsetSpec> offsets;
    std::unordered_map<std::string, std::size_t> offsetIndexByKey;
};

struct PackedLoopBodyAnalysis {
    bool valid = false;
    bool hasElementAccess = false;
    std::vector<PackedArrayBindingInfo> bindings;
    std::unordered_map<std::string, std::size_t> bindingIndexByKey;
    // Map bodyBlock instruction index -> (bindingIdx, offsetIdx)
    std::unordered_map<std::size_t, std::pair<std::size_t, std::size_t>> instElementAccess;
    std::vector<bool> skipBodyInst;
    std::vector<PackedOffsetSpec> checkedPartialSteps;
    std::unordered_map<mir::ValueId, std::size_t> bodyLocalArrayLoadRegs;
    std::unordered_set<mir::ValueId> bodyIndexLoadRegs;
};

PackedOffsetSpec canonicalizePackedOffset(
    std::int64_t constDelta,
    std::vector<PackedAffineTerm> terms) {
    // Cancel identical positive and negative invariant terms.
    std::vector<PackedAffineTerm> cancelled;
    cancelled.reserve(terms.size());
    for (auto& t : terms) {
        bool canceled = false;
        for (std::size_t j = 0; j < cancelled.size(); ++j) {
            if (cancelled[j].canonicalKey == t.canonicalKey &&
                cancelled[j].subtract != t.subtract) {
                cancelled.erase(cancelled.begin() + static_cast<std::ptrdiff_t>(j));
                canceled = true;
                break;
            }
        }
        if (!canceled) {
            cancelled.push_back(std::move(t));
        }
    }
    std::sort(
        cancelled.begin(),
        cancelled.end(),
        [](const PackedAffineTerm& a, const PackedAffineTerm& b) {
            if (a.canonicalKey != b.canonicalKey) return a.canonicalKey < b.canonicalKey;
            return static_cast<int>(a.subtract) < static_cast<int>(b.subtract);
        });

    PackedOffsetSpec spec;
    spec.constDelta = constDelta;
    spec.terms = std::move(cancelled);
    std::ostringstream keyOs;
    keyOs << spec.constDelta;
    for (const auto& t : spec.terms) {
        keyOs << (t.subtract ? '-' : '+') << t.canonicalKey;
    }
    spec.canonicalKey = keyOs.str();

    if (spec.isZero()) {
        spec.cppOffsetExpr = "std::int64_t{0}";
    } else {
        std::ostringstream exprOs;
        bool first = true;
        for (const auto& t : spec.terms) {
            if (first) {
                if (t.subtract) exprOs << '-';
            } else {
                exprOs << (t.subtract ? " - " : " + ");
            }
            exprOs << "static_cast<std::int64_t>(" << t.cppExpr << ")";
            first = false;
        }
        if (spec.constDelta != 0 || first) {
            if (first) {
                exprOs << "std::int64_t{" << int64Literal(spec.constDelta) << "}";
            } else if (spec.constDelta > 0) {
                exprOs << " + std::int64_t{" << int64Literal(spec.constDelta) << "}";
            } else {
                exprOs << " - std::int64_t{" << int64Literal(-spec.constDelta) << "}";
            }
        }
        spec.cppOffsetExpr = exprOs.str();
    }
    return spec;
}

PackedLoopBodyAnalysis analyzePackedLoopBodyDetails(
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const TypedRangeAnalysis* rangeAnalysis,
    const std::unordered_map<std::size_t, semantic::PrimitiveType>& singleStorePackedArrayLocals,
    const PackedLoopContextMaps& ctx,
    const std::unordered_set<std::size_t>& loopWrittenLocals,
    mir::BlockId headerBlockId,
    mir::BlockId bodyBlockId,
    mir::BlockId incBlockId,
    std::size_t indexLocalIndex) {
    PackedLoopBodyAnalysis result;
    const auto* bodyBlock = findBlock(function, bodyBlockId);
    if (!bodyBlock) return result;

    result.skipBodyInst.assign(bodyBlock->instructions.size(), false);
    std::unordered_map<mir::ValueId, std::size_t> bodyInstIndexByResult;
    for (std::size_t i = 0; i < bodyBlock->instructions.size(); ++i) {
        if (bodyBlock->instructions[i].result >= 0) {
            bodyInstIndexByResult[bodyBlock->instructions[i].result] = i;
        }
    }

    const auto isInsideLoop = [&](mir::BlockId bid) noexcept {
        return bid == headerBlockId || bid == bodyBlockId || bid == incBlockId;
    };

    const auto isInvariantValueDef = [&](mir::ValueId vid, mir::BlockId& outDefBlk) -> bool {
        if (vid < 0) return false;
        const auto dIt = ctx.valueDefBlock.find(vid);
        if (dIt == ctx.valueDefBlock.end()) return true;
        outDefBlk = dIt->second;
        return !isInsideLoop(outDefBlk) && ctx.strictlyDominates(outDefBlk, headerBlockId);
    };

    const auto resolveInvariantStructExpr = [&](mir::ValueId vid, std::string& outKey, std::string& outExpr) -> bool {
        if (vid < 0 || static_cast<std::size_t>(vid) >= types.size()) return false;
        const auto& slot = types[static_cast<std::size_t>(vid)];
        if (slot.primitive != semantic::PrimitiveType::Struct ||
            classifyInlineMath(semantic::PrimitiveType::Struct, slot.typeId) != InlineMathKind::None) {
            return false;
        }
        const auto dIt = ctx.valueDefBlock.find(vid);
        const auto iIt = ctx.valueDefInst.find(vid);
        if (dIt != ctx.valueDefBlock.end() && dIt->second == bodyBlockId &&
            iIt != ctx.valueDefInst.end()) {
            const auto& inst = *iIt->second;
            if (inst.opcode == mir::Opcode::LoadLocal &&
                inst.localIndex < function.localTypes.size() &&
                loopWrittenLocals.find(inst.localIndex) == loopWrittenLocals.end()) {
                outKey = "SL:" + std::to_string(inst.localIndex);
                outExpr = typedLocalExpression(inst.localIndex);
                return true;
            }
            return false;
        }
        mir::BlockId defBlk = 0;
        if (!isInvariantValueDef(vid, defBlk)) return false;
        const auto pIt = ctx.paramIndexByValue.find(vid);
        if (pIt != ctx.paramIndexByValue.end()) {
            outKey = "SP:" + std::to_string(pIt->second);
            outExpr = "typedArg_" + std::to_string(pIt->second);
            return true;
        }
        if (iIt != ctx.valueDefInst.end() &&
            iIt->second->opcode == mir::Opcode::LoadLocal &&
            iIt->second->localIndex < function.localTypes.size() &&
            loopWrittenLocals.find(iIt->second->localIndex) == loopWrittenLocals.end()) {
            outKey = "SL:" + std::to_string(iIt->second->localIndex);
            outExpr = typedLocalExpression(iIt->second->localIndex);
            return true;
        }
        outKey = "SV:" + std::to_string(vid);
        outExpr = typedValueExpression(vid);
        return true;
    };

    struct ResolvedArraySource {
        std::string canonicalKey;
        std::string precomputedCacheName;
        std::string arrayValueCppExpr;
        std::vector<std::size_t> addrBodyInstIndices;
    };

    const auto resolveArraySource = [&](mir::ValueId arrVid, semantic::PrimitiveType elemPrim, ResolvedArraySource& out) -> bool {
        if (arrVid < 0 || static_cast<std::size_t>(arrVid) >= types.size() ||
            types[static_cast<std::size_t>(arrVid)].primitive != semantic::PrimitiveType::Array) {
            return false;
        }
        const auto dIt = ctx.valueDefBlock.find(arrVid);
        const auto iIt = ctx.valueDefInst.find(arrVid);
        const bool definedInBody = (dIt != ctx.valueDefBlock.end() && dIt->second == bodyBlockId);

        if (definedInBody) {
            if (iIt == ctx.valueDefInst.end()) return false;
            const auto& inst = *iIt->second;
            const auto bodyIdxIt = bodyInstIndexByResult.find(arrVid);
            if (bodyIdxIt != bodyInstIndexByResult.end()) {
                out.addrBodyInstIndices.push_back(bodyIdxIt->second);
            }
            if (inst.opcode == mir::Opcode::LoadLocal &&
                inst.localIndex < function.localTypes.size() &&
                function.localTypes[inst.localIndex] == semantic::PrimitiveType::Array &&
                loopWrittenLocals.find(inst.localIndex) == loopWrittenLocals.end()) {
                out.canonicalKey = "L:" + std::to_string(inst.localIndex);
                const auto ssIt = singleStorePackedArrayLocals.find(inst.localIndex);
                if (ssIt != singleStorePackedArrayLocals.end() && ssIt->second == elemPrim) {
                    out.precomputedCacheName = packedArrayCacheName(inst.localIndex, elemPrim);
                    result.bodyLocalArrayLoadRegs[arrVid] = inst.localIndex;
                } else {
                    out.arrayValueCppExpr = typedLocalExpression(inst.localIndex);
                }
                return true;
            }
            if (inst.opcode == mir::Opcode::LoadStructField &&
                inst.operands.size() == 1 &&
                inst.resultType == semantic::PrimitiveType::Array) {
                std::string stKey;
                std::string stExpr;
                if (!resolveInvariantStructExpr(inst.operands[0], stKey, stExpr)) return false;
                const auto srcBodyIt = bodyInstIndexByResult.find(inst.operands[0]);
                if (srcBodyIt != bodyInstIndexByResult.end()) {
                    out.addrBodyInstIndices.push_back(srcBodyIt->second);
                }
                out.canonicalKey = stKey + ".f" + std::to_string(inst.fieldIndex);
                out.arrayValueCppExpr = stExpr + ".f_" + std::to_string(inst.fieldIndex);
                return true;
            }
            return false;
        }

        mir::BlockId defBlk = 0;
        if (!isInvariantValueDef(arrVid, defBlk)) return false;
        if (iIt != ctx.valueDefInst.end() &&
            iIt->second->opcode == mir::Opcode::LoadLocal &&
            iIt->second->localIndex < function.localTypes.size() &&
            function.localTypes[iIt->second->localIndex] == semantic::PrimitiveType::Array &&
            loopWrittenLocals.find(iIt->second->localIndex) == loopWrittenLocals.end()) {
            const std::size_t locIdx = iIt->second->localIndex;
            out.canonicalKey = "L:" + std::to_string(locIdx);
            const auto ssIt = singleStorePackedArrayLocals.find(locIdx);
            if (ssIt != singleStorePackedArrayLocals.end() && ssIt->second == elemPrim) {
                out.precomputedCacheName = packedArrayCacheName(locIdx, elemPrim);
            } else {
                out.arrayValueCppExpr = typedLocalExpression(locIdx);
            }
            return true;
        }
        const auto pIt = ctx.paramIndexByValue.find(arrVid);
        if (pIt != ctx.paramIndexByValue.end()) {
            out.canonicalKey = "P:" + std::to_string(pIt->second);
            out.arrayValueCppExpr = "typedArg_" + std::to_string(pIt->second);
            return true;
        }
        if (iIt != ctx.valueDefInst.end() &&
            iIt->second->opcode == mir::Opcode::LoadStructField &&
            iIt->second->operands.size() == 1 &&
            iIt->second->resultType == semantic::PrimitiveType::Array) {
            std::string stKey;
            std::string stExpr;
            if (resolveInvariantStructExpr(iIt->second->operands[0], stKey, stExpr)) {
                out.canonicalKey = stKey + ".f" + std::to_string(iIt->second->fieldIndex);
                out.arrayValueCppExpr = stExpr + ".f_" + std::to_string(iIt->second->fieldIndex);
                return true;
            }
        }
        out.canonicalKey = "V:" + std::to_string(arrVid);
        out.arrayValueCppExpr = typedValueExpression(arrVid);
        return true;
    };

    const auto resolveInvariantInt = [&](
        mir::ValueId vid,
        bool& outIsConst,
        std::int64_t& outConst,
        PackedAffineTerm& outTerm,
        std::vector<std::size_t>& outAddrBodyInsts) -> bool {
        if (vid < 0 || static_cast<std::size_t>(vid) >= types.size() ||
            types[static_cast<std::size_t>(vid)].primitive != semantic::PrimitiveType::Int) {
            return false;
        }
        const auto cIt = ctx.constantInts.find(vid);
        if (cIt != ctx.constantInts.end()) {
            outIsConst = true;
            outConst = cIt->second;
            const auto bIdxIt = bodyInstIndexByResult.find(vid);
            if (bIdxIt != bodyInstIndexByResult.end()) {
                outAddrBodyInsts.push_back(bIdxIt->second);
            }
            return cIt->second >= std::numeric_limits<std::int32_t>::min() &&
                cIt->second <= std::numeric_limits<std::int32_t>::max();
        }
        outIsConst = false;
        const auto dIt = ctx.valueDefBlock.find(vid);
        const auto iIt = ctx.valueDefInst.find(vid);
        if (dIt != ctx.valueDefBlock.end() && dIt->second == bodyBlockId) {
            if (iIt == ctx.valueDefInst.end()) return false;
            const auto& inst = *iIt->second;
            const auto bIdxIt = bodyInstIndexByResult.find(vid);
            if (bIdxIt != bodyInstIndexByResult.end()) {
                outAddrBodyInsts.push_back(bIdxIt->second);
            }
            if (inst.opcode == mir::Opcode::LoadLocal &&
                inst.localIndex != indexLocalIndex &&
                inst.localIndex < function.localTypes.size() &&
                function.localTypes[inst.localIndex] == semantic::PrimitiveType::Int &&
                loopWrittenLocals.find(inst.localIndex) == loopWrittenLocals.end()) {
                outTerm.canonicalKey = "IL:" + std::to_string(inst.localIndex);
                outTerm.cppExpr = typedLocalExpression(inst.localIndex);
                return true;
            }
            if (inst.opcode == mir::Opcode::LoadStructField &&
                inst.operands.size() == 1 &&
                inst.resultType == semantic::PrimitiveType::Int) {
                std::string stKey;
                std::string stExpr;
                if (!resolveInvariantStructExpr(inst.operands[0], stKey, stExpr)) return false;
                const auto srcBodyIt = bodyInstIndexByResult.find(inst.operands[0]);
                if (srcBodyIt != bodyInstIndexByResult.end()) {
                    outAddrBodyInsts.push_back(srcBodyIt->second);
                }
                outTerm.canonicalKey = "IF:" + stKey + ".f" + std::to_string(inst.fieldIndex);
                outTerm.cppExpr = stExpr + ".f_" + std::to_string(inst.fieldIndex);
                return true;
            }
            return false;
        }
        mir::BlockId defBlk = 0;
        if (!isInvariantValueDef(vid, defBlk)) return false;
        if (iIt != ctx.valueDefInst.end() &&
            iIt->second->opcode == mir::Opcode::LoadLocal &&
            iIt->second->localIndex != indexLocalIndex &&
            iIt->second->localIndex < function.localTypes.size() &&
            function.localTypes[iIt->second->localIndex] == semantic::PrimitiveType::Int &&
            loopWrittenLocals.find(iIt->second->localIndex) == loopWrittenLocals.end()) {
            outTerm.canonicalKey = "IL:" + std::to_string(iIt->second->localIndex);
            outTerm.cppExpr = typedLocalExpression(iIt->second->localIndex);
            return true;
        }
        const auto pIt = ctx.paramIndexByValue.find(vid);
        if (pIt != ctx.paramIndexByValue.end()) {
            outTerm.canonicalKey = "IP:" + std::to_string(pIt->second);
            outTerm.cppExpr = "typedArg_" + std::to_string(pIt->second);
            return true;
        }
        if (iIt != ctx.valueDefInst.end() &&
            iIt->second->opcode == mir::Opcode::LoadStructField &&
            iIt->second->operands.size() == 1 &&
            iIt->second->resultType == semantic::PrimitiveType::Int) {
            std::string stKey;
            std::string stExpr;
            if (resolveInvariantStructExpr(iIt->second->operands[0], stKey, stExpr)) {
                outTerm.canonicalKey = "IF:" + stKey + ".f" + std::to_string(iIt->second->fieldIndex);
                outTerm.cppExpr = stExpr + ".f_" + std::to_string(iIt->second->fieldIndex);
                return true;
            }
        }
        outTerm.canonicalKey = "IV:" + std::to_string(vid);
        outTerm.cppExpr = typedValueExpression(vid);
        return true;
    };

    struct LinearIndexState {
        std::int64_t constDelta = 0;
        std::vector<PackedAffineTerm> terms;
        std::vector<std::size_t> addrBodyInsts;
        std::vector<PackedOffsetSpec> checkedSteps;
    };

    const auto resolveLinearIndex = [&](
        auto& self,
        mir::ValueId vid,
        int depth,
        LinearIndexState& out) -> bool {
        if (depth > 8 || vid < 0 || static_cast<std::size_t>(vid) >= types.size()) return false;
        if (types[static_cast<std::size_t>(vid)].primitive != semantic::PrimitiveType::Int) return false;
        const auto dIt = ctx.valueDefBlock.find(vid);
        const auto iIt = ctx.valueDefInst.find(vid);
        if (dIt == ctx.valueDefBlock.end() || dIt->second != bodyBlockId || iIt == ctx.valueDefInst.end()) {
            return false;
        }
        const auto& inst = *iIt->second;
        const auto bodyIdxIt = bodyInstIndexByResult.find(vid);

        if (inst.opcode == mir::Opcode::LoadLocal) {
            if (inst.localIndex == indexLocalIndex) {
                if (bodyIdxIt != bodyInstIndexByResult.end()) {
                    out.addrBodyInsts.push_back(bodyIdxIt->second);
                }
                result.bodyIndexLoadRegs.insert(vid);
                return true;
            }
            return false;
        }
        if (inst.opcode != mir::Opcode::AddInt && inst.opcode != mir::Opcode::SubtractInt) {
            return false;
        }
        if (inst.operands.size() != 2) return false;

        auto appendInvariant = [&](mir::ValueId invVid, bool negate, LinearIndexState& state) -> bool {
            bool isConst = false;
            std::int64_t cVal = 0;
            PackedAffineTerm term{};
            if (!resolveInvariantInt(invVid, isConst, cVal, term, state.addrBodyInsts)) {
                return false;
            }
            if (isConst) {
                const std::int64_t delta = negate ? -cVal : cVal;
                const std::int64_t nextConst = state.constDelta + delta;
                if (nextConst < std::numeric_limits<std::int32_t>::min() ||
                    nextConst > std::numeric_limits<std::int32_t>::max()) {
                    return false;
                }
                state.constDelta = nextConst;
            } else {
                if (state.terms.size() >= 4u) return false;
                term.subtract = negate;
                state.terms.push_back(std::move(term));
            }
            return true;
        };

        LinearIndexState sub;
        bool resolved = false;
        if (self(self, inst.operands[0], depth + 1, sub) &&
            appendInvariant(inst.operands[1], inst.opcode == mir::Opcode::SubtractInt, sub)) {
            resolved = true;
        } else if (inst.opcode == mir::Opcode::AddInt) {
            sub = LinearIndexState{};
            if (self(self, inst.operands[1], depth + 1, sub) &&
                appendInvariant(inst.operands[0], false, sub)) {
                resolved = true;
            }
        }
        if (!resolved) return false;
        // Preserve this address instruction index after moving child state so pure
        // addressing AddInt/SubtractInt ops (including checked indexing) get skipped
        // in the SIMD compute loop and guarded via checkedPartialSteps.
        if (bodyIdxIt != bodyInstIndexByResult.end()) {
            sub.addrBodyInsts.push_back(bodyIdxIt->second);
        }
        if (inst.checkedArithmetic) {
            sub.checkedSteps.push_back(canonicalizePackedOffset(sub.constDelta, sub.terms));
        }
        out = std::move(sub);
        return true;
    };

    std::unordered_set<std::size_t> candidateAddrInstIndices;

    for (std::size_t instIdx = 0; instIdx < bodyBlock->instructions.size(); ++instIdx) {
        const auto& inst = bodyBlock->instructions[instIdx];
        for (const auto op : inst.operands) {
            const auto dIt = ctx.valueDefBlock.find(op);
            if (dIt != ctx.valueDefBlock.end() &&
                (dIt->second == headerBlockId || dIt->second == incBlockId)) {
                return result;
            }
        }

        if (inst.opcode == mir::Opcode::LoadElement || inst.opcode == mir::Opcode::StoreElement) {
            const bool isLoad = (inst.opcode == mir::Opcode::LoadElement);
            if ((isLoad && (inst.operands.size() != 2 || inst.result < 0)) ||
                (!isLoad && inst.operands.size() != 3) ||
                inst.elementTypeId != 0 ||
                !isScalarNumericArrayElemType(inst.elementType)) {
                return result;
            }
            if (isLoad) {
                const bool typeMatches = (inst.resultType == inst.elementType) ||
                    (isSubwordIntType(inst.elementType) && inst.resultType == semantic::PrimitiveType::Int);
                if (!typeMatches) return result;
            } else {
                const auto valVid = inst.operands[2];
                if (valVid < 0 || static_cast<std::size_t>(valVid) >= types.size()) return result;
                const auto valPrim = types[static_cast<std::size_t>(valVid)].primitive;
                const bool typeMatches = (valPrim == inst.elementType) ||
                    (isSubwordIntType(inst.elementType) && valPrim == semantic::PrimitiveType::Int);
                if (!typeMatches) return result;
            }

            ResolvedArraySource arrSrc;
            if (!resolveArraySource(inst.operands[0], inst.elementType, arrSrc)) {
                return result;
            }
            LinearIndexState idxState;
            if (!resolveLinearIndex(resolveLinearIndex, inst.operands[1], 0, idxState)) {
                return result;
            }
            const PackedOffsetSpec offsetSpec =
                canonicalizePackedOffset(idxState.constDelta, std::move(idxState.terms));

            for (const std::size_t ai : arrSrc.addrBodyInstIndices) candidateAddrInstIndices.insert(ai);
            for (const std::size_t ii : idxState.addrBodyInsts) {
                candidateAddrInstIndices.insert(ii);
            }
            for (auto& step : idxState.checkedSteps) {
                result.checkedPartialSteps.push_back(std::move(step));
            }

            std::size_t bIdx = 0;
            const auto bIt = result.bindingIndexByKey.find(arrSrc.canonicalKey);
            if (bIt == result.bindingIndexByKey.end()) {
                bIdx = result.bindings.size();
                result.bindingIndexByKey.emplace(arrSrc.canonicalKey, bIdx);
                PackedArrayBindingInfo bInfo;
                bInfo.canonicalKey = std::move(arrSrc.canonicalKey);
                bInfo.elemPrim = inst.elementType;
                bInfo.precomputedCacheName = std::move(arrSrc.precomputedCacheName);
                bInfo.arrayValueCppExpr = std::move(arrSrc.arrayValueCppExpr);
                result.bindings.push_back(std::move(bInfo));
            } else {
                bIdx = bIt->second;
                if (result.bindings[bIdx].elemPrim != inst.elementType) {
                    return result;
                }
            }

            auto& binding = result.bindings[bIdx];
            if (isLoad) binding.isRead = true;
            else binding.isWrite = true;

            std::size_t oIdx = 0;
            const auto oIt = binding.offsetIndexByKey.find(offsetSpec.canonicalKey);
            if (oIt == binding.offsetIndexByKey.end()) {
                oIdx = binding.offsets.size();
                binding.offsetIndexByKey.emplace(offsetSpec.canonicalKey, oIdx);
                binding.offsets.push_back(offsetSpec);
            } else {
                oIdx = oIt->second;
            }
            result.instElementAccess[instIdx] = {bIdx, oIdx};
            result.hasElementAccess = true;
        }
    }

    if (!result.hasElementAccess || result.bindings.empty()) {
        return result;
    }

    // Same-array dependence guard: a written array may only use one offset so SIMD lane reordering
    // cannot break WAW/WAR/RAW dependences; read-only arrays allow multiple offsets (e.g. stencils).
    for (const auto& b : result.bindings) {
        if (b.isWrite && b.offsets.size() > 1u) {
            return result;
        }
    }

    // Reverse topological pass in bodyBlock to detect non-addressing uses and pure-addressing regs.
    std::unordered_set<mir::ValueId> neededByCompute;
    const std::size_t numInsts = bodyBlock->instructions.size();
    for (std::size_t rev = numInsts; rev > 0; --rev) {
        const std::size_t idx = rev - 1u;
        const auto& inst = bodyBlock->instructions[idx];
        const bool isCandidateAddr =
            candidateAddrInstIndices.find(idx) != candidateAddrInstIndices.end();
        const bool resultNeeded = (inst.result >= 0) &&
            (neededByCompute.find(inst.result) != neededByCompute.end());

        if (isCandidateAddr && !resultNeeded &&
            (inst.opcode == mir::Opcode::LoadLocal ||
             inst.opcode == mir::Opcode::LoadStructField ||
             inst.opcode == mir::Opcode::AddInt ||
             inst.opcode == mir::Opcode::SubtractInt ||
             inst.opcode == mir::Opcode::ConstantInt)) {
            result.skipBodyInst[idx] = true;
            continue;
        }

        if (inst.opcode == mir::Opcode::LoadElement) {
            // operands[0] and operands[1] are strictly consumed as array/index addressing.
            continue;
        }
        if (inst.opcode == mir::Opcode::StoreElement) {
            neededByCompute.insert(inst.operands[2]);
            continue;
        }
        for (const auto op : inst.operands) {
            if (op >= 0) neededByCompute.insert(op);
        }
    }

    // Validate all remaining instructions in bodyBlock (ignoring skipped address computations).
    for (std::size_t instIdx = 0; instIdx < numInsts; ++instIdx) {
        const auto& inst = bodyBlock->instructions[instIdx];
        if (result.skipBodyInst[instIdx]) continue;

        // Disallow non-addressing reads of local array load regs.
        for (std::size_t oi = 0; oi < inst.operands.size(); ++oi) {
            if (result.bodyLocalArrayLoadRegs.find(inst.operands[oi]) !=
                result.bodyLocalArrayLoadRegs.end()) {
                const bool validArrayUse = (oi == 0u) &&
                    (inst.opcode == mir::Opcode::LoadElement ||
                     inst.opcode == mir::Opcode::StoreElement);
                if (!validArrayUse) return result;
            }
        }

        switch (inst.opcode) {
        case mir::Opcode::LoadLocal: {
            if (inst.localIndex >= function.localTypes.size() || inst.result < 0) return result;
            const auto lPrim = function.localTypes[inst.localIndex];
            if (lPrim == semantic::PrimitiveType::Array ||
                lPrim == semantic::PrimitiveType::Object ||
                lPrim == semantic::PrimitiveType::String ||
                lPrim == semantic::PrimitiveType::Handle) {
                return result;
            }
            break;
        }
        case mir::Opcode::StoreLocal: {
            if (inst.localIndex == indexLocalIndex ||
                inst.localIndex >= function.localTypes.size()) {
                return result;
            }
            const auto lPrim = function.localTypes[inst.localIndex];
            if (lPrim == semantic::PrimitiveType::Array ||
                lPrim == semantic::PrimitiveType::Object ||
                lPrim == semantic::PrimitiveType::String ||
                lPrim == semantic::PrimitiveType::Handle) {
                return result;
            }
            break;
        }
        case mir::Opcode::LoadStructField: {
            if (inst.operands.size() != 1 || inst.result < 0 ||
                !isScalarNumericArrayElemType(inst.resultType)) {
                return result;
            }
            std::string stKey;
            std::string stExpr;
            if (!resolveInvariantStructExpr(inst.operands[0], stKey, stExpr)) return result;
            break;
        }
        case mir::Opcode::LoadElement:
        case mir::Opcode::StoreElement:
            break;
        case mir::Opcode::ConstantInt:
            if (inst.resultType == semantic::PrimitiveType::Enum) return result;
            break;
        case mir::Opcode::ConstantDouble:
        case mir::Opcode::ConstantBool:
        case mir::Opcode::ConvertIntToLong:
        case mir::Opcode::ConvertIntToDouble:
        case mir::Opcode::ConvertLongToDouble:
        case mir::Opcode::AddDouble:
        case mir::Opcode::SubtractDouble:
        case mir::Opcode::MultiplyDouble:
        case mir::Opcode::DivideDouble:
        case mir::Opcode::NegateDouble:
        case mir::Opcode::LogicalNot:
        case mir::Opcode::Equal:
        case mir::Opcode::NotEqual:
        case mir::Opcode::LessInt:
        case mir::Opcode::LessOrEqualInt:
        case mir::Opcode::GreaterInt:
        case mir::Opcode::GreaterOrEqualInt:
        case mir::Opcode::LessLong:
        case mir::Opcode::LessOrEqualLong:
        case mir::Opcode::GreaterLong:
        case mir::Opcode::GreaterOrEqualLong:
        case mir::Opcode::LessDouble:
        case mir::Opcode::LessOrEqualDouble:
        case mir::Opcode::GreaterDouble:
        case mir::Opcode::GreaterOrEqualDouble:
            break;
        case mir::Opcode::ConvertNumeric: {
            if (inst.operands.size() != 1 || inst.result < 0 ||
                inst.operands[0] < 0 ||
                static_cast<std::size_t>(inst.operands[0]) >= types.size()) {
                return result;
            }
            const auto srcPrim = types[static_cast<std::size_t>(inst.operands[0])].primitive;
            if (!supportsSimdConvertNumeric(srcPrim, inst.resultType, inst.checkedArithmetic)) {
                bool safeConstFloatNarrow = false;
                if (inst.checkedArithmetic &&
                    srcPrim == semantic::PrimitiveType::Double &&
                    inst.resultType == semantic::PrimitiveType::Float) {
                    const auto dIt = ctx.valueDefInst.find(inst.operands[0]);
                    if (dIt != ctx.valueDefInst.end() &&
                        dIt->second->opcode == mir::Opcode::ConstantDouble) {
                        const double dv = dIt->second->doubleImmediate;
                        const double maxF = static_cast<double>(std::numeric_limits<float>::max());
                        if (std::isfinite(dv) && dv >= -maxF && dv <= maxF) {
                            safeConstFloatNarrow = true;
                        }
                    }
                }
                if (!safeConstFloatNarrow) {
                    return result;
                }
            }
            break;
        }
        case mir::Opcode::AddLong:
        case mir::Opcode::SubtractLong:
        case mir::Opcode::MultiplyLong:
        case mir::Opcode::NegateLong:
            if (inst.checkedArithmetic) return result;
            break;
        case mir::Opcode::AddInt:
        case mir::Opcode::SubtractInt:
        case mir::Opcode::MultiplyInt:
        case mir::Opcode::NegateInt:
            if (inst.checkedArithmetic && rangeAnalysis != nullptr &&
                !typedCheckedArithmeticProvenSafe(*rangeAnalysis, inst)) {
                return result;
            }
            break;
        default:
            return result;
        }
    }

    result.valid = true;
    return result;
}

} // namespace

std::unordered_map<mir::BlockId, VectorizedEcsLoopPlan> analyzeVectorizedEcsLoops(
    const mir::Function& function,
    const std::unordered_map<std::size_t, semantic::SymbolId>& arrayLocalStructElemType) {
    std::unordered_map<mir::BlockId, VectorizedEcsLoopPlan> plans;
    std::unordered_map<mir::ValueId, std::int64_t> constantInts;
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.opcode == mir::Opcode::ConstantInt &&
                inst.result >= 0 &&
                inst.resultType != semantic::PrimitiveType::Enum) {
                constantInts[inst.result] = inst.integerImmediate;
            }
        }
    }

    for (const auto& headerBlock : function.blocks) {
        if (headerBlock.instructions.size() != 4 ||
            headerBlock.terminator.kind != mir::TerminatorKind::Branch ||
            !headerBlock.terminator.arguments.empty() ||
            !headerBlock.terminator.falseArguments.empty()) {
            continue;
        }
        const auto& idxLoad = headerBlock.instructions[0];
        if (idxLoad.opcode != mir::Opcode::LoadLocal ||
            idxLoad.result < 0 ||
            idxLoad.localIndex >= function.localTypes.size() ||
            function.localTypes[idxLoad.localIndex] != semantic::PrimitiveType::Int) {
            continue;
        }

        VectorizedEcsLoopPlan plan;
        plan.headerBlockId = headerBlock.id;
        plan.indexLocalIndex = idxLoad.localIndex;
        plan.exitBlockId = headerBlock.terminator.falseTarget;
        const mir::ValueId indexVal = idxLoad.result;

        const mir::BasicBlock* curBlock = &headerBlock;
        std::size_t instOffset = 1;
        bool chainValid = true;
        while (curBlock) {
            if (curBlock->instructions.size() != instOffset + 3 ||
                curBlock->terminator.kind != mir::TerminatorKind::Branch ||
                curBlock->terminator.falseTarget != plan.exitBlockId ||
                !curBlock->terminator.arguments.empty() ||
                !curBlock->terminator.falseArguments.empty()) {
                chainValid = false;
                break;
            }
            const auto& colLoad = curBlock->instructions[instOffset + 0];
            const auto& lenInst = curBlock->instructions[instOffset + 1];
            const auto& cmpInst = curBlock->instructions[instOffset + 2];
            if (colLoad.opcode != mir::Opcode::LoadLocal ||
                colLoad.result < 0 ||
                colLoad.localIndex >= function.localTypes.size() ||
                function.localTypes[colLoad.localIndex] != semantic::PrimitiveType::Array ||
                lenInst.opcode != mir::Opcode::ArrayLength ||
                lenInst.operands.size() != 1 ||
                lenInst.operands[0] != colLoad.result ||
                lenInst.result < 0 ||
                cmpInst.opcode != mir::Opcode::LessInt ||
                cmpInst.operands.size() != 2 ||
                cmpInst.operands[0] != indexVal ||
                cmpInst.operands[1] != lenInst.result ||
                cmpInst.result != curBlock->terminator.condition) {
                chainValid = false;
                break;
            }
            plan.columns.push_back(VectorizedEcsColumnBinding{
                colLoad.localIndex, 0, 0, false});
            if (plan.columns.size() > 16) {
                chainValid = false;
                break;
            }
            const auto* nextBlock = findBlock(function, curBlock->terminator.target);
            if (!nextBlock) {
                chainValid = false;
                break;
            }
            if (nextBlock->instructions.size() == 3 &&
                nextBlock->instructions[0].opcode == mir::Opcode::LoadLocal &&
                nextBlock->instructions[1].opcode == mir::Opcode::ArrayLength &&
                nextBlock->instructions[2].opcode == mir::Opcode::LessInt &&
                nextBlock->terminator.kind == mir::TerminatorKind::Branch &&
                nextBlock->terminator.falseTarget == plan.exitBlockId) {
                curBlock = nextBlock;
                instOffset = 0;
                continue;
            }
            plan.bodyBlockId = nextBlock->id;
            break;
        }
        if (!chainValid || plan.columns.empty()) continue;

        const auto* bodyBlock = findBlock(function, plan.bodyBlockId);
        if (!bodyBlock ||
            bodyBlock->terminator.kind != mir::TerminatorKind::Jump ||
            !bodyBlock->terminator.arguments.empty()) {
            continue;
        }
        plan.incrementBlockId = bodyBlock->terminator.target;
        plan.bodyPreambleCount = 4 * plan.columns.size();
        if (bodyBlock->instructions.size() < plan.bodyPreambleCount) continue;

        bool preambleValid = true;
        std::set<std::size_t> usedLocals{plan.indexLocalIndex};
        std::set<semantic::SymbolId> usedElemTypes;
        for (std::size_t c = 0; c < plan.columns.size(); ++c) {
            if (!usedLocals.insert(plan.columns[c].colLocalIndex).second) {
                preambleValid = false;
                break;
            }
            const std::size_t base = 4 * c;
            const auto& b0 = bodyBlock->instructions[base + 0];
            const auto& b1 = bodyBlock->instructions[base + 1];
            const auto& b2 = bodyBlock->instructions[base + 2];
            const auto& b3 = bodyBlock->instructions[base + 3];
            if (b0.opcode != mir::Opcode::LoadLocal ||
                b0.localIndex != plan.columns[c].colLocalIndex ||
                b0.result < 0 ||
                b1.opcode != mir::Opcode::LoadLocal ||
                b1.localIndex != plan.indexLocalIndex ||
                b1.result < 0 ||
                b2.opcode != mir::Opcode::LoadElement ||
                b2.operands.size() != 2 ||
                b2.operands[0] != b0.result ||
                b2.operands[1] != b1.result ||
                b2.result < 0 ||
                b2.elementType != semantic::PrimitiveType::Struct ||
                b2.elementTypeId == 0 ||
                classifyInlineMath(b2.elementType, b2.elementTypeId) != InlineMathKind::None ||
                b3.opcode != mir::Opcode::StoreLocal ||
                b3.operands.size() != 1 ||
                b3.operands[0] != b2.result ||
                b3.localIndex >= function.localTypes.size() ||
                function.localTypes[b3.localIndex] != semantic::PrimitiveType::Struct) {
                preambleValid = false;
                break;
            }
            const auto elemTypeIt = arrayLocalStructElemType.find(plan.columns[c].colLocalIndex);
            if (elemTypeIt == arrayLocalStructElemType.end() ||
                elemTypeIt->second != b2.elementTypeId ||
                !usedElemTypes.insert(b2.elementTypeId).second ||
                !usedLocals.insert(b3.localIndex).second) {
                preambleValid = false;
                break;
            }
            plan.columns[c].iterLocalIndex = b3.localIndex;
            plan.columns[c].elemTypeId = b2.elementTypeId;
        }
        if (!preambleValid) continue;

        bool bodyValid = true;
        for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
            const auto& inst = bodyBlock->instructions[i];
            if (!isPureVectorizableBodyInstruction(inst)) {
                bodyValid = false;
                break;
            }
            if (inst.opcode == mir::Opcode::StoreLocal) {
                if (inst.localIndex == plan.indexLocalIndex) {
                    bodyValid = false;
                    break;
                }
                for (const auto& col : plan.columns) {
                    if (inst.localIndex == col.colLocalIndex) {
                        bodyValid = false;
                        break;
                    }
                }
                if (!bodyValid) break;
            } else if (inst.opcode == mir::Opcode::LoadLocal &&
                       inst.localIndex == plan.indexLocalIndex) {
                plan.usesIndexInsideBody = true;
            }
        }
        if (!bodyValid) continue;

        const auto* incBlock = findBlock(function, plan.incrementBlockId);
        if (!incBlock ||
            incBlock->terminator.kind != mir::TerminatorKind::Jump ||
            incBlock->terminator.target != plan.headerBlockId ||
            !incBlock->terminator.arguments.empty()) {
            continue;
        }

        std::size_t incPos = 0;
        bool incValid = true;
        while (incPos + 4 <= incBlock->instructions.size() &&
               incBlock->instructions[incPos].opcode == mir::Opcode::LoadLocal &&
               incBlock->instructions[incPos].localIndex < function.localTypes.size() &&
               function.localTypes[incBlock->instructions[incPos].localIndex] ==
                   semantic::PrimitiveType::Array) {
            const auto& i0 = incBlock->instructions[incPos + 0];
            const auto& i1 = incBlock->instructions[incPos + 1];
            const auto& i2 = incBlock->instructions[incPos + 2];
            const auto& i3 = incBlock->instructions[incPos + 3];
            if (i0.result < 0 ||
                i1.opcode != mir::Opcode::LoadLocal ||
                i1.localIndex != plan.indexLocalIndex ||
                i1.result < 0 ||
                i2.opcode != mir::Opcode::LoadLocal ||
                i2.result < 0 ||
                i3.opcode != mir::Opcode::StoreElement ||
                i3.operands.size() != 3 ||
                i3.operands[0] != i0.result ||
                i3.operands[1] != i1.result ||
                i3.operands[2] != i2.result) {
                incValid = false;
                break;
            }
            bool matchedCol = false;
            for (auto& col : plan.columns) {
                if (col.colLocalIndex == i0.localIndex &&
                    col.iterLocalIndex == i2.localIndex) {
                    col.isWrite = true;
                    matchedCol = true;
                    break;
                }
            }
            if (!matchedCol) {
                incValid = false;
                break;
            }
            incPos += 4;
            if (incPos < incBlock->instructions.size() &&
                incBlock->instructions[incPos].opcode == mir::Opcode::Call) {
                const std::string_view sym(incBlock->instructions[incPos].symbolName);
                if (sym.rfind("$intrinsic::ecs_writeback::", 0) == 0) {
                    plan.hasSparseWritebackCall = true;
                    incPos += 1;
                } else {
                    incValid = false;
                    break;
                }
            }
        }
        if (!incValid) continue;

        if (incPos >= incBlock->instructions.size()) continue;
        const auto& idxLoadInc = incBlock->instructions[incPos++];
        if (idxLoadInc.opcode != mir::Opcode::LoadLocal ||
            idxLoadInc.localIndex != plan.indexLocalIndex ||
            idxLoadInc.result < 0) {
            continue;
        }
        if (incPos < incBlock->instructions.size() &&
            incBlock->instructions[incPos].opcode == mir::Opcode::ConstantInt) {
            ++incPos;
        }
        if (incPos + 2 != incBlock->instructions.size()) continue;
        const auto& addInc = incBlock->instructions[incPos];
        const auto& storeInc = incBlock->instructions[incPos + 1];
        if (addInc.opcode != mir::Opcode::AddInt ||
            addInc.operands.size() != 2 ||
            addInc.operands[0] != idxLoadInc.result ||
            addInc.result < 0 ||
            storeInc.opcode != mir::Opcode::StoreLocal ||
            storeInc.localIndex != plan.indexLocalIndex ||
            storeInc.operands.size() != 1 ||
            storeInc.operands[0] != addInc.result) {
            continue;
        }
        const auto oneIt = constantInts.find(addInc.operands[1]);
        if (oneIt == constantInts.end() || oneIt->second != 1) continue;

        plans.emplace(plan.headerBlockId, std::move(plan));
    }
    return plans;
}


namespace {

void emitVectorizedBodyInstruction(
    Emitter& source,
    const mir::Instruction& instruction,
    const std::vector<TypedSlotType>& types,
    const std::string& indent) {
    (void)types;
    const auto result = instruction.result >= 0
        ? typedValueExpression(instruction.result)
        : std::string{};
    const auto operand = [&](std::size_t index) {
        return typedValueExpression(instruction.operands[index]);
    };
    switch (instruction.opcode) {
    case mir::Opcode::ConstantInt:
        if (instruction.resultType == semantic::PrimitiveType::Enum) {
            source.line(indent + result + " = runtime::EnumValue{0x" +
                hexId(instruction.resultTypeId) + "ULL, " +
                int64Literal(instruction.integerImmediate) + "};");
        } else {
            source.line(indent + result + " = std::int64_t{" +
                int64Literal(instruction.integerImmediate) + "};");
        }
        break;
    case mir::Opcode::ConstantDouble: {
        std::ostringstream literal;
        literal << std::setprecision(17) << instruction.doubleImmediate;
        if (instruction.resultType == semantic::PrimitiveType::Float) {
            source.line(indent + result + " = static_cast<float>(" +
                literal.str() + ");");
        } else {
            source.line(indent + result + " = " + literal.str() + ";");
        }
        break;
    }
    case mir::Opcode::ConstantBool:
        source.line(indent + result + " = " +
            (instruction.boolImmediate ? "true" : "false") + ";");
        break;
    case mir::Opcode::LoadLocal:
        source.line(indent + result + " = " +
            typedLocalExpression(instruction.localIndex) + ";");
        break;
    case mir::Opcode::StoreLocal:
        source.line(indent +
            typedLocalExpression(instruction.localIndex) + " = " +
            operand(0) + ";");
        break;
    case mir::Opcode::ConvertIntToLong:
        source.line(indent + result + " = static_cast<std::int64_t>(" + operand(0) + ");");
        break;
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
        source.line(indent + result + " = static_cast<double>(" + operand(0) + ");");
        break;
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt: {
        const auto operation = instruction.opcode == mir::Opcode::AddInt
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractInt ? " - " : " * ");
        if (!instruction.checkedArithmetic) {
            source.line(indent + result + " = aot::typedWrapInt32(" +
                operand(0) + operation + operand(1) + ");");
        } else {
            source.line(indent + result + " = " + operand(0) + operation + operand(1) + ";");
        }
        break;
    }
    case mir::Opcode::NegateInt:
        if (!instruction.checkedArithmetic) {
            source.line(indent + result + " = aot::typedWrapInt32(-" + operand(0) + ");");
        } else {
            source.line(indent + result + " = -" + operand(0) + ";");
        }
        break;
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong: {
        const auto operation = instruction.opcode == mir::Opcode::AddLong
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractLong ? " - " : " * ");
        source.line(indent + result +
            " = static_cast<std::int64_t>(static_cast<std::uint64_t>(" +
            operand(0) + ")" + operation + "static_cast<std::uint64_t>(" +
            operand(1) + "));");
        break;
    }
    case mir::Opcode::NegateLong:
        source.line(indent + result +
            " = static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(" +
            operand(0) + "));");
        break;
    case mir::Opcode::NegateDouble:
        source.line(indent + result + " = -" + operand(0) + ";");
        break;
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble: {
        const char* operation = instruction.opcode == mir::Opcode::AddDouble
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractDouble
                ? " - "
                : (instruction.opcode == mir::Opcode::MultiplyDouble
                    ? " * "
                    : " / "));
        source.line(indent + result + " = " + operand(0) + operation + operand(1) + ";");
        break;
    }
    case mir::Opcode::LogicalNot:
        source.line(indent + result + " = !" + operand(0) + ";");
        break;
    case mir::Opcode::Equal:
        source.line(indent + result + " = (" + operand(0) + " == " + operand(1) + ");");
        break;
    case mir::Opcode::NotEqual:
        source.line(indent + result + " = !(" + operand(0) + " == " + operand(1) + ");");
        break;
    case mir::Opcode::LessInt:
    case mir::Opcode::LessOrEqualInt:
    case mir::Opcode::GreaterInt:
    case mir::Opcode::GreaterOrEqualInt:
    case mir::Opcode::LessLong:
    case mir::Opcode::LessOrEqualLong:
    case mir::Opcode::GreaterLong:
    case mir::Opcode::GreaterOrEqualLong:
    case mir::Opcode::LessDouble:
    case mir::Opcode::LessOrEqualDouble:
    case mir::Opcode::GreaterDouble:
    case mir::Opcode::GreaterOrEqualDouble: {
        const char* operation = "<";
        if (instruction.opcode == mir::Opcode::LessOrEqualInt ||
            instruction.opcode == mir::Opcode::LessOrEqualLong ||
            instruction.opcode == mir::Opcode::LessOrEqualDouble) {
            operation = "<=";
        } else if (instruction.opcode == mir::Opcode::GreaterInt ||
                   instruction.opcode == mir::Opcode::GreaterLong ||
                   instruction.opcode == mir::Opcode::GreaterDouble) {
            operation = ">";
        } else if (instruction.opcode == mir::Opcode::GreaterOrEqualInt ||
                   instruction.opcode == mir::Opcode::GreaterOrEqualLong ||
                   instruction.opcode == mir::Opcode::GreaterOrEqualDouble) {
            operation = ">=";
        }
        source.line(indent + result + " = " + operand(0) + " " + operation + " " + operand(1) + ";");
        break;
    }
    case mir::Opcode::NewStruct: {
        const auto typeId = instruction.resultTypeId != 0
            ? instruction.resultTypeId
            : instruction.typeId;
        const auto kind = classifyInlineMath(semantic::PrimitiveType::Struct, typeId);
        if (kind != InlineMathKind::None) {
            source.line(indent + result + " = " +
                typedDefaultValue({semantic::PrimitiveType::Struct, typeId}) + ";");
        } else {
            source.line(indent + result + " = " +
                typedDefaultValue({semantic::PrimitiveType::Struct, typeId}) + ";");
            source.line(indent + "(void)context.initUnboxedStruct(0x" +
                hexId(typeId) + "ULL, " + result + ");");
        }
        break;
    }
    case mir::Opcode::LoadStructField: {
        const auto kind = classifyInlineMath(
            semantic::PrimitiveType::Struct, instruction.typeId);
        if (kind != InlineMathKind::None) {
            const char* member = inlineMathFieldMember(kind, instruction.fieldIndex);
            if (kind == InlineMathKind::Fixed || kind == InlineMathKind::Fixed16 ||
                kind == InlineMathKind::Fixed64) {
                source.line(indent + result + " = static_cast<std::int64_t>(" +
                    operand(0) + "." + member + ");");
            } else if (kind == InlineMathKind::Vec2f || kind == InlineMathKind::Vec3f ||
                       kind == InlineMathKind::Quatf) {
                source.line(indent + result + " = " + operand(0) + "." + member + ";");
            } else {
                source.line(indent + result + " = runtime::FixedValue{" +
                    operand(0) + "." + member + "};");
            }
        } else {
            source.line(indent + result + " = " + operand(0) + ".f_" +
                std::to_string(instruction.fieldIndex) + ";");
        }
        break;
    }
    case mir::Opcode::StoreStructField: {
        const auto kind = classifyInlineMath(
            semantic::PrimitiveType::Struct, instruction.typeId);
        if (kind != InlineMathKind::None) {
            const char* member = inlineMathFieldMember(kind, instruction.fieldIndex);
            if (result != operand(0)) {
                source.line(indent + result + " = " + operand(0) + ";");
            }
            if (kind == InlineMathKind::Fixed) {
                source.line(indent + result + "." + member +
                    " = static_cast<std::int32_t>(" + operand(1) + ");");
            } else if (kind == InlineMathKind::Fixed16 || kind == InlineMathKind::Fixed64) {
                source.line(indent + result + "." + member +
                    " = static_cast<std::int64_t>(" + operand(1) + ");");
            } else if (kind == InlineMathKind::Vec2f || kind == InlineMathKind::Vec3f ||
                       kind == InlineMathKind::Quatf) {
                source.line(indent + result + "." + member +
                    " = static_cast<float>(" + operand(1) + ");");
            } else {
                source.line(indent + result + "." + member + " = " + operand(1) + ".raw;");
            }
        } else {
            if (result != operand(0)) {
                source.line(indent + result + " = " + operand(0) + ";");
            }
            source.line(indent + result + ".f_" +
                std::to_string(instruction.fieldIndex) + " = " + operand(1) + ";");
        }
        break;
    }
    default:
        break;
    }
}


} // namespace

void emitVectorizedEcsLoop(
    Emitter& source,
    const mir::Module& module,
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const VectorizedEcsLoopPlan& plan,
    bool emitLineDirectives) {
    const auto* bodyBlock = findBlock(function, plan.bodyBlockId);
    if (!bodyBlock) return;

    const auto idxLocalExpr = typedLocalExpression(plan.indexLocalIndex);
    std::string exactCond;
    std::string rawCond;
    for (std::size_t c = 0; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        if (!exactCond.empty()) exactCond += " && ";
        if (!rawCond.empty()) rawCond += " && ";
        exactCond += rawCol + ".exactNativeLayout";
        rawCond += "(" + rawCol + ".basePtr != nullptr)";
        if (plan.columns[c].isWrite) {
            exactCond += " && !" + rawCol + ".readOnly";
            rawCond += " && !" + rawCol + ".readOnly";
        }
    }
    exactCond += " && (" + idxLocalExpr + " >= 0)";
    rawCond += " && (" + idxLocalExpr + " >= 0)";
    if (plan.hasSparseWritebackCall) {
        exactCond += " && !context.hasSparseEcsState()";
        rawCond += " && !context.hasSparseEcsState()";
    }

    const auto firstRawCol = "typedLocalRawCol_" + std::to_string(plan.columns[0].colLocalIndex);

    source.line("            // Reconstructed Vectorized ECS Query Fast-Loop (__restrict + #pragma GCC ivdep)");
    source.line("            if constexpr (TrustedRelease) {");
    source.line("                if (RS_AOT_LIKELY(" + exactCond + ")) {");
    source.line("                    std::int64_t ecsFastCount = " + firstRawCol + ".count;");
    for (std::size_t c = 1; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        source.line("                    if (" + rawCol + ".count < ecsFastCount) ecsFastCount = " + rawCol + ".count;");
    }
    for (const auto& col : plan.columns) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        if (col.isWrite) {
            source.line("                    auto* __restrict " + ptrName +
                " = reinterpret_cast<" + structName + "*>(" + rawCol + ".basePtr);");
        } else {
            source.line("                    const auto* __restrict " + ptrName +
                " = reinterpret_cast<const " + structName + "*>(" + rawCol + ".basePtr);");
        }
    }
    source.line("#if defined(__GNUC__) && !defined(__clang__)");
    source.line("#pragma GCC ivdep");
    source.line("#elif defined(__clang__)");
    source.line("#pragma clang loop vectorize(enable) interleave(enable)");
    source.line("#elif defined(_MSC_VER) && !defined(__clang__)");
    source.line("#pragma loop(ivdep)");
    source.line("#endif");
    source.line("                    for (std::int64_t ecsIdx = " + idxLocalExpr + "; ecsIdx < ecsFastCount; ++ecsIdx) {");
    if (plan.usesIndexInsideBody) {
        source.line("                        " + idxLocalExpr + " = ecsIdx;");
    }
    for (const auto& col : plan.columns) {
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        source.line("                        " + typedLocalExpression(col.iterLocalIndex) +
            " = " + ptrName + "[ecsIdx];");
    }
    for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
        emitLineDirective(
            source,
            module,
            resolveSequencePoint(
                module,
                function,
                bodyBlock->id,
                static_cast<std::uint32_t>(i),
                false,
                bodyBlock->instructions[i].sourceSpan),
            emitLineDirectives,
            nullptr,
            function.symbolId,
            false);
        emitVectorizedBodyInstruction(source, bodyBlock->instructions[i], types, "                        ");
    }
    emitGeneratedLineReset(source, emitLineDirectives);
    for (const auto& col : plan.columns) {
        if (!col.isWrite) continue;
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        source.line("                        " + ptrName + "[ecsIdx] = " +
            typedLocalExpression(col.iterLocalIndex) + ";");
    }
    source.line("                    }");
    source.line("                    if (ecsFastCount > " + idxLocalExpr + ") " + idxLocalExpr + " = ecsFastCount;");
    source.line("                    goto typedBlock_" + std::to_string(plan.exitBlockId) + ";");
    source.line("                } else if (RS_AOT_LIKELY(" + rawCond + ")) {");
    source.line("                    std::int64_t ecsFastCount = " + firstRawCol + ".count;");
    for (std::size_t c = 1; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        source.line("                    if (" + rawCol + ".count < ecsFastCount) ecsFastCount = " + rawCol + ".count;");
    }
    source.line("                    for (std::int64_t ecsIdx = " + idxLocalExpr + "; ecsIdx < ecsFastCount; ++ecsIdx) {");
    if (plan.usesIndexInsideBody) {
        source.line("                        " + idxLocalExpr + " = ecsIdx;");
    }
    for (const auto& col : plan.columns) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        source.line("                        (void)" + structName + "::loadFromRawColumn(" +
            rawCol + ", ecsIdx, " + typedLocalExpression(col.iterLocalIndex) + ");");
    }
    for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
        emitLineDirective(
            source,
            module,
            resolveSequencePoint(
                module,
                function,
                bodyBlock->id,
                static_cast<std::uint32_t>(i),
                false,
                bodyBlock->instructions[i].sourceSpan),
            emitLineDirectives,
            nullptr,
            function.symbolId,
            false);
        emitVectorizedBodyInstruction(source, bodyBlock->instructions[i], types, "                        ");
    }
    emitGeneratedLineReset(source, emitLineDirectives);
    for (const auto& col : plan.columns) {
        if (!col.isWrite) continue;
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        source.line("                        (void)" + structName + "::storeToRawColumn(" +
            rawCol + ", ecsIdx, " + typedLocalExpression(col.iterLocalIndex) + ");");
    }
    source.line("                    }");
    source.line("                    if (ecsFastCount > " + idxLocalExpr + ") " + idxLocalExpr + " = ecsFastCount;");
    source.line("                    goto typedBlock_" + std::to_string(plan.exitBlockId) + ";");
    source.line("                }");
    source.line("            }");
}


std::string packedArrayCacheName(
    std::size_t localIndex,
    semantic::PrimitiveType elemPrim) {
    return (elemPrim == semantic::PrimitiveType::Int
        ? "typedLocalPackedI32_"
        : "typedLocalPackedF64_") + std::to_string(localIndex);
}

std::unordered_map<mir::BlockId, VectorizedPackedArrayLoopPlan> analyzeVectorizedPackedArrayLoops(
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const TypedRangeAnalysis& rangeAnalysis,
    const std::unordered_map<std::size_t, semantic::PrimitiveType>& singleStorePackedArrayLocals) {
    std::unordered_map<mir::BlockId, VectorizedPackedArrayLoopPlan> plans;
    if (!function.exceptionHandlers.empty()) {
        return plans;
    }

    const PackedLoopContextMaps packedCtx = buildPackedLoopContextMaps(function);

    // Count block predecessors
    std::unordered_map<mir::BlockId, std::size_t> predCounts;
    for (const auto& blk : function.blocks) {
        if (blk.terminator.kind == mir::TerminatorKind::Jump) {
            ++predCounts[blk.terminator.target];
        } else if (blk.terminator.kind == mir::TerminatorKind::Branch) {
            ++predCounts[blk.terminator.target];
            ++predCounts[blk.terminator.falseTarget];
        }
    }

    for (const auto& headerBlock : function.blocks) {
        if (headerBlock.terminator.kind != mir::TerminatorKind::Branch ||
            !headerBlock.terminator.arguments.empty() ||
            !headerBlock.terminator.falseArguments.empty() ||
            headerBlock.instructions.size() < 2 ||
            headerBlock.instructions.size() > 4) {
            continue;
        }
        const auto& idxLoad = headerBlock.instructions[0];
        if (idxLoad.opcode != mir::Opcode::LoadLocal ||
            idxLoad.result < 0 ||
            idxLoad.localIndex >= function.localTypes.size() ||
            function.localTypes[idxLoad.localIndex] != semantic::PrimitiveType::Int) {
            continue;
        }

        const mir::BlockId bodyId = headerBlock.terminator.target;
        const mir::BlockId exitId = headerBlock.terminator.falseTarget;
        if (bodyId == headerBlock.id || exitId == headerBlock.id || bodyId == exitId) {
            continue;
        }
        const auto* bodyBlock = findBlock(function, bodyId);
        const auto* exitBlock = findBlock(function, exitId);
        if (!bodyBlock || !exitBlock ||
            !bodyBlock->parameters.empty() ||
            !exitBlock->parameters.empty() ||
            predCounts[bodyId] != 1u ||
            bodyBlock->terminator.kind != mir::TerminatorKind::Jump ||
            !bodyBlock->terminator.arguments.empty()) {
            continue;
        }

        const mir::BlockId incId = bodyBlock->terminator.target;
        const auto* incBlock = findBlock(function, incId);
        if (!incBlock ||
            incId == headerBlock.id ||
            incId == bodyId ||
            incId == exitId ||
            !incBlock->parameters.empty() ||
            predCounts[incId] != 1u ||
            incBlock->terminator.kind != mir::TerminatorKind::Jump ||
            incBlock->terminator.target != headerBlock.id ||
            !incBlock->terminator.arguments.empty()) {
            continue;
        }

        const auto isInsideLoop = [&](mir::BlockId bid) noexcept {
            return bid == headerBlock.id || bid == bodyId || bid == incId;
        };

        // Check increment block: [load.local idx, (const.int 1)?, add.i32 idx 1, store.local idx]
        // Also tolerate [const.int 1, load.local idx, add.i32 ..., store.local idx].
        if (incBlock->instructions.size() != 3 && incBlock->instructions.size() != 4) {
            continue;
        }
        mir::ValueId incIdxReg = -1;
        std::size_t incLoadPos = incBlock->instructions.size();
        for (std::size_t k = 0; k + 2u <= incBlock->instructions.size(); ++k) {
            const auto& inst = incBlock->instructions[k];
            if (inst.opcode == mir::Opcode::LoadLocal &&
                inst.localIndex == idxLoad.localIndex &&
                inst.result >= 0) {
                incIdxReg = inst.result;
                incLoadPos = k;
            } else if (inst.opcode == mir::Opcode::ConstantInt &&
                       inst.result >= 0 &&
                       inst.integerImmediate == 1 &&
                       incBlock->instructions.size() == 4) {
                continue;
            } else if (k + 2u == incBlock->instructions.size()) {
                break;
            } else {
                incIdxReg = -1;
                break;
            }
        }
        if (incIdxReg < 0 || incLoadPos >= incBlock->instructions.size() - 2u) {
            continue;
        }
        const auto& incAdd = incBlock->instructions[incBlock->instructions.size() - 2u];
        const auto& incStore = incBlock->instructions[incBlock->instructions.size() - 1u];
        if (incAdd.opcode != mir::Opcode::AddInt ||
            incAdd.operands.size() != 2 ||
            incAdd.result < 0 ||
            incStore.opcode != mir::Opcode::StoreLocal ||
            incStore.localIndex != idxLoad.localIndex ||
            incStore.operands.size() != 1 ||
            incStore.operands[0] != incAdd.result) {
            continue;
        }
        mir::ValueId incOneReg = -1;
        if (incAdd.operands[0] == incIdxReg) {
            incOneReg = incAdd.operands[1];
        } else if (incAdd.operands[1] == incIdxReg) {
            incOneReg = incAdd.operands[0];
        } else {
            continue;
        }
        const auto oneIt = packedCtx.constantInts.find(incOneReg);
        if (oneIt == packedCtx.constantInts.end() || oneIt->second != 1) {
            continue;
        }

        // Collect locals written inside bodyBlock or incBlock
        std::unordered_set<std::size_t> loopWrittenLocals{idxLoad.localIndex};
        for (const auto& inst : bodyBlock->instructions) {
            if (inst.opcode == mir::Opcode::StoreLocal) {
                loopWrittenLocals.insert(inst.localIndex);
            }
        }
        for (const auto& inst : incBlock->instructions) {
            if (inst.opcode == mir::Opcode::StoreLocal) {
                loopWrittenLocals.insert(inst.localIndex);
            }
        }

        // Match header bound shape
        VectorizedPackedArrayLoopPlan plan;
        plan.headerBlockId = headerBlock.id;
        plan.bodyBlockId = bodyId;
        plan.incrementBlockId = incId;
        plan.exitBlockId = exitId;
        plan.indexLocalIndex = idxLoad.localIndex;

        std::map<std::size_t, VectorizedPackedArrayBinding> headerSingleStoreCaches;
        const auto resolveHeaderStructExpr = [&](mir::ValueId vid, std::string& outExpr) -> bool {
            if (vid < 0 || static_cast<std::size_t>(vid) >= types.size()) return false;
            const auto& slot = types[static_cast<std::size_t>(vid)];
            if (slot.primitive != semantic::PrimitiveType::Struct ||
                classifyInlineMath(semantic::PrimitiveType::Struct, slot.typeId) != InlineMathKind::None) {
                return false;
            }
            const auto dIt = packedCtx.valueDefBlock.find(vid);
            const auto iIt = packedCtx.valueDefInst.find(vid);
            if (dIt == packedCtx.valueDefBlock.end() || isInsideLoop(dIt->second) ||
                !packedCtx.strictlyDominates(dIt->second, headerBlock.id)) {
                return false;
            }
            const auto pIt = packedCtx.paramIndexByValue.find(vid);
            if (pIt != packedCtx.paramIndexByValue.end()) {
                outExpr = "typedArg_" + std::to_string(pIt->second);
                return true;
            }
            if (iIt != packedCtx.valueDefInst.end() &&
                iIt->second->opcode == mir::Opcode::LoadLocal &&
                iIt->second->localIndex < function.localTypes.size() &&
                loopWrittenLocals.find(iIt->second->localIndex) == loopWrittenLocals.end()) {
                outExpr = typedLocalExpression(iIt->second->localIndex);
                return true;
            }
            outExpr = typedValueExpression(vid);
            return true;
        };

        const auto resolveHeaderArrayBound = [&](mir::ValueId arrVid, std::string& outBoundExpr) -> bool {
            if (arrVid < 0 || static_cast<std::size_t>(arrVid) >= types.size() ||
                types[static_cast<std::size_t>(arrVid)].primitive != semantic::PrimitiveType::Array) {
                return false;
            }
            const auto dIt = packedCtx.valueDefBlock.find(arrVid);
            const auto iIt = packedCtx.valueDefInst.find(arrVid);
            if (dIt != packedCtx.valueDefBlock.end() && dIt->second == headerBlock.id) {
                if (iIt == packedCtx.valueDefInst.end()) return false;
                const auto& srcInst = *iIt->second;
                if (srcInst.opcode != mir::Opcode::LoadLocal ||
                    srcInst.localIndex >= function.localTypes.size() ||
                    loopWrittenLocals.find(srcInst.localIndex) != loopWrittenLocals.end()) {
                    return false;
                }
                const auto pIt = singleStorePackedArrayLocals.find(srcInst.localIndex);
                if (pIt != singleStorePackedArrayLocals.end()) {
                    auto& cacheEntry = headerSingleStoreCaches[srcInst.localIndex];
                    cacheEntry.arrayLocalIndex = srcInst.localIndex;
                    cacheEntry.elemPrim = pIt->second;
                    cacheEntry.isRead = true;
                    outBoundExpr = "__VEC_CACHE_COUNT__:" +
                        packedArrayCacheName(srcInst.localIndex, pIt->second);
                } else {
                    outBoundExpr = "__VEC_ARRAY_LEN__:" + typedLocalExpression(srcInst.localIndex);
                }
                return true;
            }
            if (dIt != packedCtx.valueDefBlock.end() &&
                (isInsideLoop(dIt->second) || !packedCtx.strictlyDominates(dIt->second, headerBlock.id))) {
                return false;
            }
            if (iIt != packedCtx.valueDefInst.end() &&
                iIt->second->opcode == mir::Opcode::LoadLocal &&
                iIt->second->localIndex < function.localTypes.size() &&
                loopWrittenLocals.find(iIt->second->localIndex) == loopWrittenLocals.end()) {
                const auto locIdx = iIt->second->localIndex;
                const auto pIt = singleStorePackedArrayLocals.find(locIdx);
                if (pIt != singleStorePackedArrayLocals.end()) {
                    auto& cacheEntry = headerSingleStoreCaches[locIdx];
                    cacheEntry.arrayLocalIndex = locIdx;
                    cacheEntry.elemPrim = pIt->second;
                    cacheEntry.isRead = true;
                    outBoundExpr = "__VEC_CACHE_COUNT__:" +
                        packedArrayCacheName(locIdx, pIt->second);
                } else {
                    outBoundExpr = "__VEC_ARRAY_LEN__:" + typedLocalExpression(locIdx);
                }
                return true;
            }
            const auto parIt = packedCtx.paramIndexByValue.find(arrVid);
            if (parIt != packedCtx.paramIndexByValue.end()) {
                outBoundExpr = "__VEC_ARRAY_LEN__:typedArg_" + std::to_string(parIt->second);
                return true;
            }
            outBoundExpr = "__VEC_ARRAY_LEN__:" + typedValueExpression(arrVid);
            return true;
        };

        const auto resolveHeaderIntExpr = [&](mir::ValueId vid, std::string& outExpr) -> bool {
            if (vid < 0 || static_cast<std::size_t>(vid) >= types.size() ||
                types[static_cast<std::size_t>(vid)].primitive != semantic::PrimitiveType::Int) {
                return false;
            }
            const auto cIt = packedCtx.constantInts.find(vid);
            if (cIt != packedCtx.constantInts.end()) {
                outExpr = "std::int64_t{" + int64Literal(cIt->second) + "}";
                return true;
            }
            const auto dIt = packedCtx.valueDefBlock.find(vid);
            if (dIt != packedCtx.valueDefBlock.end() &&
                (isInsideLoop(dIt->second) || !packedCtx.strictlyDominates(dIt->second, headerBlock.id))) {
                return false;
            }
            const auto pIt = packedCtx.paramIndexByValue.find(vid);
            if (pIt != packedCtx.paramIndexByValue.end()) {
                outExpr = "typedArg_" + std::to_string(pIt->second);
                return true;
            }
            const auto iIt = packedCtx.valueDefInst.find(vid);
            if (iIt != packedCtx.valueDefInst.end() &&
                iIt->second->opcode == mir::Opcode::LoadLocal &&
                iIt->second->localIndex < function.localTypes.size() &&
                function.localTypes[iIt->second->localIndex] == semantic::PrimitiveType::Int &&
                loopWrittenLocals.find(iIt->second->localIndex) == loopWrittenLocals.end()) {
                outExpr = typedLocalExpression(iIt->second->localIndex);
                return true;
            }
            outExpr = typedValueExpression(vid);
            return true;
        };

        const auto matchesIdxBoundCmp = [&](const mir::Instruction& cmp, mir::ValueId& outBoundVal) -> bool {
            if (cmp.operands.size() != 2 || cmp.result != headerBlock.terminator.condition) {
                return false;
            }
            if (cmp.opcode == mir::Opcode::LessInt && cmp.operands[0] == idxLoad.result) {
                outBoundVal = cmp.operands[1];
                return true;
            }
            if (cmp.opcode == mir::Opcode::GreaterInt && cmp.operands[1] == idxLoad.result) {
                outBoundVal = cmp.operands[0];
                return true;
            }
            return false;
        };

        if (headerBlock.instructions.size() == 2) {
            const auto& cmp = headerBlock.instructions[1];
            mir::ValueId bndVal = -1;
            if (!matchesIdxBoundCmp(cmp, bndVal) ||
                !resolveHeaderIntExpr(bndVal, plan.boundExpr)) {
                continue;
            }
        } else if (headerBlock.instructions.size() == 3) {
            const auto& bndInst = headerBlock.instructions[1];
            const auto& cmp = headerBlock.instructions[2];
            mir::ValueId cmpBound = -1;
            if (bndInst.result < 0 ||
                !matchesIdxBoundCmp(cmp, cmpBound) ||
                cmpBound != bndInst.result) {
                continue;
            }
            if (bndInst.opcode == mir::Opcode::LoadLocal &&
                bndInst.localIndex < function.localTypes.size() &&
                function.localTypes[bndInst.localIndex] == semantic::PrimitiveType::Int &&
                loopWrittenLocals.find(bndInst.localIndex) == loopWrittenLocals.end()) {
                plan.boundExpr = typedLocalExpression(bndInst.localIndex);
            } else if (bndInst.opcode == mir::Opcode::ConstantInt &&
                       bndInst.resultType != semantic::PrimitiveType::Enum) {
                plan.boundExpr = "std::int64_t{" + int64Literal(bndInst.integerImmediate) + "}";
            } else if (bndInst.opcode == mir::Opcode::LoadStructField &&
                       bndInst.operands.size() == 1 &&
                       bndInst.resultType == semantic::PrimitiveType::Int) {
                std::string stExpr;
                if (!resolveHeaderStructExpr(bndInst.operands[0], stExpr)) {
                    continue;
                }
                plan.boundExpr = stExpr + ".f_" + std::to_string(bndInst.fieldIndex);
            } else if (bndInst.opcode == mir::Opcode::ArrayLength &&
                       bndInst.operands.size() == 1) {
                if (!resolveHeaderArrayBound(bndInst.operands[0], plan.boundExpr)) {
                    continue;
                }
            } else if ((bndInst.opcode == mir::Opcode::AddInt ||
                        bndInst.opcode == mir::Opcode::SubtractInt) &&
                       bndInst.operands.size() == 2) {
                std::string lhsExpr;
                std::string rhsExpr;
                if (!resolveHeaderIntExpr(bndInst.operands[0], lhsExpr) ||
                    !resolveHeaderIntExpr(bndInst.operands[1], rhsExpr)) {
                    continue;
                }
                const char* opStr = (bndInst.opcode == mir::Opcode::AddInt) ? " + " : " - ";
                plan.boundExpr = "(static_cast<std::int64_t>(" + lhsExpr + ")" + opStr +
                    "static_cast<std::int64_t>(" + rhsExpr + "))";
            } else {
                continue;
            }
        } else if (headerBlock.instructions.size() == 4) {
            const auto& srcInst = headerBlock.instructions[1];
            const auto& lenInst = headerBlock.instructions[2];
            const auto& cmp = headerBlock.instructions[3];
            mir::ValueId cmpBound = -1;
            if (srcInst.opcode != mir::Opcode::LoadLocal ||
                srcInst.result < 0 ||
                srcInst.localIndex >= function.localTypes.size() ||
                loopWrittenLocals.find(srcInst.localIndex) != loopWrittenLocals.end() ||
                lenInst.operands.size() != 1 ||
                lenInst.operands[0] != srcInst.result ||
                lenInst.result < 0 ||
                !matchesIdxBoundCmp(cmp, cmpBound) ||
                cmpBound != lenInst.result) {
                continue;
            }
            if (lenInst.opcode == mir::Opcode::ArrayLength) {
                if (!resolveHeaderArrayBound(srcInst.result, plan.boundExpr)) {
                    continue;
                }
            } else if (lenInst.opcode == mir::Opcode::LoadStructField &&
                       lenInst.resultType == semantic::PrimitiveType::Int) {
                const auto srcPrim = function.localTypes[srcInst.localIndex];
                if (srcPrim != semantic::PrimitiveType::Struct ||
                    classifyInlineMath(srcPrim, lenInst.typeId) != InlineMathKind::None) {
                    continue;
                }
                plan.boundExpr = typedLocalExpression(srcInst.localIndex) +
                    ".f_" + std::to_string(lenInst.fieldIndex);
            } else {
                continue;
            }
        } else {
            continue;
        }

        // Verify no ValueId defined inside the loop is read outside the loop
        bool ssaEscapes = false;
        for (const auto& blk : function.blocks) {
            if (isInsideLoop(blk.id)) continue;
            auto checkExternalUse = [&](mir::ValueId vid) {
                if (vid < 0) return;
                const auto dIt = packedCtx.valueDefBlock.find(vid);
                if (dIt != packedCtx.valueDefBlock.end() && isInsideLoop(dIt->second)) {
                    ssaEscapes = true;
                }
            };
            for (const auto& inst : blk.instructions) {
                for (const auto op : inst.operands) checkExternalUse(op);
            }
            checkExternalUse(blk.terminator.value);
            checkExternalUse(blk.terminator.condition);
            for (const auto a : blk.terminator.arguments) checkExternalUse(a);
            for (const auto a : blk.terminator.falseArguments) checkExternalUse(a);
            if (ssaEscapes) break;
        }
        if (ssaEscapes) continue;

        const PackedLoopBodyAnalysis details = analyzePackedLoopBodyDetails(
            function,
            types,
            &rangeAnalysis,
            singleStorePackedArrayLocals,
            packedCtx,
            loopWrittenLocals,
            headerBlock.id,
            bodyId,
            incId,
            idxLoad.localIndex);
        if (!details.valid) {
            continue;
        }

        // Store singleStorePackedArrayLocals used by the loop (plus header cache bound if any)
        // in plan.arrays so emitVectorizedPackedArrayLoop knows which TypedPacked caches exist.
        std::map<std::size_t, VectorizedPackedArrayBinding> knownCaches = headerSingleStoreCaches;
        for (const auto& b : details.bindings) {
            if (b.precomputedCacheName.empty()) continue;
            for (const auto& [arrLoc, elemPrim] : singleStorePackedArrayLocals) {
                if (elemPrim == b.elemPrim &&
                    packedArrayCacheName(arrLoc, elemPrim) == b.precomputedCacheName) {
                    auto& entry = knownCaches[arrLoc];
                    entry.arrayLocalIndex = arrLoc;
                    entry.elemPrim = elemPrim;
                    if (b.isRead) entry.isRead = true;
                    if (b.isWrite) entry.isWrite = true;
                }
            }
        }
        for (const auto& [arrLoc, binding] : knownCaches) {
            (void)arrLoc;
            plan.arrays.push_back(binding);
        }
        plan.bodyArrayLoadRegs = details.bodyLocalArrayLoadRegs;
        plan.bodyIndexLoadRegs = details.bodyIndexLoadRegs;
        plan.headerInstsWithTerminator =
            static_cast<std::uint64_t>(headerBlock.instructions.size()) + 1ULL;
        plan.iterInstsWithTerminators =
            plan.headerInstsWithTerminator +
            (static_cast<std::uint64_t>(bodyBlock->instructions.size()) + 1ULL) +
            (static_cast<std::uint64_t>(incBlock->instructions.size()) + 1ULL);
        plan.iterBranches = 3ULL;

        plans.emplace(plan.headerBlockId, std::move(plan));
    }

    return plans;
}

void emitVectorizedPackedArrayLoop(
    Emitter& source,
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const VectorizedPackedArrayLoopPlan& plan) {
    const auto* bodyBlock = findBlock(function, plan.bodyBlockId);
    const auto* incBlock = findBlock(function, plan.incrementBlockId);
    if (!bodyBlock || !incBlock) return;

    std::unordered_map<std::size_t, semantic::PrimitiveType> knownSingleStoreLocals;
    for (const auto& arr : plan.arrays) {
        knownSingleStoreLocals.emplace(arr.arrayLocalIndex, arr.elemPrim);
    }

    std::unordered_set<std::size_t> loopWrittenLocals{plan.indexLocalIndex};
    for (const auto& inst : bodyBlock->instructions) {
        if (inst.opcode == mir::Opcode::StoreLocal) {
            loopWrittenLocals.insert(inst.localIndex);
        }
    }
    for (const auto& inst : incBlock->instructions) {
        if (inst.opcode == mir::Opcode::StoreLocal) {
            loopWrittenLocals.insert(inst.localIndex);
        }
    }

    const PackedLoopContextMaps packedCtx = buildPackedLoopContextMaps(function);
    const PackedLoopBodyAnalysis details = analyzePackedLoopBodyDetails(
        function,
        types,
        nullptr,
        knownSingleStoreLocals,
        packedCtx,
        loopWrittenLocals,
        plan.headerBlockId,
        plan.bodyBlockId,
        plan.incrementBlockId,
        plan.indexLocalIndex);
    if (!details.valid || details.bindings.empty()) {
        return;
    }

    const std::string idxLocalExpr = typedLocalExpression(plan.indexLocalIndex);

    source.line("            // P2-C: Reconstructed PackedArray/Span __restrict SIMD Fast-Loop");
    source.line("            if constexpr (TrustedRelease || FastAccounting) {");
    source.line("                const std::int64_t vecStart = " + idxLocalExpr + ";");

    constexpr std::string_view kCacheCountPrefix = "__VEC_CACHE_COUNT__:";
    constexpr std::string_view kArrayLenPrefix = "__VEC_ARRAY_LEN__:";
    std::string extraBoundValidExpr;
    if (plan.boundExpr.rfind(kCacheCountPrefix, 0) == 0) {
        const std::string cName = plan.boundExpr.substr(kCacheCountPrefix.size());
        source.line("                const std::int64_t vecEnd = " + cName + ".count;");
        extraBoundValidExpr = "(" + cName + ".count >= 0)";
    } else if (plan.boundExpr.rfind(kArrayLenPrefix, 0) == 0) {
        const std::string arrExpr = plan.boundExpr.substr(kArrayLenPrefix.size());
        source.line("                std::int64_t vecEnd = -1;");
        source.line("                bool vecBoundLenOk = false;");
        source.line("                if (context.heap() != nullptr && (" + arrExpr +
            ").rawTag() == runtime::ValueTag::ObjectRef) {");
        source.line("                    if (const auto vecHdrLen = context.heap()->arrayLength((" +
            arrExpr + ").asObjectRefUnchecked())) {");
        source.line("                        vecEnd = static_cast<std::int64_t>(*vecHdrLen);");
        source.line("                        vecBoundLenOk = true;");
        source.line("                    }");
        source.line("                }");
        extraBoundValidExpr = "vecBoundLenOk";
    } else {
        source.line("                const std::int64_t vecEnd = " + plan.boundExpr + ";");
    }

    struct EmittedBindingMeta {
        std::string validExpr;
        std::string lenExpr;
        std::string rawDataExpr;
        std::string bytePtrExpr;
    };
    std::vector<EmittedBindingMeta> metas(details.bindings.size());
    std::string validCachesCond = extraBoundValidExpr;

    for (std::size_t bIdx = 0; bIdx < details.bindings.size(); ++bIdx) {
        const auto& b = details.bindings[bIdx];
        auto& meta = metas[bIdx];
        if (!b.precomputedCacheName.empty()) {
            meta.validExpr = "(" + b.precomputedCacheName + ".count >= 0)";
            meta.lenExpr = b.precomputedCacheName + ".count";
            meta.rawDataExpr = b.precomputedCacheName + ".data";
            meta.bytePtrExpr = "static_cast<const void*>(" + b.precomputedCacheName + ".data)";
        } else {
            const std::string vName = "vecDynView_" + std::to_string(bIdx);
            const std::string okName = "vecDynOk_" + std::to_string(bIdx);
            source.line("                runtime::PackedArrayFastView " + vName + "{};");
            source.line("                const bool " + okName + " = (context.heap() != nullptr) && "
                "((" + b.arrayValueCppExpr + ").rawTag() == runtime::ValueTag::ObjectRef) && "
                "context.heap()->tryBindPackedArrayFastView((" + b.arrayValueCppExpr +
                ").asObjectRefUnchecked(), " + vName + ") && "
                "(" + vName + ".encoding == " + packedElemEncodingExpr(b.elemPrim) + ") && "
                "((" + vName + ".length == 0u) || (" + vName + ".rawData != nullptr));");
            meta.validExpr = okName;
            meta.lenExpr = "static_cast<std::int64_t>(" + vName + ".length)";
            meta.rawDataExpr = "static_cast<" + packedElemCppType(b.elemPrim) + "*>(" + vName + ".rawData)";
            meta.bytePtrExpr = "static_cast<const void*>(" + vName + ".rawData)";
        }
        if (!validCachesCond.empty()) validCachesCond += " && ";
        validCachesCond += meta.validExpr;
    }

    std::string inBoundsCond;
    for (std::size_t bIdx = 0; bIdx < details.bindings.size(); ++bIdx) {
        const auto& b = details.bindings[bIdx];
        for (std::size_t oIdx = 0; oIdx < b.offsets.size(); ++oIdx) {
            const std::string offVar =
                "vecOff_" + std::to_string(bIdx) + "_" + std::to_string(oIdx);
            source.line("                const std::int64_t " + offVar + " = " +
                b.offsets[oIdx].cppOffsetExpr + ";");
            if (!inBoundsCond.empty()) inBoundsCond += " && ";
            inBoundsCond += "(vecStart + " + offVar + " >= 0) && (vecEnd + " + offVar +
                " <= " + metas[bIdx].lenExpr + ")";
        }
    }

    for (std::size_t sIdx = 0; sIdx < details.checkedPartialSteps.size(); ++sIdx) {
        const std::string stepVar = "vecChkStep_" + std::to_string(sIdx);
        source.line("                const std::int64_t " + stepVar + " = " +
            details.checkedPartialSteps[sIdx].cppOffsetExpr + ";");
        if (!inBoundsCond.empty()) inBoundsCond += " && ";
        inBoundsCond += "(vecStart + " + stepVar + " >= static_cast<std::int64_t>(INT32_MIN)) && "
            "((vecEnd - std::int64_t{1}) + " + stepVar + " <= static_cast<std::int64_t>(INT32_MAX))";
    }

    bool needsAliasGuard = false;
    for (std::size_t i = 0; i < details.bindings.size(); ++i) {
        for (std::size_t j = i + 1u; j < details.bindings.size(); ++j) {
            if (details.bindings[i].isWrite || details.bindings[j].isWrite) {
                needsAliasGuard = true;
                break;
            }
        }
        if (needsAliasGuard) break;
    }

    std::string noAliasCond;
    if (needsAliasGuard) {
        for (std::size_t bIdx = 0; bIdx < details.bindings.size(); ++bIdx) {
            const auto& b = details.bindings[bIdx];
            const std::string minVar = "vecMinOff_" + std::to_string(bIdx);
            const std::string maxVar = "vecMaxOff_" + std::to_string(bIdx);
            const std::string off0 = "vecOff_" + std::to_string(bIdx) + "_0";
            source.line("                std::int64_t " + minVar + " = " + off0 + ";");
            source.line("                std::int64_t " + maxVar + " = " + off0 + ";");
            for (std::size_t oIdx = 1u; oIdx < b.offsets.size(); ++oIdx) {
                const std::string offN =
                    "vecOff_" + std::to_string(bIdx) + "_" + std::to_string(oIdx);
                source.line("                if (" + offN + " < " + minVar + ") " + minVar + " = " + offN + ";");
                source.line("                if (" + offN + " > " + maxVar + ") " + maxVar + " = " + offN + ";");
            }
            source.line("                (void)" + minVar + ";");
            source.line("                (void)" + maxVar + ";");
        }
        for (std::size_t i = 0; i < details.bindings.size(); ++i) {
            for (std::size_t j = i + 1u; j < details.bindings.size(); ++j) {
                if (!details.bindings[i].isWrite && !details.bindings[j].isWrite) continue;
                const std::string sI = std::to_string(i);
                const std::string sJ = std::to_string(j);
                if (!noAliasCond.empty()) noAliasCond += " && ";
                noAliasCond += "((" + metas[i].bytePtrExpr + " != " + metas[j].bytePtrExpr +
                    ") || ((vecEnd + vecMaxOff_" + sI + ") <= (vecStart + vecMinOff_" + sJ +
                    ")) || ((vecEnd + vecMaxOff_" + sJ + ") <= (vecStart + vecMinOff_" + sI + ")))";
            }
        }
    }

    std::string activeTripGuard = inBoundsCond;
    if (!noAliasCond.empty()) {
        if (!activeTripGuard.empty()) activeTripGuard += " && ";
        activeTripGuard += noAliasCond;
    }

    // Track body-defined non-skipped ValueIds for narrow SIMD register typing
    std::unordered_set<mir::ValueId> bodyDefinedValues;
    for (std::size_t instIdx = 0; instIdx < bodyBlock->instructions.size(); ++instIdx) {
        if (details.skipBodyInst[instIdx]) continue;
        const auto& inst = bodyBlock->instructions[instIdx];
        if (inst.result >= 0) {
            bodyDefinedValues.insert(inst.result);
        }
    }

    const auto bodyOperandExpr = [&](mir::ValueId vid) -> std::string {
        if (bodyDefinedValues.find(vid) != bodyDefinedValues.end()) {
            return "vecReg_" + std::to_string(vid);
        }
        return typedValueExpression(vid);
    };

    const auto vecRegCppType = [&](mir::ValueId vid) -> std::string {
        const auto& slot = types[static_cast<std::size_t>(vid)];
        if (slot.primitive == semantic::PrimitiveType::Int) {
            return "std::int32_t";
        }
        return typedCppType(slot);
    };

    const auto scalarConvertCppType = [](semantic::PrimitiveType p) -> std::string {
        if (p == semantic::PrimitiveType::Bool) {
            return "bool";
        }
        return packedElemCppType(p);
    };

    source.line("                if (RS_AOT_LIKELY(" + validCachesCond +
        " && (vecStart >= 0) && (vecEnd >= static_cast<std::int64_t>(INT32_MIN)) && "
        "(vecEnd <= static_cast<std::int64_t>(INT32_MAX)) && "
        "((vecEnd <= vecStart) || (" + activeTripGuard + ")))) {");
    source.line("                    const std::uint64_t vecTrips = (vecEnd > vecStart) "
        "? static_cast<std::uint64_t>(vecEnd - vecStart) : 0ULL;");
    source.line("                    const std::uint64_t vecTotalInsts = vecTrips * " +
        std::to_string(plan.iterInstsWithTerminators) + "ULL + " +
        std::to_string(plan.headerInstsWithTerminator) + "ULL;");
    source.line("                    const std::uint64_t vecTotalBranches = vecTrips * " +
        std::to_string(plan.iterBranches) + "ULL + 1ULL;");
    source.line("                    if (TrustedRelease || rawAccounting.canConsume(vecTotalInsts)) {");
    source.line("                        if constexpr (FastAccounting && !TrustedRelease) {");
    source.line("                            (void)rawAccounting.consume(vecTotalInsts);");
    source.line("                            rawAccounting.addBranches(vecTotalBranches);");
    source.line("                        }");
    source.line("                        if (vecTrips > 0ULL) {");
    for (std::size_t bIdx = 0; bIdx < details.bindings.size(); ++bIdx) {
        const auto& b = details.bindings[bIdx];
        const std::string ptrName = "packedPtr_" + std::to_string(bIdx);
        const std::string elemTypeStr = packedElemCppType(b.elemPrim);
        if (b.isWrite) {
            source.line("                            " + elemTypeStr + "* RS_AOT_RESTRICT " +
                ptrName + " = " + metas[bIdx].rawDataExpr + ";");
        } else {
            source.line("                            const " + elemTypeStr + "* RS_AOT_RESTRICT " +
                ptrName + " = " + metas[bIdx].rawDataExpr + ";");
        }
        source.line("                            (void)" + ptrName + ";");
        for (std::size_t oIdx = 0; oIdx < b.offsets.size(); ++oIdx) {
            if (b.offsets[oIdx].isZero()) continue;
            const std::string sB = std::to_string(bIdx);
            const std::string sO = std::to_string(oIdx);
            source.line("                            const std::ptrdiff_t vecBaseOff_" + sB + "_" + sO +
                " = static_cast<std::ptrdiff_t>(vecOff_" + sB + "_" + sO + ");");
        }
    }
    source.line("                            const auto vecStartIdx = static_cast<std::ptrdiff_t>(vecStart);");
    source.line("                            const auto vecEndIdx = static_cast<std::ptrdiff_t>(vecEnd);");
    source.line("                            RS_AOT_PRAGMA_SIMD_LOOP");
    source.line("                            for (std::ptrdiff_t vecIdx = vecStartIdx; vecIdx < vecEndIdx; ++vecIdx) {");

    const std::string ind = "                                ";
    for (std::size_t instIdx = 0; instIdx < bodyBlock->instructions.size(); ++instIdx) {
        if (details.skipBodyInst[instIdx]) {
            continue;
        }
        const auto& inst = bodyBlock->instructions[instIdx];
        const std::string resDecl = (inst.result >= 0)
            ? ("[[maybe_unused]] const " + vecRegCppType(inst.result) +
               " vecReg_" + std::to_string(inst.result))
            : std::string{};
        const auto op = [&](std::size_t idx) {
            return bodyOperandExpr(inst.operands[idx]);
        };
        const auto accessIndexExpr = [&](std::size_t bIdx, std::size_t oIdx) -> std::string {
            if (details.bindings[bIdx].offsets[oIdx].isZero()) {
                return "vecIdx";
            }
            return "vecIdx + vecBaseOff_" + std::to_string(bIdx) + "_" + std::to_string(oIdx);
        };

        switch (inst.opcode) {
        case mir::Opcode::LoadLocal:
            if (inst.localIndex == plan.indexLocalIndex) {
                source.line(ind + resDecl + " = static_cast<std::int32_t>(vecIdx);");
            } else if (function.localTypes[inst.localIndex] == semantic::PrimitiveType::Int) {
                source.line(ind + resDecl + " = static_cast<std::int32_t>(" +
                    typedLocalExpression(inst.localIndex) + ");");
            } else {
                source.line(ind + resDecl + " = " +
                    typedLocalExpression(inst.localIndex) + ";");
            }
            break;
        case mir::Opcode::StoreLocal:
            if (function.localTypes[inst.localIndex] == semantic::PrimitiveType::Int) {
                source.line(ind + typedLocalExpression(inst.localIndex) +
                    " = static_cast<std::int64_t>(" + op(0) + ");");
            } else {
                source.line(ind + typedLocalExpression(inst.localIndex) + " = " + op(0) + ";");
            }
            break;
        case mir::Opcode::LoadStructField:
            if (inst.resultType == semantic::PrimitiveType::Int) {
                source.line(ind + resDecl + " = static_cast<std::int32_t>(" + op(0) +
                    ".f_" + std::to_string(inst.fieldIndex) + ");");
            } else {
                source.line(ind + resDecl + " = " + op(0) +
                    ".f_" + std::to_string(inst.fieldIndex) + ";");
            }
            break;
        case mir::Opcode::LoadElement: {
            const auto [bIdx, oIdx] = details.instElementAccess.at(instIdx);
            const std::string elemExpr =
                "packedPtr_" + std::to_string(bIdx) + "[" + accessIndexExpr(bIdx, oIdx) + "]";
            if (inst.elementType == semantic::PrimitiveType::Bool) {
                if (inst.resultType == semantic::PrimitiveType::Bool) {
                    source.line(ind + resDecl + " = (" + elemExpr + " != 0);");
                } else {
                    source.line(ind + resDecl + " = static_cast<" + vecRegCppType(inst.result) +
                        ">(" + elemExpr + " != 0 ? 1 : 0);");
                }
            } else {
                source.line(ind + resDecl + " = static_cast<" + vecRegCppType(inst.result) +
                    ">(" + elemExpr + ");");
            }
            break;
        }
        case mir::Opcode::StoreElement: {
            const auto [bIdx, oIdx] = details.instElementAccess.at(instIdx);
            const std::string targetExpr =
                "packedPtr_" + std::to_string(bIdx) + "[" + accessIndexExpr(bIdx, oIdx) + "]";
            if (inst.elementType == semantic::PrimitiveType::Bool) {
                source.line(ind + targetExpr + " = static_cast<std::uint8_t>((" + op(2) + ") ? 1 : 0);");
            } else {
                source.line(ind + targetExpr + " = static_cast<" +
                    packedElemCppType(inst.elementType) + ">(" + op(2) + ");");
            }
            break;
        }
        case mir::Opcode::ConstantInt:
            if (inst.resultType == semantic::PrimitiveType::Long ||
                inst.resultType == semantic::PrimitiveType::ULong ||
                inst.resultType == semantic::PrimitiveType::UInt ||
                inst.resultType == semantic::PrimitiveType::Byte ||
                inst.resultType == semantic::PrimitiveType::SByte ||
                inst.resultType == semantic::PrimitiveType::Short ||
                inst.resultType == semantic::PrimitiveType::UShort) {
                source.line(ind + resDecl + " = std::int64_t{" +
                    int64Literal(inst.integerImmediate) + "};");
            } else {
                source.line(ind + resDecl + " = static_cast<std::int32_t>(" +
                    int64Literal(inst.integerImmediate) + ");");
            }
            break;
        case mir::Opcode::ConstantDouble: {
            std::ostringstream lit;
            lit << std::setprecision(17) << inst.doubleImmediate;
            if (inst.resultType == semantic::PrimitiveType::Float) {
                source.line(ind + resDecl + " = static_cast<float>(" + lit.str() + ");");
            } else {
                source.line(ind + resDecl + " = " + lit.str() + ";");
            }
            break;
        }
        case mir::Opcode::ConstantBool:
            source.line(ind + resDecl + " = " +
                (inst.boolImmediate ? "true" : "false") + ";");
            break;
        case mir::Opcode::ConvertIntToLong:
            source.line(ind + resDecl + " = static_cast<std::int64_t>(" + op(0) + ");");
            break;
        case mir::Opcode::ConvertIntToDouble:
        case mir::Opcode::ConvertLongToDouble:
            source.line(ind + resDecl + " = static_cast<" + vecRegCppType(inst.result) +
                ">(" + op(0) + ");");
            break;
        case mir::Opcode::ConvertNumeric: {
            const auto srcPrim = types[static_cast<std::size_t>(inst.operands[0])].primitive;
            const std::string srcScalar = scalarConvertCppType(srcPrim);
            const std::string dstScalar = scalarConvertCppType(inst.resultType);
            const std::string dstReg = vecRegCppType(inst.result);
            source.line(ind + resDecl + " = static_cast<" + dstReg + ">("
                "static_cast<" + dstScalar + ">(static_cast<" + srcScalar + ">(" + op(0) + ")));");
            break;
        }
        case mir::Opcode::AddInt:
        case mir::Opcode::SubtractInt:
        case mir::Opcode::MultiplyInt: {
            const char* sym = inst.opcode == mir::Opcode::AddInt
                ? " + "
                : (inst.opcode == mir::Opcode::SubtractInt ? " - " : " * ");
            source.line(ind + resDecl +
                " = static_cast<std::int32_t>(static_cast<std::uint32_t>(" + op(0) + ")" +
                sym + "static_cast<std::uint32_t>(" + op(1) + "));");
            break;
        }
        case mir::Opcode::NegateInt:
            source.line(ind + resDecl +
                " = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(" + op(0) + "));");
            break;
        case mir::Opcode::AddLong:
        case mir::Opcode::SubtractLong:
        case mir::Opcode::MultiplyLong: {
            const char* sym = inst.opcode == mir::Opcode::AddLong
                ? " + "
                : (inst.opcode == mir::Opcode::SubtractLong ? " - " : " * ");
            source.line(ind + resDecl +
                " = static_cast<std::int64_t>(static_cast<std::uint64_t>(" + op(0) + ")" +
                sym + "static_cast<std::uint64_t>(" + op(1) + "));");
            break;
        }
        case mir::Opcode::NegateLong:
            source.line(ind + resDecl +
                " = static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(" + op(0) + "));");
            break;
        case mir::Opcode::AddDouble:
        case mir::Opcode::SubtractDouble:
        case mir::Opcode::MultiplyDouble:
        case mir::Opcode::DivideDouble: {
            const char* sym = inst.opcode == mir::Opcode::AddDouble
                ? " + "
                : (inst.opcode == mir::Opcode::SubtractDouble
                    ? " - "
                    : (inst.opcode == mir::Opcode::MultiplyDouble ? " * " : " / "));
            source.line(ind + resDecl + " = " + op(0) + sym + op(1) + ";");
            break;
        }
        case mir::Opcode::NegateDouble:
            source.line(ind + resDecl + " = -" + op(0) + ";");
            break;
        case mir::Opcode::LogicalNot:
            source.line(ind + resDecl + " = !" + op(0) + ";");
            break;
        case mir::Opcode::Equal:
        case mir::Opcode::NotEqual: {
            const auto lhsPrim = types[static_cast<std::size_t>(inst.operands[0])].primitive;
            const char* cmpOp = (inst.opcode == mir::Opcode::Equal) ? " == " : " != ";
            if (lhsPrim == semantic::PrimitiveType::Int) {
                source.line(ind + resDecl + " = (static_cast<std::int32_t>(" + op(0) + ")" +
                    cmpOp + "static_cast<std::int32_t>(" + op(1) + "));");
            } else {
                source.line(ind + resDecl + " = (" + op(0) + cmpOp + op(1) + ");");
            }
            break;
        }
        case mir::Opcode::LessInt:
        case mir::Opcode::LessOrEqualInt:
        case mir::Opcode::GreaterInt:
        case mir::Opcode::GreaterOrEqualInt: {
            const char* cmpOp = inst.opcode == mir::Opcode::LessInt
                ? " < "
                : (inst.opcode == mir::Opcode::LessOrEqualInt
                    ? " <= "
                    : (inst.opcode == mir::Opcode::GreaterInt ? " > " : " >= "));
            source.line(ind + resDecl + " = (static_cast<std::int32_t>(" + op(0) + ")" +
                cmpOp + "static_cast<std::int32_t>(" + op(1) + "));");
            break;
        }
        case mir::Opcode::LessLong:
        case mir::Opcode::LessOrEqualLong:
        case mir::Opcode::GreaterLong:
        case mir::Opcode::GreaterOrEqualLong:
        case mir::Opcode::LessDouble:
        case mir::Opcode::LessOrEqualDouble:
        case mir::Opcode::GreaterDouble:
        case mir::Opcode::GreaterOrEqualDouble: {
            const char* cmpOp =
                (inst.opcode == mir::Opcode::LessLong || inst.opcode == mir::Opcode::LessDouble)
                ? " < "
                : ((inst.opcode == mir::Opcode::LessOrEqualLong ||
                    inst.opcode == mir::Opcode::LessOrEqualDouble)
                    ? " <= "
                    : ((inst.opcode == mir::Opcode::GreaterLong ||
                        inst.opcode == mir::Opcode::GreaterDouble)
                        ? " > "
                        : " >= "));
            source.line(ind + resDecl + " = (" + op(0) + cmpOp + op(1) + ");");
            break;
        }
        default:
            break;
        }
    }

    source.line("                            }");
    source.line("                            " + idxLocalExpr + " = vecEnd;");
    source.line("                        }");
    source.line("                        goto typedBlock_" + std::to_string(plan.exitBlockId) + ";");
    source.line("                    }");
    source.line("                }");
    source.line("            }");
}


} // namespace realscript::aot::detail
