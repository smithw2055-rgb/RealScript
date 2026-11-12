#include "AotCppInternal.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace realscript::aot::detail {

bool aotTypeAssignable(
    semantic::SymbolId actual,
    semantic::SymbolId expected,
    const std::map<semantic::SymbolId, TypeView>& allTypes) noexcept {
    if (actual == expected) return true;
    if (actual == 0 || expected == 0) return false;
    auto current = actual;
    for (std::size_t depth = 0; current != 0 && depth < 256; ++depth) {
        const auto it = allTypes.find(current);
        if (it == allTypes.end() || !it->second.type) return false;
        for (const auto& imap : it->second.type->interfaceDispatchMaps) {
            if (imap.interfaceTypeId == expected) return true;
        }
        if (it->second.type->baseTypeId == expected) return true;
        current = it->second.type->baseTypeId;
    }
    return false;
}

void emitTypedPrimitiveFunction(
    Emitter& source,
    const FunctionView& view,
    const GenerationOptions& options,
    std::vector<SourceMapRecord>& sourceMap,
    const std::unordered_map<semantic::SymbolId, const FunctionView*>& typedFunctions,
    const std::map<semantic::SymbolId, TypeView>& allTypes,
    const std::unordered_map<const mir::Instruction*, std::size_t>& callIds) {
    const auto& module = *view.module;
    const auto& function = *view.function;
    const auto types = valueSlotTypes(function);
    const auto rangeAnalysis = analyzeTypedIntegerRanges(function);
    std::string safeArithmeticValues;
    for (const auto value : rangeAnalysis.safeCheckedArithmetic) {
        if (!safeArithmeticValues.empty()) safeArithmeticValues += ",";
        safeArithmeticValues += std::to_string(value);
    }
    source.line("// range-proven checked arithmetic values: " +
        (safeArithmeticValues.empty() ? std::string("none")
                                      : safeArithmeticValues));
    source.line(typedScalarFunctionSignature(view, false));
    source.line("    (void)context;");
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        source.line("    (void)typedArg_" + std::to_string(index) + ";");
    }
    if (function.returnType != semantic::PrimitiveType::Void) {
        source.line("    (void)typedOutResult;");
    }
    source.line("    ExecutionContext::TypedRawAccountingScope<FastAccounting && !TrustedRelease> rawAccounting(context);");
    source.line("    ExecutionContext::TypedDeterminismAccountingScope<DeterminismAccounting && !TrustedRelease> determinismAccounting(context);");
    std::vector<std::size_t> paramGuardSlot(
        function.parameterTypes.size(),
        std::numeric_limits<std::size_t>::max());
    std::vector<std::size_t> localGuardSlot(
        function.localTypes.size(),
        std::numeric_limits<std::size_t>::max());
    std::vector<std::size_t> regGuardSlot(
        types.size(),
        std::numeric_limits<std::size_t>::max());
    std::size_t totalGcGuardSlots = 0;
    bool hasRefParams = false;
    for (std::size_t pIdx = 0; pIdx < function.parameterTypes.size(); ++pIdx) {
        if (function.parameterTypes[pIdx] == semantic::PrimitiveType::Object ||
            function.parameterTypes[pIdx] == semantic::PrimitiveType::Array) {
            paramGuardSlot[pIdx] = totalGcGuardSlots++;
            hasRefParams = true;
        }
    }
    for (std::size_t lIdx = 0; lIdx < function.localTypes.size(); ++lIdx) {
        if (function.localTypes[lIdx] == semantic::PrimitiveType::Object ||
            function.localTypes[lIdx] == semantic::PrimitiveType::Array) {
            localGuardSlot[lIdx] = totalGcGuardSlots++;
        }
    }
    for (std::size_t rIdx = 0; rIdx < types.size(); ++rIdx) {
        if (types[rIdx].primitive == semantic::PrimitiveType::Object ||
            types[rIdx].primitive == semantic::PrimitiveType::Array) {
            regGuardSlot[rIdx] = totalGcGuardSlots++;
        }
    }
    if (totalGcGuardSlots > 0) {
        source.line("    std::conditional_t<!FastAccounting && !DeterminismAccounting && !TrustedRelease, std::array<runtime::PersistentRoot, " +
            std::to_string(totalGcGuardSlots) + ">, aot::EmptyGcGuards> typedLocalGcGuards{};");
    } else {
        source.line("    aot::EmptyGcGuards typedLocalGcGuards{};");
    }
    source.line("    (void)typedLocalGcGuards;");
    if (hasRefParams) {
        source.line("    if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
        source.line("        if (auto* rawH = context.rawHeap()) {");
        for (std::size_t pIdx = 0; pIdx < function.parameterTypes.size(); ++pIdx) {
            if (paramGuardSlot[pIdx] != std::numeric_limits<std::size_t>::max()) {
                source.line("            aot::retainTypedGcSlot(rawH, typedLocalGcGuards[" +
                    std::to_string(paramGuardSlot[pIdx]) + "], typedArg_" +
                    std::to_string(pIdx) + ");");
            }
        }
        source.line("        }");
        source.line("    }");
    }
    source.line("    std::uint32_t typedActiveGenLine = 0;");
    source.line("    (void)typedActiveGenLine;");
    source.line("    const auto typedFail = [&](runtime::ErrorCode code, const char* message) {");
    source.line("        if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush();");
    source.line("        context.setActiveGeneratedLine(typedActiveGenLine);");
    source.line("        return context.fail(code, message, typedActiveGenLine);");
    source.line("    };");
    source.line("    (void)typedFail;");
    const bool isAsyncFunction = !function.blocks.empty() &&
        !function.blocks.front().instructions.empty() &&
        function.blocks.front().instructions.front().opcode == mir::Opcode::AsyncEnter;
    std::vector<std::size_t> localStoreCounts(function.localTypes.size(), 0);
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if ((inst.opcode == mir::Opcode::StoreLocal ||
                 inst.opcode == mir::Opcode::AwaitSuspend) &&
                inst.localIndex < localStoreCounts.size()) {
                ++localStoreCounts[inst.localIndex];
            }
        }
    }
    const semantic::SymbolId kPackedInt32ArrayTid = semantic::stableTypeId("int[]");
    const semantic::SymbolId kPackedDoubleArrayTid = semantic::stableTypeId("double[]");
    std::unordered_map<std::size_t, semantic::PrimitiveType> arrayParamPackedPrim;
    for (std::size_t pIdx = 0; pIdx < function.parameterTypes.size(); ++pIdx) {
        if (function.parameterTypes[pIdx] != semantic::PrimitiveType::Array) continue;
        const auto pTid = pIdx < function.parameterTypeIds.size() ? function.parameterTypeIds[pIdx] : 0;
        if (pTid == kPackedInt32ArrayTid) {
            arrayParamPackedPrim[pIdx] = semantic::PrimitiveType::Int;
        } else if (pTid == kPackedDoubleArrayTid) {
            arrayParamPackedPrim[pIdx] = semantic::PrimitiveType::Double;
        }
    }
    std::unordered_map<std::size_t, semantic::PrimitiveType> arrayLocalPackedPrim;
    for (std::size_t lIdx = 0; lIdx < function.localTypes.size(); ++lIdx) {
        if (function.localTypes[lIdx] != semantic::PrimitiveType::Array) continue;
        const auto lTid = lIdx < function.localTypeIds.size() ? function.localTypeIds[lIdx] : 0;
        if (lTid == kPackedInt32ArrayTid) {
            arrayLocalPackedPrim[lIdx] = semantic::PrimitiveType::Int;
        } else if (lTid == kPackedDoubleArrayTid) {
            arrayLocalPackedPrim[lIdx] = semantic::PrimitiveType::Double;
        }
    }
    std::unordered_map<mir::ValueId, std::size_t> valueToArrayLocal;
    std::unordered_map<mir::ValueId, std::size_t> valueToArrayParam;
    std::unordered_map<std::size_t, semantic::SymbolId> arrayLocalStructElemType;
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.opcode == mir::Opcode::LoadLocal &&
                inst.result >= 0 &&
                inst.localIndex < function.localTypes.size() &&
                function.localTypes[inst.localIndex] == semantic::PrimitiveType::Array &&
                localStoreCounts[inst.localIndex] == 1) {
                valueToArrayLocal[inst.result] = inst.localIndex;
            } else if (inst.opcode == mir::Opcode::Parameter &&
                       inst.result >= 0) {
                const auto pIdx = static_cast<std::size_t>(inst.integerImmediate);
                if (pIdx < function.parameterTypes.size() &&
                    function.parameterTypes[pIdx] == semantic::PrimitiveType::Array) {
                    valueToArrayParam[inst.result] = pIdx;
                }
            }
        }
    }
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if ((inst.opcode == mir::Opcode::LoadElement ||
                 inst.opcode == mir::Opcode::StoreElement) &&
                !inst.operands.empty()) {
                const auto it = valueToArrayLocal.find(inst.operands[0]);
                if (it != valueToArrayLocal.end()) {
                    if (inst.elementType == semantic::PrimitiveType::Struct &&
                        classifyInlineMath(inst.elementType, inst.elementTypeId) ==
                            InlineMathKind::None) {
                        arrayLocalStructElemType[it->second] = inst.elementTypeId;
                    } else if (inst.elementTypeId == 0 &&
                               (inst.elementType == semantic::PrimitiveType::Int ||
                                inst.elementType == semantic::PrimitiveType::Double)) {
                        arrayLocalPackedPrim.try_emplace(it->second, inst.elementType);
                    }
                }
            }
        }
    }
    std::unordered_map<std::size_t, semantic::PrimitiveType> singleStorePackedArrayLocals;
    for (const auto& [lIdx, prim] : arrayLocalPackedPrim) {
        if (lIdx < localStoreCounts.size() && localStoreCounts[lIdx] == 1) {
            singleStorePackedArrayLocals.emplace(lIdx, prim);
        }
    }
    for (const auto& [pIdx, prim] : arrayParamPackedPrim) {
        if (prim == semantic::PrimitiveType::Int) {
            source.line("    const aot::TypedPackedInt32ArrayCache typedArgPackedI32_" +
                std::to_string(pIdx) + " = context.acquirePackedInt32ArrayCache(typedArg_" +
                std::to_string(pIdx) + ");");
            source.line("    (void)typedArgPackedI32_" + std::to_string(pIdx) + ";");
        } else {
            source.line("    const aot::TypedPackedDoubleArrayCache typedArgPackedF64_" +
                std::to_string(pIdx) + " = context.acquirePackedDoubleArrayCache(typedArg_" +
                std::to_string(pIdx) + ");");
            source.line("    (void)typedArgPackedF64_" + std::to_string(pIdx) + ";");
        }
    }
    for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
        const auto typeId = index < function.localTypeIds.size()
            ? function.localTypeIds[index]
            : 0;
        const TypedSlotType localSlot{function.localTypes[index], typeId};
        source.line("    [[maybe_unused]] " + typedCppType(localSlot) + " " +
            typedLocalExpression(index) + " = " +
            typedDefaultValue(localSlot) + ";");
        source.line("    (void)" + typedLocalExpression(index) + ";");
        if (function.localTypes[index] == semantic::PrimitiveType::Array) {
            source.line("    aot::TypedRawColumnCache typedLocalRawCol_" +
                std::to_string(index) + "{};");
            source.line("    (void)typedLocalRawCol_" + std::to_string(index) + ";");
            const auto packedIt = arrayLocalPackedPrim.find(index);
            if (packedIt != arrayLocalPackedPrim.end()) {
                if (packedIt->second == semantic::PrimitiveType::Int) {
                    source.line("    aot::TypedPackedInt32ArrayCache typedLocalPackedI32_" +
                        std::to_string(index) + "{};");
                    source.line("    (void)typedLocalPackedI32_" + std::to_string(index) + ";");
                } else {
                    source.line("    aot::TypedPackedDoubleArrayCache typedLocalPackedF64_" +
                        std::to_string(index) + "{};");
                    source.line("    (void)typedLocalPackedF64_" + std::to_string(index) + ";");
                }
            }
        }
    }
    for (std::size_t index = 0; index < types.size(); ++index) {
        source.line("    [[maybe_unused]] " + typedCppType(types[index]) + " " +
            typedValueExpression(static_cast<mir::ValueId>(index)) + "{};");
    }
    if (options.emitLineDirectives && !function.debugInfo.locals.empty()) {
        std::unordered_map<std::string, std::size_t> debugNameCounts;
        for (const auto& local : function.debugInfo.locals) {
            if (!local.name.empty() && local.name.front() != '$') ++debugNameCounts[sanitizeCppIdentifier(local.name)];
        }
        std::set<std::string> emittedDebugAliases;
        for (const auto& local : function.debugInfo.locals) {
            if (local.name.empty() || local.name.front() == '$') continue;
            const std::string clean = sanitizeCppIdentifier(local.name);
            std::string aliasName = "dbg_" + clean;
            if (debugNameCounts[clean] > 1) {
                aliasName += (local.parameter ? "_arg" : "_slot") + std::to_string(local.slot);
            }
            if (!emittedDebugAliases.insert(aliasName).second) continue;
            if (local.parameter &&
                local.slot < localStoreCounts.size() &&
                localStoreCounts[local.slot] > 1 &&
                local.slot < function.localTypes.size()) {
                source.line("    [[maybe_unused]] auto& " + aliasName +
                    " = " + typedLocalExpression(local.slot) + ";");
            } else if (local.parameter && local.slot < function.parameterTypes.size()) {
                source.line("    [[maybe_unused]] auto& " + aliasName +
                    " = typedArg_" + std::to_string(local.slot) + ";");
            } else if (!local.parameter && local.slot < function.localTypes.size()) {
                source.line("    [[maybe_unused]] auto& " + aliasName +
                    " = " + typedLocalExpression(local.slot) + ";");
            }
        }
    }
    struct CoroSavedField {
        std::string cppType;
        std::string exprName;
        std::string memberName;
        std::size_t guardSlot = std::numeric_limits<std::size_t>::max();
        std::size_t arrayLocalIndex = std::numeric_limits<std::size_t>::max();
    };
    std::vector<CoroSavedField> podCoroFields;
    std::vector<CoroSavedField> refCoroFields;
    std::vector<std::uint32_t> asyncResumeBlockIds;
    if (isAsyncFunction) {
        std::set<std::uint32_t> resumeBlockSet;
        std::vector<std::int64_t> defBlock(types.size(), -1);
        std::vector<std::size_t> defInstIdx(types.size(), 0);
        std::vector<bool> regLiveAcrossSuspend(types.size(), false);
        for (const auto& blk : function.blocks) {
            for (const auto& bp : blk.parameters) {
                if (bp.value >= 0 && static_cast<std::size_t>(bp.value) < types.size()) {
                    regLiveAcrossSuspend[static_cast<std::size_t>(bp.value)] = true;
                }
            }
            for (std::size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
                const auto& inst = blk.instructions[iIdx];
                if (inst.opcode == mir::Opcode::AwaitSuspend) {
                    resumeBlockSet.insert(static_cast<std::uint32_t>(inst.integerImmediate));
                }
                if (inst.result >= 0 && static_cast<std::size_t>(inst.result) < types.size()) {
                    defBlock[static_cast<std::size_t>(inst.result)] = static_cast<std::int64_t>(blk.id);
                    defInstIdx[static_cast<std::size_t>(inst.result)] = iIdx;
                }
            }
        }
        for (const auto& blk : function.blocks) {
            std::vector<std::size_t> suspendPositions;
            for (std::size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
                if (blk.instructions[iIdx].opcode == mir::Opcode::AwaitSuspend) {
                    suspendPositions.push_back(iIdx);
                }
            }
            auto checkUse = [&](mir::ValueId vid, std::size_t usePos) {
                if (vid < 0 || static_cast<std::size_t>(vid) >= types.size()) return;
                const auto idx = static_cast<std::size_t>(vid);
                if (defBlock[idx] != static_cast<std::int64_t>(blk.id)) {
                    regLiveAcrossSuspend[idx] = true;
                    return;
                }
                for (const auto sp : suspendPositions) {
                    if (defInstIdx[idx] < sp && usePos > sp) {
                        regLiveAcrossSuspend[idx] = true;
                        return;
                    }
                }
            };
            for (std::size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
                for (const auto op : blk.instructions[iIdx].operands) {
                    checkUse(op, iIdx);
                }
            }
            const std::size_t termPos = blk.instructions.size();
            checkUse(blk.terminator.value, termPos);
            checkUse(blk.terminator.condition, termPos);
            for (const auto arg : blk.terminator.arguments) checkUse(arg, termPos);
            for (const auto arg : blk.terminator.falseArguments) checkUse(arg, termPos);
        }
        asyncResumeBlockIds.assign(resumeBlockSet.begin(), resumeBlockSet.end());

        auto addSlotField = [&](
            TypedSlotType slot,
            const std::string& expr,
            const std::string& member,
            std::size_t guardSlot = std::numeric_limits<std::size_t>::max(),
            std::size_t arrayLocIdx = std::numeric_limits<std::size_t>::max()) {
            if (slot.primitive == semantic::PrimitiveType::Void ||
                slot.primitive == semantic::PrimitiveType::Error) {
                return;
            }
            const bool isRef =
                slot.primitive == semantic::PrimitiveType::Object ||
                slot.primitive == semantic::PrimitiveType::Array ||
                slot.primitive == semantic::PrimitiveType::Null;
            if (isRef) {
                refCoroFields.push_back({
                    typedCppType(slot),
                    expr,
                    member,
                    guardSlot,
                    arrayLocIdx,
                });
            } else {
                podCoroFields.push_back({
                    typedCppType(slot),
                    expr,
                    member,
                });
            }
        };

        for (std::size_t pIdx = 0; pIdx < function.parameterTypes.size(); ++pIdx) {
            const auto tid = pIdx < function.parameterTypeIds.size() ? function.parameterTypeIds[pIdx] : 0;
            addSlotField(
                {function.parameterTypes[pIdx], tid},
                "typedArg_" + std::to_string(pIdx),
                "arg_" + std::to_string(pIdx),
                paramGuardSlot[pIdx]);
        }
        for (std::size_t lIdx = 0; lIdx < function.localTypes.size(); ++lIdx) {
            const auto tid = lIdx < function.localTypeIds.size() ? function.localTypeIds[lIdx] : 0;
            const auto arrLocIdx = function.localTypes[lIdx] == semantic::PrimitiveType::Array
                ? lIdx
                : std::numeric_limits<std::size_t>::max();
            addSlotField(
                {function.localTypes[lIdx], tid},
                typedLocalExpression(lIdx),
                "loc_" + std::to_string(lIdx),
                localGuardSlot[lIdx],
                arrLocIdx);
        }
        for (std::size_t rIdx = 0; rIdx < types.size(); ++rIdx) {
            if (!regLiveAcrossSuspend[rIdx]) continue;
            addSlotField(
                types[rIdx],
                typedValueExpression(static_cast<mir::ValueId>(rIdx)),
                "reg_" + std::to_string(rIdx),
                regGuardSlot[rIdx]);
        }

        source.line("    struct ScalarCoroPodState {");
        if (podCoroFields.empty()) {
            source.line("        std::uint8_t _empty = 0;");
        } else {
            for (const auto& f : podCoroFields) {
                source.line("        " + f.cppType + " " + f.memberName + "{};");
            }
        }
        source.line("    };");
        source.line("    static_assert(std::is_trivially_copyable_v<ScalarCoroPodState>, "
                    "\"ScalarCoroPodState must be trivially copyable\");");
        source.line("    runtime::CoroutineFrameHandle activeCoroutine{};");
        source.line("    (void)activeCoroutine;");
        if (!asyncResumeBlockIds.empty()) {
            source.line("    {");
            source.line("        std::uint32_t resumeBlockId = 0;");
            source.line("        ScalarCoroPodState restoredPod{};");
            if (!refCoroFields.empty()) {
                source.line("        std::array<runtime::Value, " +
                    std::to_string(refCoroFields.size()) + "> restoredRefs{};");
            }
            const std::string refPtrExpr = refCoroFields.empty()
                ? std::string("nullptr")
                : std::string("restoredRefs.data()");
            source.line("        if (RS_AOT_UNLIKELY(context.enterResumingScalarCoroutine(0x" +
                hexId(function.symbolId) + "ULL, &restoredPod, sizeof(ScalarCoroPodState), " +
                refPtrExpr + ", " + std::to_string(refCoroFields.size()) +
                "u, resumeBlockId, activeCoroutine))) {");
            for (const auto& f : podCoroFields) {
                source.line("            " + f.exprName + " = restoredPod." + f.memberName + ";");
            }
            for (std::size_t rfIdx = 0; rfIdx < refCoroFields.size(); ++rfIdx) {
                source.line("            " + refCoroFields[rfIdx].exprName +
                    " = restoredRefs[" + std::to_string(rfIdx) + "];");
            }
            bool anyHeapRefField = false;
            for (const auto& rf : refCoroFields) {
                if (rf.guardSlot != std::numeric_limits<std::size_t>::max()) {
                    anyHeapRefField = true;
                    break;
                }
            }
            if (anyHeapRefField) {
                source.line("            if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
                source.line("                if (auto* rawH = context.rawHeap()) {");
                for (const auto& rf : refCoroFields) {
                    if (rf.guardSlot == std::numeric_limits<std::size_t>::max()) continue;
                    source.line("                    aot::retainTypedGcSlot(rawH, typedLocalGcGuards[" +
                        std::to_string(rf.guardSlot) + "], " + rf.exprName + ");");
                }
                source.line("                }");
                source.line("            }");
            }
            for (const auto& rf : refCoroFields) {
                if (rf.arrayLocalIndex == std::numeric_limits<std::size_t>::max()) continue;
                const auto lIdx = rf.arrayLocalIndex;
                const auto elemTypeIt = arrayLocalStructElemType.find(lIdx);
                if (elemTypeIt != arrayLocalStructElemType.end()) {
                    const std::string structName = "AotStruct_" + hexId(elemTypeIt->second);
                    source.line("            typedLocalRawCol_" + std::to_string(lIdx) +
                        " = context.acquireRawColumnCache(" + typedLocalExpression(lIdx) +
                        ", 0x" + hexId(elemTypeIt->second) + "ULL, sizeof(" +
                        structName + "), " + structName + "_layout, " +
                        structName + "::kFieldCount);");
                } else {
                    source.line("            typedLocalRawCol_" + std::to_string(lIdx) +
                        " = context.acquireRawColumnCache(" + typedLocalExpression(lIdx) + ");");
                }
                const auto packedIt = arrayLocalPackedPrim.find(lIdx);
                if (packedIt != arrayLocalPackedPrim.end()) {
                    if (packedIt->second == semantic::PrimitiveType::Int) {
                        source.line("            typedLocalPackedI32_" + std::to_string(lIdx) +
                            " = context.acquirePackedInt32ArrayCache(" + typedLocalExpression(lIdx) + ");");
                    } else {
                        source.line("            typedLocalPackedF64_" + std::to_string(lIdx) +
                            " = context.acquirePackedDoubleArrayCache(" + typedLocalExpression(lIdx) + ");");
                    }
                }
            }
            source.line("            switch (resumeBlockId) {");
            for (const auto rbId : asyncResumeBlockIds) {
                source.line("            case " + std::to_string(rbId) +
                    "u: goto typedBlock_" + std::to_string(rbId) + ";");
            }
            source.line("            default: return typedFail(runtime::ErrorCode::InvalidProgram, "
                        "\"AOT scalar coroutine resume block is invalid\");");
            source.line("            }");
            source.line("        }");
            source.line("    }");
        }
    }
    const auto emitScalarCallFailureReturn = [&](const std::string& indent, const std::string& okVar) {
        if (isAsyncFunction) {
            source.line(indent + "if (!" + okVar + ") {");
            source.line(indent + "    if (context.hasPendingException()) {");
            source.line(indent + "        return context.finishUncaughtAsyncExceptionScalar(activeCoroutine, 0x" +
                hexId(function.returnTypeId) + "ULL, typedOutResult.f_0);");
            source.line(indent + "    }");
            source.line(indent + "    return false;");
            source.line(indent + "}");
        } else {
            source.line(indent + "if (!" + okVar + ") return false;");
        }
    };
    source.line("    goto typedBlock_0;");

    const auto vectorizedEcsLoops =
        analyzeVectorizedEcsLoops(function, arrayLocalStructElemType);
    const auto vectorizedPackedLoops = analyzeVectorizedPackedArrayLoops(
        function, types, rangeAnalysis, singleStorePackedArrayLocals);
    const auto resolvePackedCacheExpr = [&](mir::ValueId arrVal, semantic::PrimitiveType expectedPrim) -> std::string {
        const auto locIt = valueToArrayLocal.find(arrVal);
        if (locIt != valueToArrayLocal.end()) {
            const auto pIt = singleStorePackedArrayLocals.find(locIt->second);
            if (pIt != singleStorePackedArrayLocals.end() && pIt->second == expectedPrim) {
                return packedArrayCacheName(locIt->second, expectedPrim);
            }
        }
        const auto parIt = valueToArrayParam.find(arrVal);
        if (parIt != valueToArrayParam.end()) {
            const auto pIt = arrayParamPackedPrim.find(parIt->second);
            if (pIt != arrayParamPackedPrim.end() && pIt->second == expectedPrim) {
                return (expectedPrim == semantic::PrimitiveType::Int
                    ? "typedArgPackedI32_"
                    : "typedArgPackedF64_") + std::to_string(parIt->second);
            }
        }
        return {};
    };
    std::size_t typedCallSiteCounter = 0;
    for (const auto& block : function.blocks) {
        source.line("    typedBlock_" + std::to_string(block.id) + ":");
        source.line("    {");
        const auto vecLoopIt = vectorizedEcsLoops.find(block.id);
        if (vecLoopIt != vectorizedEcsLoops.end()) {
            emitVectorizedEcsLoop(
                source, module, function, types, vecLoopIt->second, options.emitLineDirectives);
        } else {
            const auto vecPackedIt = vectorizedPackedLoops.find(block.id);
            if (vecPackedIt != vectorizedPackedLoops.end()) {
                emitVectorizedPackedArrayLoop(
                    source, function, types, vecPackedIt->second);
            }
        }
        const auto rawConsumeBatches =
            typedRawConsumeBatches(block, rangeAnalysis);
        bool activeSourceLine = false;
        for (std::size_t instructionIndex = 0;
             instructionIndex < block.instructions.size();
             ++instructionIndex) {
            const auto& instruction = block.instructions[instructionIndex];
            if (activeSourceLine) {
                emitGeneratedLineReset(source, options.emitLineDirectives);
                activeSourceLine = false;
            }
            emitTypedRawConsume(
                source, rawConsumeBatches[instructionIndex], "            ");
            emitTypedConsume(source, instruction.opcode, "            ");
            bool explicitPoint = false;
            const auto resolvedPoint = resolveSequencePoint(
                module,
                function,
                block.id,
                static_cast<std::uint32_t>(instructionIndex),
                false,
                instruction.sourceSpan,
                &explicitPoint);
            const std::uint32_t currentInstGenLine = source.nextLine() + (options.emitLineDirectives && resolvedPoint ? 1u : 0u);
            if (emitLineDirective(
                    source,
                    module,
                    resolvedPoint,
                    options.emitLineDirectives,
                    &sourceMap,
                    function.symbolId,
                    true)) {
                activeSourceLine = true;
            }
            if (resolvedPoint) {
                source.line("            typedActiveGenLine = " + std::to_string(currentInstGenLine) + "u;");
            }
            const auto result = instruction.result >= 0
                ? typedValueExpression(instruction.result)
                : std::string{};
            const auto operand = [&](std::size_t index) {
                return typedValueExpression(instruction.operands[index]);
            };
            const auto operandSlot = [&](std::size_t index) -> TypedSlotType {
                if (index >= instruction.operands.size() ||
                    instruction.operands[index] < 0) return {};
                return types[static_cast<std::size_t>(instruction.operands[index])];
            };
            const auto callOperandExpr = [&](std::size_t index) -> std::string {
                const auto opSlot = operandSlot(index);
                if (index < instruction.parameterTypes.size() &&
                    opSlot.primitive == semantic::PrimitiveType::Null) {
                    if (instruction.parameterTypes[index] == semantic::PrimitiveType::Object) {
                        return "runtime::Value{runtime::NullObject{}}";
                    }
                    if (instruction.parameterTypes[index] == semantic::PrimitiveType::Array) {
                        return "runtime::Value{runtime::NullArray{}}";
                    }
                }
                return operand(index);
            };
            const auto emitRetainRegResult = [&](const std::string& indent = "            ") {
                if (instruction.result < 0) return;
                const auto rIdx = static_cast<std::size_t>(instruction.result);
                if (rIdx >= regGuardSlot.size() ||
                    regGuardSlot[rIdx] == std::numeric_limits<std::size_t>::max()) {
                    return;
                }
                source.line(indent + "if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
                source.line(indent + "    aot::retainTypedGcSlot(context.rawHeap(), typedLocalGcGuards[" +
                    std::to_string(regGuardSlot[rIdx]) + "], " + result + ");");
                source.line(indent + "}");
            };
            switch (instruction.opcode) {
            case mir::Opcode::Parameter: {
                const auto index = static_cast<std::size_t>(
                    instruction.integerImmediate);
                source.line("            " + result + " = typedArg_" +
                    std::to_string(index) + ";");
                break;
            }
            case mir::Opcode::ConstantInt:
                if (instruction.resultType == semantic::PrimitiveType::Enum) {
                    source.line("            " + result + " = runtime::EnumValue{0x" +
                        hexId(instruction.resultTypeId) + "ULL, " +
                        int64Literal(instruction.integerImmediate) + "};");
                } else {
                    source.line("            " + result + " = std::int64_t{" +
                        int64Literal(instruction.integerImmediate) + "};");
                }
                break;
            case mir::Opcode::ConstantNull:
                if (instruction.resultType == semantic::PrimitiveType::Object) {
                    source.line("            " + result + " = runtime::Value{runtime::NullObject{}};");
                } else if (instruction.resultType == semantic::PrimitiveType::Array) {
                    source.line("            " + result + " = runtime::Value{runtime::NullArray{}};");
                } else {
                    source.line("            " + result + " = runtime::Value{std::monostate{}};");
                }
                break;
            case mir::Opcode::ConvertNullToObject:
                source.line("            " + result + " = runtime::Value{runtime::NullObject{}};");
                break;
            case mir::Opcode::ConvertNullToArray:
                source.line("            " + result + " = runtime::Value{runtime::NullArray{}};");
                break;
            case mir::Opcode::NewObject: {
                const auto objTid = instruction.resultTypeId != 0
                    ? instruction.resultTypeId
                    : instruction.typeId;
                source.line("            if (!context.newObject(0x" +
                    hexId(objTid) + "ULL, " + result +
                    ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                emitRetainRegResult();
                break;
            }
            case mir::Opcode::IsType:
                source.line("            if (!context.isTypeObjectFast(" + operand(0) +
                    ", 0x" + hexId(instruction.elementTypeId) + "ULL, " + result +
                    ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                break;
            case mir::Opcode::AsType:
                source.line("            if (!context.asTypeObjectFast(" + operand(0) +
                    ", 0x" + hexId(instruction.elementTypeId) + "ULL, " + result +
                    ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                emitRetainRegResult();
                break;
            case mir::Opcode::CheckNotNull:
                source.line("            if (!context.checkNotNullFast(0x" +
                    hexId(instruction.typeId) + "ULL, " + operand(0) + ", " +
                    result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                emitRetainRegResult();
                break;
            case mir::Opcode::LoadField: {
                if (instruction.resultType == semantic::PrimitiveType::Int) {
                    source.line("            if (!context.loadFieldInt64Fast(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                } else if (instruction.resultType == semantic::PrimitiveType::Object) {
                    source.line("            if (!context.loadFieldObjectFast(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, 0x" +
                        hexId(instruction.resultTypeId) + "ULL, " + operand(0) +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    emitRetainRegResult();
                } else {
                    const auto tmpField = "typedField_" + std::to_string(instructionIndex);
                    source.line("            runtime::Value " + tmpField + ";");
                    source.line("            if (!context.loadField(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                        ", " + tmpField + ") || !aot::typedUnboxScalar(" +
                        primitiveExpression(instruction.resultType) + ", " + tmpField +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    if (instruction.resultType == semantic::PrimitiveType::Object ||
                        instruction.resultType == semantic::PrimitiveType::Array) {
                        emitRetainRegResult();
                    }
                }
                break;
            }
            case mir::Opcode::StoreField: {
                const auto valSlot = operandSlot(1);
                if (valSlot.primitive == semantic::PrimitiveType::Int) {
                    source.line("            if (!context.storeFieldInt64Fast(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                        ", " + operand(1) + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                } else {
                    source.line("            if (!context.storeField(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                        ", aot::typedBoxScalar(" + primitiveExpression(valSlot.primitive) +
                        ", " + operand(1) + "))) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                }
                break;
            }
            case mir::Opcode::NewArray: {
                source.line("            if (!context.newArray(0x" +
                    hexId(instruction.resultTypeId) + "ULL, " +
                    primitiveExpression(instruction.elementType) + ", 0x" +
                    hexId(instruction.elementTypeId) + "ULL, runtime::Value{" +
                    operand(0) + "}, " + result +
                    ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                emitRetainRegResult();
                break;
            }
            case mir::Opcode::ArrayLength: {
                const std::string i32Cache = resolvePackedCacheExpr(
                    instruction.operands[0], semantic::PrimitiveType::Int);
                const std::string f64Cache = i32Cache.empty()
                    ? resolvePackedCacheExpr(instruction.operands[0], semantic::PrimitiveType::Double)
                    : std::string{};
                const std::string& packedCache = !i32Cache.empty() ? i32Cache : f64Cache;
                const auto arrLocalIt = valueToArrayLocal.find(instruction.operands[0]);
                if (!packedCache.empty()) {
                    source.line("            if (RS_AOT_LIKELY(" + packedCache + ".count >= 0)) {");
                    source.line("                " + result + " = " + packedCache + ".count;");
                    source.line("            } else if (!context.arrayLengthFast(" + operand(0) +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                } else if (arrLocalIt != valueToArrayLocal.end()) {
                    const auto rawColExpr = "typedLocalRawCol_" + std::to_string(arrLocalIt->second);
                    source.line("            if (RS_AOT_LIKELY(" + rawColExpr + ".count >= 0)) {");
                    source.line("                " + result + " = " + rawColExpr + ".count;");
                    source.line("            } else if (!context.arrayLengthFast(" + operand(0) +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                } else {
                    source.line("            if (!context.arrayLengthFast(" + operand(0) +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                }
                break;
            }
            case mir::Opcode::LoadElement: {
                if (instruction.elementType == semantic::PrimitiveType::Struct &&
                    classifyInlineMath(instruction.elementType, instruction.elementTypeId) ==
                        InlineMathKind::None) {
                    const auto arrLocalIt = valueToArrayLocal.find(instruction.operands[0]);
                    const std::string structName = "AotStruct_" + hexId(instruction.elementTypeId);
                    const auto tmpElem = "typedElem_" + std::to_string(instructionIndex);
                    if (arrLocalIt != valueToArrayLocal.end()) {
                        const auto rawColExpr = "typedLocalRawCol_" + std::to_string(arrLocalIt->second);
                        source.line("            if (RS_AOT_LIKELY(" + rawColExpr + ".basePtr != nullptr)) {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(" + operand(1) + ") >= static_cast<std::uint64_t>(" + rawColExpr + ".count))) {");
                        source.line("                        return typedFail(runtime::ErrorCode::IndexOutOfRange, \"array element index is out of range\");");
                        source.line("                    }");
                        source.line("                }");
                        source.line("                (void)" + structName + "::loadFromRawColumn(" + rawColExpr + ", " + operand(1) + ", " + result + ");");
                        source.line("            } else {");
                        source.line("                runtime::Value " + tmpElem + ";");
                        source.line("                if (!context.loadElement(semantic::PrimitiveType::Struct, 0x" +
                            hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                            ", runtime::Value{" + operand(1) + "}, " + tmpElem +
                            ") || !aot::typedUnboxScalar(semantic::PrimitiveType::Struct, " + tmpElem +
                            ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                        source.line("            }");
                    } else {
                        source.line("            runtime::Value " + tmpElem + ";");
                        source.line("            if (!context.loadElement(semantic::PrimitiveType::Struct, 0x" +
                            hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                            ", runtime::Value{" + operand(1) + "}, " + tmpElem +
                            ") || !aot::typedUnboxScalar(semantic::PrimitiveType::Struct, " + tmpElem +
                            ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    }
                } else if (instruction.elementType == semantic::PrimitiveType::Int &&
                           instruction.elementTypeId == 0 &&
                           instruction.resultType == semantic::PrimitiveType::Int) {
                    const std::string packedCache = resolvePackedCacheExpr(
                        instruction.operands[0], semantic::PrimitiveType::Int);
                    if (!packedCache.empty()) {
                        source.line("            if (RS_AOT_LIKELY(" + packedCache + ".count >= 0)) {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(" + operand(1) + ") >= static_cast<std::uint64_t>(" + packedCache + ".count))) {");
                        source.line("                        return typedFail(runtime::ErrorCode::IndexOutOfRange, \"array element index is out of range\");");
                        source.line("                    }");
                        source.line("                }");
                        source.line("                " + result + " = static_cast<std::int64_t>(" + packedCache + ".data[static_cast<std::size_t>(" + operand(1) + ")]);");
                        source.line("            } else if (!context.loadElementInt32Fast(" + operand(0) + ", " + operand(1) + ", " + result +
                            ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    } else {
                        source.line("            if (!context.loadElementInt32Fast(" + operand(0) + ", " + operand(1) + ", " + result +
                            ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    }
                } else if (instruction.elementType == semantic::PrimitiveType::Double &&
                           instruction.elementTypeId == 0 &&
                           instruction.resultType == semantic::PrimitiveType::Double) {
                    const std::string packedCache = resolvePackedCacheExpr(
                        instruction.operands[0], semantic::PrimitiveType::Double);
                    if (!packedCache.empty()) {
                        source.line("            if (RS_AOT_LIKELY(" + packedCache + ".count >= 0)) {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(" + operand(1) + ") >= static_cast<std::uint64_t>(" + packedCache + ".count))) {");
                        source.line("                        return typedFail(runtime::ErrorCode::IndexOutOfRange, \"array element index is out of range\");");
                        source.line("                    }");
                        source.line("                }");
                        source.line("                " + result + " = " + packedCache + ".data[static_cast<std::size_t>(" + operand(1) + ")];");
                        source.line("            } else if (!context.loadElementDoubleFast(" + operand(0) + ", " + operand(1) + ", " + result +
                            ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    } else {
                        source.line("            if (!context.loadElementDoubleFast(" + operand(0) + ", " + operand(1) + ", " + result +
                            ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    }
                } else if (instruction.elementType == semantic::PrimitiveType::Object &&
                           instruction.resultType == semantic::PrimitiveType::Object) {
                    source.line("            if (!context.loadElementObjectFast(0x" +
                        hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                        ", " + operand(1) + ", " + result +
                        ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    emitRetainRegResult();
                } else {
                    const auto tmpElem = "typedElem_" + std::to_string(instructionIndex);
                    source.line("            runtime::Value " + tmpElem + ";");
                    source.line("            if (!context.loadElement(" +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                        ", runtime::Value{" + operand(1) + "}, " + tmpElem +
                        ") || !aot::typedUnboxScalar(" +
                        primitiveExpression(instruction.resultType) + ", " + tmpElem +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    if (instruction.resultType == semantic::PrimitiveType::Array ||
                        instruction.resultType == semantic::PrimitiveType::Object) {
                        emitRetainRegResult();
                    }
                }
                break;
            }
            case mir::Opcode::StoreElement: {
                const auto valSlot = operandSlot(2);
                if (instruction.elementType == semantic::PrimitiveType::Struct &&
                    classifyInlineMath(instruction.elementType, instruction.elementTypeId) ==
                        InlineMathKind::None) {
                    const auto arrLocalIt = valueToArrayLocal.find(instruction.operands[0]);
                    const std::string structName = "AotStruct_" + hexId(instruction.elementTypeId);
                    if (arrLocalIt != valueToArrayLocal.end()) {
                        const auto rawColExpr = "typedLocalRawCol_" + std::to_string(arrLocalIt->second);
                        source.line("            if (RS_AOT_LIKELY(" + rawColExpr + ".basePtr != nullptr)) {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    if (RS_AOT_UNLIKELY(" + rawColExpr + ".readOnly || static_cast<std::uint64_t>(" + operand(1) + ") >= static_cast<std::uint64_t>(" + rawColExpr + ".count))) {");
                        source.line("                        return typedFail(" + rawColExpr + ".readOnly ? runtime::ErrorCode::CapabilityDenied : runtime::ErrorCode::IndexOutOfRange, \"raw ECS column store failed\");");
                        source.line("                    }");
                        source.line("                }");
                        source.line("                (void)" + structName + "::storeToRawColumn(" + rawColExpr + ", " + operand(1) + ", " + operand(2) + ");");
                        source.line("            } else {");
                        source.line("                if (!context.storeElement(semantic::PrimitiveType::Struct, 0x" +
                            hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                            ", runtime::Value{" + operand(1) + "}, " + operand(2) +
                            ".boxToValue())) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                        source.line("            }");
                    } else {
                        source.line("            if (!context.storeElement(semantic::PrimitiveType::Struct, 0x" +
                            hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                            ", runtime::Value{" + operand(1) + "}, " + operand(2) +
                            ".boxToValue())) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    }
                } else if (instruction.elementType == semantic::PrimitiveType::Int &&
                           instruction.elementTypeId == 0 &&
                           valSlot.primitive == semantic::PrimitiveType::Int) {
                    const std::string packedCache = resolvePackedCacheExpr(
                        instruction.operands[0], semantic::PrimitiveType::Int);
                    if (!packedCache.empty()) {
                        source.line("            if (RS_AOT_LIKELY(" + packedCache + ".count >= 0)) {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(" + operand(1) + ") >= static_cast<std::uint64_t>(" + packedCache + ".count))) {");
                        source.line("                        return typedFail(runtime::ErrorCode::IndexOutOfRange, \"array element index is out of range\");");
                        source.line("                    }");
                        source.line("                }");
                        source.line("                " + packedCache + ".data[static_cast<std::size_t>(" + operand(1) + ")] = static_cast<std::int32_t>(" + operand(2) + ");");
                        source.line("            } else if (!context.storeElementInt32Fast(" + operand(0) + ", " + operand(1) + ", " + operand(2) +
                            ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    } else {
                        source.line("            if (!context.storeElementInt32Fast(" + operand(0) + ", " + operand(1) + ", " + operand(2) +
                            ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    }
                } else if (instruction.elementType == semantic::PrimitiveType::Double &&
                           instruction.elementTypeId == 0 &&
                           valSlot.primitive == semantic::PrimitiveType::Double) {
                    const std::string packedCache = resolvePackedCacheExpr(
                        instruction.operands[0], semantic::PrimitiveType::Double);
                    if (!packedCache.empty()) {
                        source.line("            if (RS_AOT_LIKELY(" + packedCache + ".count >= 0)) {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(" + operand(1) + ") >= static_cast<std::uint64_t>(" + packedCache + ".count))) {");
                        source.line("                        return typedFail(runtime::ErrorCode::IndexOutOfRange, \"array element index is out of range\");");
                        source.line("                    }");
                        source.line("                }");
                        source.line("                " + packedCache + ".data[static_cast<std::size_t>(" + operand(1) + ")] = " + operand(2) + ";");
                        source.line("            } else if (!context.storeElementDoubleFast(" + operand(0) + ", " + operand(1) + ", " + operand(2) +
                            ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    } else {
                        source.line("            if (!context.storeElementDoubleFast(" + operand(0) + ", " + operand(1) + ", " + operand(2) +
                            ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    }
                } else {
                    source.line("            if (!context.storeElement(" +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                        ", runtime::Value{" + operand(1) + "}, aot::typedBoxScalar(" +
                        primitiveExpression(valSlot.primitive) + ", " + operand(2) +
                        "))) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                }
                break;
            }
            case mir::Opcode::ConstantDouble: {
                std::ostringstream literal;
                literal << std::setprecision(17) << instruction.doubleImmediate;
                if (instruction.resultType == semantic::PrimitiveType::Float) {
                    source.line("            " + result + " = static_cast<float>(" +
                        literal.str() + ");");
                } else {
                    source.line("            " + result + " = " + literal.str() + ";");
                }
                break;
            }
            case mir::Opcode::ConstantBool:
                source.line("            " + result + " = " +
                    (instruction.boolImmediate ? "true" : "false") + ";");
                break;
            case mir::Opcode::LoadLocal:
                source.line("            " + result + " = " +
                    typedLocalExpression(instruction.localIndex) + ";");
                break;
            case mir::Opcode::StoreLocal: {
                const auto srcSlot = operandSlot(0);
                std::string rhsExpr = operand(0);
                if (instruction.localIndex < function.localTypes.size() &&
                    srcSlot.primitive == semantic::PrimitiveType::Null) {
                    if (function.localTypes[instruction.localIndex] == semantic::PrimitiveType::Object) {
                        rhsExpr = "runtime::Value{runtime::NullObject{}}";
                    } else if (function.localTypes[instruction.localIndex] == semantic::PrimitiveType::Array) {
                        rhsExpr = "runtime::Value{runtime::NullArray{}}";
                    }
                }
                source.line("            " +
                    typedLocalExpression(instruction.localIndex) + " = " +
                    rhsExpr + ";");
                if (instruction.localIndex < localGuardSlot.size() &&
                    localGuardSlot[instruction.localIndex] != std::numeric_limits<std::size_t>::max()) {
                    source.line("            if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
                    source.line("                aot::retainTypedGcSlot(context.rawHeap(), typedLocalGcGuards[" +
                        std::to_string(localGuardSlot[instruction.localIndex]) + "], " +
                        typedLocalExpression(instruction.localIndex) + ");");
                    source.line("            }");
                }
                if (instruction.localIndex < function.localTypes.size() &&
                    function.localTypes[instruction.localIndex] == semantic::PrimitiveType::Array) {
                    const auto elemTypeIt = arrayLocalStructElemType.find(instruction.localIndex);
                    if (elemTypeIt != arrayLocalStructElemType.end()) {
                        const std::string structName = "AotStruct_" + hexId(elemTypeIt->second);
                        source.line("            typedLocalRawCol_" +
                            std::to_string(instruction.localIndex) +
                            " = context.acquireRawColumnCache(" +
                            typedLocalExpression(instruction.localIndex) +
                            ", 0x" + hexId(elemTypeIt->second) + "ULL, sizeof(" +
                            structName + "), " + structName + "_layout, " +
                            structName + "::kFieldCount);");
                    } else {
                        source.line("            typedLocalRawCol_" +
                            std::to_string(instruction.localIndex) +
                            " = context.acquireRawColumnCache(" +
                            typedLocalExpression(instruction.localIndex) + ");");
                    }
                    const auto packedIt = arrayLocalPackedPrim.find(instruction.localIndex);
                    if (packedIt != arrayLocalPackedPrim.end()) {
                        const auto parIt = valueToArrayParam.find(instruction.operands[0]);
                        if (parIt != valueToArrayParam.end() &&
                            arrayParamPackedPrim.find(parIt->second) != arrayParamPackedPrim.end() &&
                            arrayParamPackedPrim.at(parIt->second) == packedIt->second) {
                            const std::string argCache = (packedIt->second == semantic::PrimitiveType::Int
                                ? "typedArgPackedI32_"
                                : "typedArgPackedF64_") + std::to_string(parIt->second);
                            source.line("            " +
                                packedArrayCacheName(instruction.localIndex, packedIt->second) +
                                " = " + argCache + ";");
                        } else if (packedIt->second == semantic::PrimitiveType::Int) {
                            source.line("            typedLocalPackedI32_" +
                                std::to_string(instruction.localIndex) +
                                " = context.acquirePackedInt32ArrayCache(" +
                                typedLocalExpression(instruction.localIndex) + ");");
                        } else {
                            source.line("            typedLocalPackedF64_" +
                                std::to_string(instruction.localIndex) +
                                " = context.acquirePackedDoubleArrayCache(" +
                                typedLocalExpression(instruction.localIndex) + ");");
                        }
                    }
                }
                break;
            }
            case mir::Opcode::ConvertIntToLong:
                source.line("            " + result +
                    " = static_cast<std::int64_t>(" + operand(0) + ");");
                break;
            case mir::Opcode::ConvertIntToDouble:
            case mir::Opcode::ConvertLongToDouble:
                source.line("            " + result +
                    " = static_cast<double>(" + operand(0) + ");");
                break;
            case mir::Opcode::ConvertNumeric: {
                const auto srcSlot = operandSlot(0);
                source.line("            if (!aot::typedConvertNumeric<" +
                    primitiveExpression(srcSlot.primitive) + ", " +
                    primitiveExpression(instruction.resultType) + ", " +
                    (instruction.checkedArithmetic ? "true" : "false") + ">(" +
                    operand(0) + ", " + result + ")) return typedFail("
                    "runtime::ErrorCode::IntegerOverflow, "
                    "\"numeric conversion overflow\");");
                break;
            }
            case mir::Opcode::AddInt:
            case mir::Opcode::SubtractInt:
            case mir::Opcode::MultiplyInt: {
                const auto operation = instruction.opcode == mir::Opcode::AddInt
                    ? "+"
                    : instruction.opcode == mir::Opcode::SubtractInt ? "-" : "*";
                const auto temporary = "typedArithmetic_" +
                    std::to_string(instruction.result);
                source.line("            const auto " + temporary + " = " +
                    operand(0) + " " + operation + " " + operand(1) + ";");
                if (!instruction.checkedArithmetic) {
                    source.line("            " + result +
                        " = aot::typedWrapInt32(" + temporary + ");");
                } else {
                    if (typedCheckedArithmeticProvenSafe(
                            rangeAnalysis, instruction)) {
                        source.line("            // checked arithmetic proven safe by integer range analysis");
                    } else {
                        source.line("            if constexpr (!TrustedRelease) {");
                        source.line("                if (" + temporary +
                            " < std::numeric_limits<std::int32_t>::min() || " +
                            temporary +
                            " > std::numeric_limits<std::int32_t>::max()) return "
                            "typedFail(runtime::ErrorCode::IntegerOverflow, "
                            "\"integer arithmetic overflow\");");
                        source.line("            }");
                    }
                    source.line("            " + result + " = " + temporary + ";");
                }
                break;
            }
            case mir::Opcode::DivideInt:
            case mir::Opcode::RemainderInt: {
                source.line("            if (" + operand(1) +
                    " == 0) return typedFail(runtime::ErrorCode::DivisionByZero, \"" +
                    (instruction.opcode == mir::Opcode::DivideInt
                        ? "division by zero"
                        : "remainder by zero") + "\");");
                if (instruction.opcode == mir::Opcode::DivideInt) {
                    source.line("            if (" + operand(0) +
                        " == std::numeric_limits<std::int32_t>::min() && " +
                        operand(1) +
                        " == -1) return typedFail(runtime::ErrorCode::IntegerOverflow, "
                        "\"integer division overflow\");");
                    source.line("            " + result + " = " + operand(0) +
                        " / " + operand(1) + ";");
                } else {
                    source.line("            " + result + " = (" + operand(0) +
                        " == std::numeric_limits<std::int32_t>::min() && " +
                        operand(1) + " == -1) ? std::int64_t{0} : (" +
                        operand(0) + " % " + operand(1) + ");");
                }
                break;
            }
            case mir::Opcode::NegateInt:
                if (!instruction.checkedArithmetic) {
                    source.line("            " + result + " = (" + operand(0) +
                        " == std::numeric_limits<std::int32_t>::min()) ? " +
                        operand(0) + " : -" + operand(0) + ";");
                } else {
                    if (typedCheckedArithmeticProvenSafe(
                            rangeAnalysis, instruction)) {
                        source.line("            // checked negation proven safe by integer range analysis");
                    } else {
                        source.line("            if (" + operand(0) +
                            " == std::numeric_limits<std::int32_t>::min()) return "
                            "typedFail(runtime::ErrorCode::IntegerOverflow, "
                            "\"integer negation overflow\");");
                    }
                    source.line("            " + result + " = -" + operand(0) + ";");
                }
                break;
            case mir::Opcode::AddLong:
            case mir::Opcode::SubtractLong:
            case mir::Opcode::MultiplyLong: {
                const char* helper = instruction.opcode == mir::Opcode::AddLong
                    ? "aot::typedAddLong"
                    : (instruction.opcode == mir::Opcode::SubtractLong
                        ? "aot::typedSubLong"
                        : "aot::typedMulLong");
                source.line("            if (!" + std::string(helper) + "<" +
                    (instruction.checkedArithmetic ? "true" : "false") + ">(" +
                    operand(0) + ", " + operand(1) + ", " + result +
                    ")) return typedFail(runtime::ErrorCode::IntegerOverflow, "
                    "\"long arithmetic overflow\");");
                break;
            }
            case mir::Opcode::DivideLong:
            case mir::Opcode::RemainderLong: {
                source.line("            if (" + operand(1) +
                    " == 0) return typedFail(runtime::ErrorCode::DivisionByZero, \"" +
                    (instruction.opcode == mir::Opcode::DivideLong
                        ? "division by zero"
                        : "remainder by zero") + "\");");
                if (instruction.opcode == mir::Opcode::DivideLong) {
                    source.line("            if (" + operand(0) +
                        " == std::numeric_limits<std::int64_t>::min() && " +
                        operand(1) +
                        " == -1) return typedFail(runtime::ErrorCode::IntegerOverflow, "
                        "\"long division overflow\");");
                    source.line("            " + result + " = " + operand(0) +
                        " / " + operand(1) + ";");
                } else {
                    source.line("            " + result + " = (" + operand(0) +
                        " == std::numeric_limits<std::int64_t>::min() && " +
                        operand(1) + " == -1) ? std::int64_t{0} : (" +
                        operand(0) + " % " + operand(1) + ");");
                }
                break;
            }
            case mir::Opcode::NegateLong:
                if (!instruction.checkedArithmetic) {
                    source.line("            " + result + " = (" + operand(0) +
                        " == std::numeric_limits<std::int64_t>::min()) ? " +
                        operand(0) + " : -" + operand(0) + ";");
                } else {
                    source.line("            if (" + operand(0) +
                        " == std::numeric_limits<std::int64_t>::min()) return "
                        "typedFail(runtime::ErrorCode::IntegerOverflow, "
                        "\"long negation overflow\");");
                    source.line("            " + result + " = -" + operand(0) + ";");
                }
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
                source.line("            " + result + " = " + operand(0) +
                    operation + operand(1) + ";");
                break;
            }
            case mir::Opcode::NegateDouble:
                source.line("            " + result + " = -" + operand(0) + ";");
                break;
            case mir::Opcode::LogicalNot:
                source.line("            " + result + " = !" + operand(0) + ";");
                break;
            case mir::Opcode::Equal:
                source.line("            " + result + " = (" + operand(0) +
                    " == " + operand(1) + ");");
                break;
            case mir::Opcode::NotEqual:
                source.line("            " + result + " = !(" + operand(0) +
                    " == " + operand(1) + ");");
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
                source.line("            " + result + " = " + operand(0) +
                    " " + operation + " " + operand(1) + ";");
                break;
            }
            case mir::Opcode::NewStruct: {
                const auto typeId = instruction.resultTypeId != 0
                    ? instruction.resultTypeId
                    : instruction.typeId;
                const auto kind = classifyInlineMath(
                    semantic::PrimitiveType::Struct, typeId);
                if (kind != InlineMathKind::None) {
                    source.line("            " + result + " = " +
                        typedDefaultValue({semantic::PrimitiveType::Struct, typeId}) + ";");
                } else {
                    source.line("            if (!context.initUnboxedStruct(0x" +
                        hexId(typeId) + "ULL, " + result + ")) return false;");
                }
                break;
            }
            case mir::Opcode::LoadStructField: {
                const auto kind = classifyInlineMath(
                    semantic::PrimitiveType::Struct, instruction.typeId);
                if (kind != InlineMathKind::None) {
                    const char* member = inlineMathFieldMember(
                        kind, instruction.fieldIndex);
                    if (kind == InlineMathKind::Fixed ||
                        kind == InlineMathKind::Fixed16 ||
                        kind == InlineMathKind::Fixed64) {
                        source.line("            " + result +
                            " = static_cast<std::int64_t>(" + operand(0) + "." +
                            member + ");");
                    } else if (kind == InlineMathKind::Vec2f ||
                               kind == InlineMathKind::Vec3f ||
                               kind == InlineMathKind::Quatf) {
                        source.line("            " + result +
                            " = " + operand(0) + "." + member + ";");
                    } else {
                        source.line("            " + result +
                            " = runtime::FixedValue{" + operand(0) + "." +
                            member + "};");
                    }
                } else {
                    source.line("            " + result + " = " + operand(0) +
                        ".f_" + std::to_string(instruction.fieldIndex) + ";");
                }
                break;
            }
            case mir::Opcode::StoreStructField: {
                const auto kind = classifyInlineMath(
                    semantic::PrimitiveType::Struct, instruction.typeId);
                if (kind != InlineMathKind::None) {
                    const char* member = inlineMathFieldMember(
                        kind, instruction.fieldIndex);
                    source.line("            " + result + " = " + operand(0) + ";");
                    if (kind == InlineMathKind::Fixed) {
                        source.line("            " + result + "." + member +
                            " = static_cast<std::int32_t>(" + operand(1) + ");");
                    } else if (kind == InlineMathKind::Fixed16 ||
                               kind == InlineMathKind::Fixed64) {
                        source.line("            " + result + "." + member +
                            " = static_cast<std::int64_t>(" + operand(1) + ");");
                    } else if (kind == InlineMathKind::Vec2f ||
                               kind == InlineMathKind::Vec3f ||
                               kind == InlineMathKind::Quatf) {
                        source.line("            " + result + "." + member +
                            " = " + operand(1) + ";");
                    } else {
                        source.line("            " + result + "." + member +
                            " = " + operand(1) + ".raw;");
                    }
                } else {
                    if (result != operand(0)) {
                        source.line("            " + result + " = " + operand(0) + ";");
                    }
                    source.line("            " + result + ".f_" +
                        std::to_string(instruction.fieldIndex) + " = " + operand(1) + ";");
                }
                break;
            }
            case mir::Opcode::Call: {
                const auto callIdx = typedCallSiteCounter++;
                const auto suffix = std::to_string(callIdx);
                source.line("            if constexpr (!TrustedRelease) {");
                source.line("                rawAccounting.suspendBeforeCall();");
                source.line("                determinismAccounting.suspendBeforeCall();");
                source.line("            }");
                const bool isPolyCall = instruction.virtualDispatch || instruction.interfaceDispatch;
                const auto targetIt = !isPolyCall
                    ? typedFunctions.find(instruction.symbolId)
                    : typedFunctions.end();
                if (isPolyCall && !instruction.operands.empty()) {
                    const auto callId = callIds.at(&instruction);
                    std::vector<std::pair<semantic::SymbolId, const FunctionView*>> polyTargets;
                    const semantic::SymbolId baseRecvTid =
                        !instruction.parameterTypeIds.empty() ? instruction.parameterTypeIds[0] : 0;
                    for (const auto& [candTid, tView] : allTypes) {
                        if (!tView.type ||
                            tView.type->kind != semantic::TypeKind::Class ||
                            tView.type->abstractType ||
                            tView.type->interfaceType) {
                            continue;
                        }
                        semantic::SymbolId resolvedSym = 0;
                        if (instruction.virtualDispatch) {
                            if (baseRecvTid != 0 && !aotTypeAssignable(candTid, baseRecvTid, allTypes)) {
                                continue;
                            }
                            if (instruction.virtualSlot < tView.type->virtualDispatchTable.size()) {
                                resolvedSym = tView.type->virtualDispatchTable[instruction.virtualSlot];
                            }
                        } else if (instruction.interfaceDispatch) {
                            for (const auto& imap : tView.type->interfaceDispatchMaps) {
                                if (imap.interfaceTypeId == instruction.interfaceTypeId) {
                                    if (instruction.interfaceSlot < imap.slots.size()) {
                                        resolvedSym = imap.slots[instruction.interfaceSlot];
                                    }
                                    break;
                                }
                            }
                        }
                        if (resolvedSym == 0) continue;
                        const auto tfIt = typedFunctions.find(resolvedSym);
                        if (tfIt == typedFunctions.end()) continue;
                        const auto* candFn = tfIt->second->function;
                        if (candFn->parameterTypes.size() != instruction.operands.size() ||
                            candFn->returnType != instruction.resultType) {
                            continue;
                        }
                        bool sigCompatible = true;
                        for (std::size_t aIdx = 0; aIdx < instruction.operands.size(); ++aIdx) {
                            const auto cTid = aIdx < candFn->parameterTypeIds.size()
                                ? candFn->parameterTypeIds[aIdx]
                                : 0;
                            const auto opSlot = operandSlot(aIdx);
                            if (aIdx >= instruction.parameterTypes.size() ||
                                candFn->parameterTypes[aIdx] != instruction.parameterTypes[aIdx]) {
                                sigCompatible = false;
                                break;
                            }
                            if (candFn->parameterTypes[aIdx] == semantic::PrimitiveType::Object) {
                                if (opSlot.primitive != semantic::PrimitiveType::Null &&
                                    (opSlot.primitive != semantic::PrimitiveType::Object ||
                                     (aIdx > 0 && cTid != 0 &&
                                      !aotTypeAssignable(opSlot.typeId, cTid, allTypes)))) {
                                    sigCompatible = false;
                                    break;
                                }
                            } else if (typedCppType({candFn->parameterTypes[aIdx], cTid}) !=
                                       typedCppType(opSlot)) {
                                sigCompatible = false;
                                break;
                            }
                        }
                        if (instruction.resultType != semantic::PrimitiveType::Void &&
                            typedCppType({candFn->returnType, candFn->returnTypeId}) !=
                                typedCppType({instruction.resultType, instruction.resultTypeId})) {
                            sigCompatible = false;
                        }
                        if (sigCompatible) {
                            polyTargets.emplace_back(candTid, tfIt->second);
                        }
                    }

                    source.line("            if (RS_AOT_UNLIKELY(std::holds_alternative<runtime::NullObject>(" + callOperandExpr(0) + "))) {");
                    source.line("                if constexpr (!TrustedRelease) {");
                    source.line("                    rawAccounting.resumeAfterCall();");
                    source.line("                    determinismAccounting.resumeAfterCall();");
                    source.line("                }");
                    source.line("                return typedFail(runtime::ErrorCode::NullReference, \"" +
                        std::string(instruction.virtualDispatch
                            ? "AOT virtual call attempted to dereference null"
                            : "AOT interface call attempted to dereference null") + "\");");
                    source.line("            }");
                    source.line("            const semantic::SymbolId polyActualTid_" + suffix +
                        " = context.objectTypeIdFast(" + callOperandExpr(0) + ");");
                    source.line("            bool polyDispatched_" + suffix + " = false;");
                    source.line("            bool polyCallOk_" + suffix + " = false;");
                    if (instruction.resultType != semantic::PrimitiveType::Void) {
                        const TypedSlotType retSlot{instruction.resultType, instruction.resultTypeId};
                        source.line("            " + typedCppType(retSlot) + " polyCallRes_" + suffix + "{};");
                    }
                    if (!polyTargets.empty()) {
                        source.line("            switch (polyActualTid_" + suffix + ") {");
                        for (const auto& [candTid, calleeView] : polyTargets) {
                            source.line("            case 0x" + hexId(candTid) + "ULL: {");
                            source.line("                polyDispatched_" + suffix + " = true;");
                            source.line("                semantic::SymbolId prevFn_" + suffix + " = 0;");
                            source.line("                (void)prevFn_" + suffix + ";");
                            source.line("                if (!context.enterTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(0x" +
                                hexId(calleeView->function->symbolId) + "ULL, \"" +
                                escapeCppString(calleeView->qualifiedName) + "\", prevFn_" + suffix + ", typedActiveGenLine)) {");
                            source.line("                    if constexpr (!TrustedRelease) {");
                            source.line("                        rawAccounting.resumeAfterCall();");
                            source.line("                        determinismAccounting.resumeAfterCall();");
                            source.line("                    }");
                            source.line("                    return false;");
                            source.line("                }");
                            std::string callArgs = "context";
                            for (std::size_t argIndex = 0; argIndex < instruction.operands.size(); ++argIndex) {
                                callArgs += ", " + callOperandExpr(argIndex);
                            }
                            if (instruction.resultType != semantic::PrimitiveType::Void) {
                                callArgs += ", polyCallRes_" + suffix;
                            }
                            source.line("                polyCallOk_" + suffix + " = " + calleeView->cppName +
                                "_scalar<FastAccounting, DeterminismAccounting, TrustedRelease>(" + callArgs + ");");
                            if (instruction.resultType != semantic::PrimitiveType::Void) {
                                source.line("                context.exitTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(prevFn_" +
                                    suffix + ", polyCallOk_" + suffix +
                                    ", [&]() { return aot::typedBoxScalar(" +
                                    primitiveExpression(instruction.resultType) +
                                    ", polyCallRes_" + suffix + "); });");
                            } else {
                                source.line("                context.exitTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(prevFn_" +
                                    suffix + ", polyCallOk_" + suffix +
                                    ", []() { return runtime::Value{std::monostate{}}; });");
                            }
                            source.line("                break;");
                            source.line("            }");
                        }
                        source.line("            default: break;");
                        source.line("            }");
                    }
                    source.line("            if (RS_AOT_UNLIKELY(!polyDispatched_" + suffix + ")) {");
                    std::string argPtr = "nullptr";
                    if (!instruction.operands.empty()) {
                        source.line("                std::array<runtime::Value, " +
                            std::to_string(instruction.operands.size()) +
                            "> bridgeArgs_" + suffix + "{{");
                        for (std::size_t argIndex = 0; argIndex < instruction.operands.size(); ++argIndex) {
                            const auto argSlot = operandSlot(argIndex);
                            const auto boxPrim = (argIndex < instruction.parameterTypes.size() &&
                                                  argSlot.primitive == semantic::PrimitiveType::Null)
                                ? instruction.parameterTypes[argIndex]
                                : argSlot.primitive;
                            source.line("                    aot::typedBoxScalar(" +
                                primitiveExpression(boxPrim) + ", " +
                                callOperandExpr(argIndex) + "),");
                        }
                        source.line("                }};");
                        argPtr = "bridgeArgs_" + suffix + ".data()";
                    }
                    source.line("                context.setActiveGeneratedLine(typedActiveGenLine);");
                    source.line("                runtime::Value bridgeRes_" + suffix + ";");
                    source.line("                polyCallOk_" + suffix +
                        " = context.call(call_" + std::to_string(callId) +
                        "_signature, " + argPtr + ", " +
                        std::to_string(instruction.operands.size()) +
                        "u, bridgeRes_" + suffix + ");");
                    if (instruction.resultType != semantic::PrimitiveType::Void) {
                        source.line("                if (polyCallOk_" + suffix +
                            " && !aot::typedUnboxScalar(" +
                            primitiveExpression(instruction.resultType) +
                            ", bridgeRes_" + suffix + ", polyCallRes_" + suffix + ")) {");
                        source.line("                    if constexpr (!TrustedRelease) {");
                        source.line("                        rawAccounting.resumeAfterCall();");
                        source.line("                        determinismAccounting.resumeAfterCall();");
                        source.line("                    }");
                        source.line("                    return typedFail(runtime::ErrorCode::TypeMismatch, \"AOT call return type mismatch\");");
                        source.line("                }");
                    }
                    source.line("            }");
                    source.line("            if constexpr (!TrustedRelease) {");
                    source.line("                rawAccounting.resumeAfterCall();");
                    source.line("                determinismAccounting.resumeAfterCall();");
                    source.line("            }");
                    emitScalarCallFailureReturn("            ", "polyCallOk_" + suffix);
                    if (instruction.result >= 0 &&
                        instruction.resultType != semantic::PrimitiveType::Void) {
                        source.line("            " + result + " = polyCallRes_" + suffix + ";");
                        if (instruction.resultType == semantic::PrimitiveType::Array ||
                            instruction.resultType == semantic::PrimitiveType::Object) {
                            emitRetainRegResult();
                        }
                    }
                } else if (targetIt != typedFunctions.end()) {
                    const auto* calleeView = targetIt->second;
                    source.line("            semantic::SymbolId prevFn_" + suffix + " = 0;");
                    source.line("            (void)prevFn_" + suffix + ";");
                    source.line("            if (!context.enterTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(0x" +
                        hexId(calleeView->function->symbolId) + "ULL, \"" +
                        escapeCppString(calleeView->qualifiedName) + "\", prevFn_" + suffix + ", typedActiveGenLine)) {");
                    source.line("                if constexpr (!TrustedRelease) {");
                    source.line("                    rawAccounting.resumeAfterCall();");
                    source.line("                    determinismAccounting.resumeAfterCall();");
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                    std::string callArgs = "context";
                    for (std::size_t argIndex = 0; argIndex < instruction.operands.size(); ++argIndex) {
                        callArgs += ", " + callOperandExpr(argIndex);
                    }
                    if (instruction.resultType != semantic::PrimitiveType::Void) {
                        const TypedSlotType retSlot{
                            instruction.resultType,
                            instruction.resultTypeId,
                        };
                        source.line("            " + typedCppType(retSlot) +
                            " directCallRes_" + suffix + "{};");
                        callArgs += ", directCallRes_" + suffix;
                    }
                    source.line("            const bool directCallOk_" + suffix +
                        " = " + calleeView->cppName +
                        "_scalar<FastAccounting, DeterminismAccounting, TrustedRelease>(" +
                        callArgs + ");");
                    if (instruction.resultType != semantic::PrimitiveType::Void) {
                        source.line("            context.exitTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(prevFn_" +
                            suffix + ", directCallOk_" + suffix +
                            ", [&]() { return aot::typedBoxScalar(" +
                            primitiveExpression(instruction.resultType) +
                            ", directCallRes_" + suffix + "); });");
                    } else {
                        source.line("            context.exitTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(prevFn_" +
                            suffix + ", directCallOk_" + suffix +
                            ", []() { return runtime::Value{std::monostate{}}; });");
                    }
                    source.line("            if constexpr (!TrustedRelease) {");
                    source.line("                rawAccounting.resumeAfterCall();");
                    source.line("                determinismAccounting.resumeAfterCall();");
                    source.line("            }");
                    emitScalarCallFailureReturn("            ", "directCallOk_" + suffix);
                    if (instruction.result >= 0 &&
                        instruction.resultType != semantic::PrimitiveType::Void) {
                        source.line("            " + result + " = directCallRes_" + suffix + ";");
                        if (instruction.resultType == semantic::PrimitiveType::Array ||
                            instruction.resultType == semantic::PrimitiveType::Object) {
                            emitRetainRegResult();
                        }
                    }
                } else {
                    const auto callId = callIds.at(&instruction);
                    const std::string_view symName(instruction.symbolName);
                    if (symName.rfind("$intrinsic::ecs_writeback::", 0) == 0 &&
                        instruction.operands.size() == 2 &&
                        operandSlot(1).primitive == semantic::PrimitiveType::Struct &&
                        classifyInlineMath(operandSlot(1).primitive, operandSlot(1).typeId) ==
                            InlineMathKind::None) {
                        source.line("            if (RS_AOT_UNLIKELY(context.hasSparseEcsState())) {");
                        source.line("                const bool wbOk_" + suffix +
                            " = context.ecsWritebackBoxed(call_" +
                            std::to_string(callId) + "_signature, " +
                            operand(0) + ", " + operand(1) + ".boxToValue());");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    rawAccounting.resumeAfterCall();");
                        source.line("                    determinismAccounting.resumeAfterCall();");
                        source.line("                }");
                        source.line("                if (!wbOk_" + suffix + ") return false;");
                        source.line("            } else {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    rawAccounting.resumeAfterCall();");
                        source.line("                    determinismAccounting.resumeAfterCall();");
                        source.line("                }");
                        source.line("            }");
                    } else if (symName.rfind("$intrinsic::static_get::ecs_col::", 0) == 0 &&
                               instruction.operands.empty() &&
                               instruction.result >= 0) {
                        semantic::SymbolId denseColSymbolId = 0;
                        const auto rest = symName.substr(std::string_view("$intrinsic::static_get::ecs_col::").size());
                        const auto sep = rest.find("::");
                        if (sep != std::string_view::npos) {
                            const auto colIdx = static_cast<std::size_t>(
                                std::strtoull(std::string(rest.substr(0, sep)).c_str(), nullptr, 10));
                            const auto spec = rest.substr(sep + 2);
                            const auto firstSemi = spec.find(';');
                            const auto restAfterCols = firstSemi == std::string_view::npos
                                ? std::string_view{}
                                : spec.substr(firstSemi);
                            if (restAfterCols == ";;" || restAfterCols.empty()) {
                                const auto colsPart = firstSemi == std::string_view::npos
                                    ? spec
                                    : spec.substr(0, firstSemi);
                                std::size_t start = 0;
                                std::size_t cur = 0;
                                while (start <= colsPart.size()) {
                                    const auto comma = colsPart.find_first_of(",|", start);
                                    const auto tok = comma == std::string_view::npos
                                        ? colsPart.substr(start)
                                        : colsPart.substr(start, comma - start);
                                    if (cur == colIdx && !tok.empty()) {
                                        denseColSymbolId = semantic::stableTypeId("$ecs_column::" + std::string(tok));
                                        break;
                                    }
                                    if (comma == std::string_view::npos) break;
                                    start = comma + 1;
                                    ++cur;
                                }
                            }
                        }
                        source.line("            const bool colOk_" + suffix +
                            " = context.resolveEcsColumnFast(0x" +
                            hexId(denseColSymbolId) + "ULL, call_" +
                            std::to_string(callId) + "_signature, " + result + ");");
                        source.line("            if constexpr (!TrustedRelease) {");
                        source.line("                rawAccounting.resumeAfterCall();");
                        source.line("                determinismAccounting.resumeAfterCall();");
                        source.line("            }");
                        source.line("            if (!colOk_" + suffix + ") return false;");
                        if (!options.trustedRelease) {
                            emitRetainRegResult();
                        }
                    } else {
                        std::string fastAsyncExpr;
                        if (symName.rfind("$intrinsic::Async.__", 0) == 0 && instruction.result >= 0) {
                            const auto asyncOp = symName.substr(std::string_view("$intrinsic::Async.__").size());
                            if (asyncOp == "CompletedVoid" && instruction.operands.empty()) {
                                fastAsyncExpr = "(" + result + " = static_cast<std::int64_t>(runtime::AwaitableHandle::completedVoid().pack64()), true)";
                            } else if (asyncOp == "TokenPreCanceled" && instruction.operands.empty()) {
                                fastAsyncExpr = "(" + result + " = static_cast<std::int64_t>(runtime::CancellationToken::preCanceled().pack64()), true)";
                            } else if (asyncOp == "FromCanceled" && instruction.operands.empty()) {
                                fastAsyncExpr = "context.asyncIntrinsicFromCanceledFast(" + result + ")";
                            } else if (asyncOp == "Yield" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicYieldFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "NewPending" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicNewPendingFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "CompleteVoid" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicCompleteVoidFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "CompleteInt" && instruction.operands.size() == 2) {
                                fastAsyncExpr = "context.asyncIntrinsicCompleteIntFast(" + operand(0) + ", " + operand(1) + ", " + result + ")";
                            } else if (asyncOp == "CompleteCancel" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicCompleteCancelFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "IsCompleted" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicIsCompletedFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "CtsCreate" && instruction.operands.empty()) {
                                fastAsyncExpr = "context.asyncIntrinsicCtsCreateFast(" + result + ")";
                            } else if (asyncOp == "CtsCreateLinked" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicCtsCreateLinkedFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "CtsCreateLinked2" && instruction.operands.size() == 2) {
                                fastAsyncExpr = "context.asyncIntrinsicCtsCreateLinked2Fast(" + operand(0) + ", " + operand(1) + ", " + result + ")";
                            } else if (asyncOp == "CtsCreateTimeout" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicCtsCreateTimeoutFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "CtsCreateLinkedTimeout" && instruction.operands.size() == 2) {
                                fastAsyncExpr = "context.asyncIntrinsicCtsCreateLinkedTimeoutFast(" + operand(0) + ", " + operand(1) + ", " + result + ")";
                            } else if (asyncOp == "CtsToken" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicCtsTokenFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "CtsCancel" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicCtsCancelFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "CtsRelease" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicCtsReleaseFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "TokenIsCanceled" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicTokenIsCanceledFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "TokenIsTimedOut" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicTokenIsTimedOutFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "AdvanceTimeouts" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicAdvanceTimeoutsFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "StepUpdate" && instruction.operands.size() == 1) {
                                fastAsyncExpr = "context.asyncIntrinsicStepUpdateFast(" + operand(0) + ", " + result + ")";
                            } else if (asyncOp == "ReadyUpdateCount" && instruction.operands.empty()) {
                                fastAsyncExpr = "context.asyncIntrinsicReadyUpdateCountFast(" + result + ")";
                            }
                        }
                        if (!fastAsyncExpr.empty()) {
                            source.line("            if constexpr (TrustedRelease) {");
                            source.line("                const bool fastIntrOk_" + suffix + " = " + fastAsyncExpr + ";");
                            emitScalarCallFailureReturn("                ", "fastIntrOk_" + suffix);
                            source.line("            } else {");
                        }
                        const std::string brIndent = fastAsyncExpr.empty() ? "            " : "                ";
                        std::string argPtr = "nullptr";
                        if (!instruction.operands.empty()) {
                            source.line(brIndent + "std::array<runtime::Value, " +
                                std::to_string(instruction.operands.size()) +
                                "> bridgeArgs_" + suffix + "{{");
                            for (std::size_t argIndex = 0; argIndex < instruction.operands.size(); ++argIndex) {
                                const auto argSlot = operandSlot(argIndex);
                                const auto boxPrim = (argIndex < instruction.parameterTypes.size() &&
                                                      argSlot.primitive == semantic::PrimitiveType::Null)
                                    ? instruction.parameterTypes[argIndex]
                                    : argSlot.primitive;
                                source.line(brIndent + "    aot::typedBoxScalar(" +
                                    primitiveExpression(boxPrim) + ", " +
                                    callOperandExpr(argIndex) + "),");
                            }
                            source.line(brIndent + "}};");
                            argPtr = "bridgeArgs_" + suffix + ".data()";
                        }
                        source.line(brIndent + "context.setActiveGeneratedLine(typedActiveGenLine);");
                        source.line(brIndent + "runtime::Value bridgeRes_" + suffix + ";");
                        source.line(brIndent + "const bool bridgeOk_" + suffix +
                            " = context.call(call_" + std::to_string(callId) +
                            "_signature, " + argPtr + ", " +
                            std::to_string(instruction.operands.size()) +
                            "u, bridgeRes_" + suffix + ");");
                        source.line(brIndent + "if constexpr (!TrustedRelease) {");
                        source.line(brIndent + "    rawAccounting.resumeAfterCall();");
                        source.line(brIndent + "    determinismAccounting.resumeAfterCall();");
                        source.line(brIndent + "}");
                        emitScalarCallFailureReturn(brIndent, "bridgeOk_" + suffix);
                        if (instruction.result >= 0 &&
                            instruction.resultType != semantic::PrimitiveType::Void) {
                            source.line(brIndent + "if (!aot::typedUnboxScalar(" +
                                primitiveExpression(instruction.resultType) +
                                ", bridgeRes_" + suffix + ", " + result +
                                ")) return typedFail(runtime::ErrorCode::TypeMismatch, "
                                "\"AOT call return type mismatch\");");
                            if (instruction.resultType == semantic::PrimitiveType::Array ||
                                instruction.resultType == semantic::PrimitiveType::Object) {
                                emitRetainRegResult(brIndent);
                            }
                        }
                        if (!fastAsyncExpr.empty()) {
                            source.line("            }");
                        }
                    }
                }
                break;
            }
            case mir::Opcode::NewDelegate: {
                const auto callId = callIds.at(&instruction);
                if (instruction.operands.empty()) {
                    source.line("            if (!context.newDelegate(0x" +
                        hexId(instruction.typeId) + "ULL, call_" +
                        std::to_string(callId) + "_signature, nullptr, 0u, " +
                        result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                } else if (operandSlot(0).primitive == semantic::PrimitiveType::Null) {
                    const auto nullTmp = "delNullTarget_" + std::to_string(instructionIndex);
                    source.line("            const runtime::Value " + nullTmp + "{runtime::NullObject{}};");
                    source.line("            if (!context.newDelegate(0x" +
                        hexId(instruction.typeId) + "ULL, call_" +
                        std::to_string(callId) + "_signature, &" + nullTmp +
                        ", 1u, " + result +
                        ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                } else {
                    source.line("            if (!context.newDelegate(0x" +
                        hexId(instruction.typeId) + "ULL, call_" +
                        std::to_string(callId) + "_signature, &" + operand(0) +
                        ", 1u, " + result +
                        ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                }
                emitRetainRegResult();
                break;
            }
            case mir::Opcode::CombineDelegate:
            case mir::Opcode::RemoveDelegate: {
                const auto leftExpr = (operandSlot(0).primitive == semantic::PrimitiveType::Null)
                    ? std::string("runtime::Value{runtime::NullObject{}}")
                    : operand(0);
                const auto rightExpr = (operandSlot(1).primitive == semantic::PrimitiveType::Null)
                    ? std::string("runtime::Value{runtime::NullObject{}}")
                    : operand(1);
                source.line("            if (!context.combineDelegates(0x" +
                    hexId(instruction.typeId) + "ULL, " + leftExpr + ", " +
                    rightExpr + ", " +
                    (instruction.opcode == mir::Opcode::RemoveDelegate ? "true" : "false") +
                    ", " + result +
                    ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                emitRetainRegResult();
                break;
            }
            case mir::Opcode::InvokeDelegate: {
                const auto callIdx = typedCallSiteCounter++;
                const auto suffix = std::to_string(callIdx);
                const std::size_t userArgCount = instruction.operands.size() - 1u;

                const semantic::FunctionSymbol* invokeSym = nullptr;
                if (const auto dtIt = allTypes.find(instruction.typeId);
                    dtIt != allTypes.end() && dtIt->second.type && dtIt->second.type->delegateType) {
                    for (const auto& m : dtIt->second.type->methods) {
                        if (m.name == "Invoke") {
                            invokeSym = &m;
                            break;
                        }
                    }
                }

                const auto delegateArgSlot = [&](std::size_t k) -> TypedSlotType {
                    const auto opSlot = operandSlot(k + 1u);
                    if (opSlot.primitive != semantic::PrimitiveType::Null) {
                        return opSlot;
                    }
                    if (invokeSym && (k + 1u) < invokeSym->parameters.size()) {
                        const auto& param = invokeSym->parameters[k + 1u];
                        const auto pt = semantic::storageTypeOf(param);
                        const auto ptId = exactTypeId(pt, semantic::storageTypeNameOf(param));
                        return TypedSlotType{pt, ptId};
                    }
                    return opSlot;
                };

                const auto matchesCandidateParam = [&](semantic::PrimitiveType cPt,
                                                       semantic::SymbolId cTid,
                                                       std::size_t k) -> bool {
                    const auto opSlot = operandSlot(k + 1u);
                    const auto expSlot = delegateArgSlot(k);
                    if (cPt != expSlot.primitive) return false;
                    if (cPt == semantic::PrimitiveType::Object) {
                        if (opSlot.primitive == semantic::PrimitiveType::Null) {
                            return cTid == 0 || expSlot.typeId == 0 ||
                                   aotTypeAssignable(expSlot.typeId, cTid, allTypes);
                        }
                        return cTid == 0 || aotTypeAssignable(opSlot.typeId, cTid, allTypes);
                    }
                    if (cPt == semantic::PrimitiveType::Array) {
                        if (opSlot.primitive == semantic::PrimitiveType::Null) {
                            return cTid == 0 || expSlot.typeId == cTid;
                        }
                        return cTid == 0 || opSlot.typeId == cTid;
                    }
                    return opSlot.primitive == cPt && opSlot.typeId == cTid;
                };

                struct DelegateScalarCandidate {
                    semantic::SymbolId symbolId = 0;
                    const FunctionView* view = nullptr;
                    bool expectsReceiver = false;
                    semantic::SymbolId recvTypeId = 0;
                };
                std::vector<DelegateScalarCandidate> delCandidates;
                for (const auto& [candSym, candView] : typedFunctions) {
                    const auto* candFn = candView->function;
                    if (candFn->returnType != instruction.resultType) continue;
                    if (instruction.resultType != semantic::PrimitiveType::Void) {
                        if (instruction.resultType == semantic::PrimitiveType::Object) {
                            if (instruction.resultTypeId != 0 &&
                                !aotTypeAssignable(candFn->returnTypeId, instruction.resultTypeId, allTypes)) {
                                continue;
                            }
                        } else if (candFn->returnTypeId != instruction.resultTypeId) {
                            continue;
                        }
                        if (typedCppType({candFn->returnType, candFn->returnTypeId}) !=
                            typedCppType({instruction.resultType, instruction.resultTypeId})) {
                            continue;
                        }
                    }
                    if (candFn->parameterTypes.size() == userArgCount) {
                        bool match = true;
                        for (std::size_t k = 0; k < userArgCount; ++k) {
                            const auto cTid = k < candFn->parameterTypeIds.size() ? candFn->parameterTypeIds[k] : 0;
                            if (!matchesCandidateParam(candFn->parameterTypes[k], cTid, k)) {
                                match = false;
                                break;
                            }
                        }
                        if (match) {
                            delCandidates.push_back({candSym, candView, false, 0});
                        }
                    } else if (candFn->parameterTypes.size() == userArgCount + 1u &&
                               candFn->parameterTypes[0] == semantic::PrimitiveType::Object) {
                        bool match = true;
                        for (std::size_t k = 0; k < userArgCount; ++k) {
                            const auto cTid = (k + 1u) < candFn->parameterTypeIds.size()
                                ? candFn->parameterTypeIds[k + 1u]
                                : 0;
                            if (!matchesCandidateParam(candFn->parameterTypes[k + 1u], cTid, k)) {
                                match = false;
                                break;
                            }
                        }
                        if (match) {
                            const auto recvTid = !candFn->parameterTypeIds.empty()
                                ? candFn->parameterTypeIds[0]
                                : 0;
                            delCandidates.push_back({candSym, candView, true, recvTid});
                        }
                    }
                }
                std::sort(
                    delCandidates.begin(),
                    delCandidates.end(),
                    [](const DelegateScalarCandidate& a, const DelegateScalarCandidate& b) {
                        return a.symbolId < b.symbolId;
                    });

                source.line("            if constexpr (!TrustedRelease) {");
                source.line("                rawAccounting.suspendBeforeCall();");
                source.line("                determinismAccounting.suspendBeforeCall();");
                source.line("                context.setActiveGeneratedLine(typedActiveGenLine);");
                source.line("            }");
                if (instruction.resultType != semantic::PrimitiveType::Void) {
                    const TypedSlotType retSlot{instruction.resultType, instruction.resultTypeId};
                    source.line("            " + typedCppType(retSlot) + " delCallRes_" + suffix + "{};");
                }
                source.line("            auto invokeDelLeaf_" + suffix + " = [&](const aot::ResolvedAotDelegateLeaf& leaf) -> bool {");
                if (!delCandidates.empty()) {
                    source.line("                switch (leaf.targetSymbolId) {");
                    for (const auto& cand : delCandidates) {
                        source.line("                case 0x" + hexId(cand.symbolId) + "ULL: {");
                        std::string recvGuard = cand.expectsReceiver ? "leaf.hasReceiver" : "!leaf.hasReceiver";
                        if (cand.expectsReceiver && cand.recvTypeId != 0) {
                            const auto rHex = "0x" + hexId(cand.recvTypeId) + "ULL";
                            recvGuard += " && (leaf.receiver.rawTag() == runtime::ValueTag::NullObject || "
                                "(leaf.receiver.rawTag() == runtime::ValueTag::ObjectRef && context.rawHeap() != nullptr && "
                                "(context.rawHeap()->objectTypeIdFast(leaf.receiver.asObjectRefUnchecked()) == " + rHex +
                                " || context.isAssignable(context.rawHeap()->objectTypeIdFast(leaf.receiver.asObjectRefUnchecked()), " + rHex + "))))";
                        }
                        source.line("                    if (RS_AOT_LIKELY(" + recvGuard + ")) {");
                        source.line("                        semantic::SymbolId prevFn_" + suffix + " = 0;");
                        source.line("                        (void)prevFn_" + suffix + ";");
                        source.line("                        if (!context.enterTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(0x" +
                            hexId(cand.symbolId) + "ULL, \"" +
                            escapeCppString(cand.view->qualifiedName) + "\", prevFn_" + suffix + ", typedActiveGenLine)) {");
                        source.line("                            return false;");
                        source.line("                        }");
                        std::string callArgs = "context";
                        if (cand.expectsReceiver) {
                            callArgs += ", leaf.receiver";
                        }
                        const std::size_t bias = cand.expectsReceiver ? 1u : 0u;
                        for (std::size_t k = 0; k < userArgCount; ++k) {
                            const auto opSlot = operandSlot(k + 1u);
                            const auto cPt = cand.view->function->parameterTypes[k + bias];
                            if (opSlot.primitive == semantic::PrimitiveType::Null &&
                                cPt == semantic::PrimitiveType::Object) {
                                callArgs += ", runtime::Value{runtime::NullObject{}}";
                            } else if (opSlot.primitive == semantic::PrimitiveType::Null &&
                                       cPt == semantic::PrimitiveType::Array) {
                                callArgs += ", runtime::Value{runtime::NullArray{}}";
                            } else {
                                callArgs += ", " + operand(k + 1u);
                            }
                        }
                        if (instruction.resultType != semantic::PrimitiveType::Void) {
                            callArgs += ", delCallRes_" + suffix;
                        }
                        source.line("                        const bool leafOk = " + cand.view->cppName +
                            "_scalar<FastAccounting, DeterminismAccounting, TrustedRelease>(" + callArgs + ");");
                        if (instruction.resultType != semantic::PrimitiveType::Void) {
                            source.line("                        context.exitTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(prevFn_" +
                                suffix + ", leafOk, [&]() { return aot::typedBoxScalar(" +
                                primitiveExpression(instruction.resultType) + ", delCallRes_" + suffix + "); });");
                        } else {
                            source.line("                        context.exitTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(prevFn_" +
                                suffix + ", leafOk, []() { return runtime::Value{std::monostate{}}; });");
                        }
                        source.line("                        return leafOk;");
                        source.line("                    }");
                        source.line("                    break;");
                        source.line("                }");
                    }
                    source.line("                default: break;");
                    source.line("                }");
                }
                source.line("                if (RS_AOT_UNLIKELY(!context.hasFunction(leaf.targetSymbolId))) {");
                source.line("                    return context.fail(runtime::ErrorCode::InvalidProgram, \"AOT delegate target function is unavailable\", typedActiveGenLine);");
                source.line("                }");
                source.line("                std::array<runtime::Value, " + std::to_string(userArgCount + 1u) + "> boxedDelArgs{};");
                source.line("                std::size_t boxedCount = 0;");
                source.line("                if (leaf.hasReceiver) boxedDelArgs[boxedCount++] = leaf.receiver;");
                for (std::size_t k = 0; k < userArgCount; ++k) {
                    const auto argSlot = operandSlot(k + 1u);
                    const auto boxPrim = delegateArgSlot(k).primitive;
                    if (argSlot.primitive == semantic::PrimitiveType::Struct &&
                        classifyInlineMath(argSlot.primitive, argSlot.typeId) == InlineMathKind::None) {
                        source.line("                boxedDelArgs[boxedCount++] = " + operand(k + 1u) + ".boxToValue();");
                    } else {
                        source.line("                boxedDelArgs[boxedCount++] = aot::typedBoxScalar(" +
                            primitiveExpression(boxPrim) + ", " + operand(k + 1u) + ");");
                    }
                }
                source.line("                context.setActiveGeneratedLine(typedActiveGenLine);");
                source.line("                runtime::Value boxedDelRes;");
                source.line("                if (!context.invoke(leaf.targetSymbolId, boxedCount == 0 ? nullptr : boxedDelArgs.data(), boxedCount, boxedDelRes)) {");
                source.line("                    return false;");
                source.line("                }");
                if (instruction.resultType != semantic::PrimitiveType::Void) {
                    source.line("                if (!aot::typedUnboxScalar(" +
                        primitiveExpression(instruction.resultType) + ", boxedDelRes, delCallRes_" + suffix + ")) {");
                    source.line("                    return context.fail(runtime::ErrorCode::TypeMismatch, \"AOT delegate return type mismatch\", typedActiveGenLine);");
                    source.line("                }");
                }
                source.line("                return true;");
                source.line("            };");

                const auto invokeDelOpId = std::to_string(
                    runtime::determinismOperationId("invoke.delegate")) + "ULL";
                source.line("            aot::ResolvedAotDelegateLeaf singleLeaf_" + suffix + "{};");
                source.line("            const int delKind_" + suffix + " = context.inspectSingleOrMulticastDelegate(0x" +
                    hexId(instruction.typeId) + "ULL, " + operand(0) + ", singleLeaf_" + suffix + ");");
                source.line("            bool delCallOk_" + suffix + " = false;");
                source.line("            if (RS_AOT_LIKELY(delKind_" + suffix + " == 1)) {");
                source.line("                delCallOk_" + suffix + " = invokeDelLeaf_" + suffix + "(singleLeaf_" + suffix + ");");
                source.line("            } else if (delKind_" + suffix + " == 2) {");
                source.line("                std::vector<aot::ResolvedAotDelegateLeaf> mcFallback_" + suffix + ";");
                source.line("                std::size_t mcCount_" + suffix + " = 0;");
                source.line("                const auto* mcLeaves_" + suffix + " = context.acquireMulticastLeavesFast(0x" +
                    hexId(instruction.typeId) + "ULL, " + operand(0) + ", mcCount_" + suffix + ", mcFallback_" + suffix + ");");
                source.line("                if (RS_AOT_LIKELY(mcLeaves_" + suffix + " != nullptr)) {");
                source.line("                    struct McPinGuard { ExecutionContext& ctx; const aot::ResolvedAotDelegateLeaf* ptr; ~McPinGuard() { ctx.releaseMulticastLeavesFast(ptr); } } mcPin_" + suffix + "{context, mcLeaves_" + suffix + "};");
                source.line("                    delCallOk_" + suffix + " = true;");
                source.line("                    for (std::size_t mcIdx_" + suffix + " = 0; mcIdx_" + suffix + " < mcCount_" + suffix + "; ++mcIdx_" + suffix + ") {");
                source.line("                        if constexpr (FastAccounting && !TrustedRelease) {");
                source.line("                            if (!context.consumeRawTyped(1ULL)) { delCallOk_" + suffix + " = false; break; }");
                source.line("                        } else if constexpr (DeterminismAccounting && !TrustedRelease) {");
                source.line("                            determinismAccounting.resumeAfterCall();");
                source.line("                            if (!determinismAccounting.consume(" + invokeDelOpId + ")) { delCallOk_" + suffix + " = false; break; }");
                source.line("                            determinismAccounting.suspendBeforeCall();");
                source.line("                        } else if constexpr (!TrustedRelease) {");
                source.line("                            if (!context.consume(" + invokeDelOpId + ", \"invoke.delegate\")) { delCallOk_" + suffix + " = false; break; }");
                source.line("                        }");
                source.line("                        if (!invokeDelLeaf_" + suffix + "(mcLeaves_" + suffix + "[mcIdx_" + suffix + "])) { delCallOk_" + suffix + " = false; break; }");
                source.line("                    }");
                source.line("                }");
                source.line("            }");
                source.line("            if constexpr (!TrustedRelease) {");
                source.line("                rawAccounting.resumeAfterCall();");
                source.line("                determinismAccounting.resumeAfterCall();");
                source.line("            }");
                emitScalarCallFailureReturn("            ", "delCallOk_" + suffix);
                if (instruction.result >= 0 &&
                    instruction.resultType != semantic::PrimitiveType::Void) {
                    source.line("            " + result + " = delCallRes_" + suffix + ";");
                    if (instruction.resultType == semantic::PrimitiveType::Array ||
                        instruction.resultType == semantic::PrimitiveType::Object) {
                        emitRetainRegResult();
                    }
                }
                break;
            }
            case mir::Opcode::AsyncEnter:
                break;
            case mir::Opcode::AwaitSuspend: {
                const auto awaitLocExpr = typedLocalExpression(instruction.localIndex);
                source.line("            " + awaitLocExpr + " = " + operand(0) + ";");
                source.line("            if (!context.canSkipAwaitSuspendFast(" + awaitLocExpr + ".f_0)) {");
                source.line("                ScalarCoroPodState savedPod{};");
                for (const auto& f : podCoroFields) {
                    source.line("                savedPod." + f.memberName + " = " + f.exprName + ";");
                }
                std::string savedRefPtr = "nullptr";
                if (!refCoroFields.empty()) {
                    source.line("                std::array<runtime::Value, " +
                        std::to_string(refCoroFields.size()) + "> savedRefs{{");
                    for (const auto& rf : refCoroFields) {
                        source.line("                    " + rf.exprName + ",");
                    }
                    source.line("                }};");
                    savedRefPtr = "savedRefs.data()";
                }
                source.line("                if constexpr (!TrustedRelease) context.setActiveGeneratedLine(typedActiveGenLine);");
                source.line("                bool suspendedNow = false;");
                source.line("                std::int64_t packedCoroResult = 0;");
                source.line("                if (!context.awaitSuspendScalar(0x" +
                    hexId(function.symbolId) + "ULL, 0x" +
                    hexId(function.returnTypeId) + "ULL, " +
                    awaitLocExpr + ".f_0, " +
                    std::to_string(instruction.integerImmediate) +
                    "u, &savedPod, sizeof(ScalarCoroPodState), " +
                    savedRefPtr + ", " + std::to_string(refCoroFields.size()) +
                    "u, activeCoroutine, suspendedNow, packedCoroResult)) return false;");
                source.line("                if (suspendedNow) {");
                source.line("                    typedOutResult.f_0 = packedCoroResult;");
                source.line("                    return true;");
                source.line("                }");
                source.line("            }");
                break;
            }
            case mir::Opcode::AwaitResume: {
                const auto awaitLocExpr = typedLocalExpression(instruction.localIndex);
                source.line("            if constexpr (!TrustedRelease) context.setActiveGeneratedLine(typedActiveGenLine);");
                if (instruction.result < 0 ||
                    instruction.elementType == semantic::PrimitiveType::Void) {
                    source.line("            if (!context.awaitResumeVoidScalar(" + awaitLocExpr + ".f_0)) {");
                    source.line("                if (context.hasPendingException()) {");
                    source.line("                    return context.finishUncaughtAsyncExceptionScalar(activeCoroutine, 0x" +
                        hexId(function.returnTypeId) + "ULL, typedOutResult.f_0);");
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                } else if (instruction.elementType == semantic::PrimitiveType::Int ||
                           instruction.elementType == semantic::PrimitiveType::Long) {
                    source.line("            if (!context.awaitResumeInt64Scalar(" + awaitLocExpr + ".f_0, " +
                        primitiveExpression(instruction.elementType) + ", " + result + ")) {");
                    source.line("                if (context.hasPendingException()) {");
                    source.line("                    return context.finishUncaughtAsyncExceptionScalar(activeCoroutine, 0x" +
                        hexId(function.returnTypeId) + "ULL, typedOutResult.f_0);");
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                } else if (instruction.elementType == semantic::PrimitiveType::Double) {
                    source.line("            if (!context.awaitResumeDoubleScalar(" + awaitLocExpr + ".f_0, " + result + ")) {");
                    source.line("                if (context.hasPendingException()) {");
                    source.line("                    return context.finishUncaughtAsyncExceptionScalar(activeCoroutine, 0x" +
                        hexId(function.returnTypeId) + "ULL, typedOutResult.f_0);");
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                } else if (instruction.elementType == semantic::PrimitiveType::Bool) {
                    source.line("            if (!context.awaitResumeBoolScalar(" + awaitLocExpr + ".f_0, " + result + ")) {");
                    source.line("                if (context.hasPendingException()) {");
                    source.line("                    return context.finishUncaughtAsyncExceptionScalar(activeCoroutine, 0x" +
                        hexId(function.returnTypeId) + "ULL, typedOutResult.f_0);");
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                } else {
                    source.line("            {");
                    source.line("                runtime::Value awaitedBoxedVal;");
                    source.line("                if (!context.awaitResumeBoxedScalar(" + awaitLocExpr + ".f_0, " +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, awaitedBoxedVal)) {");
                    source.line("                    if (context.hasPendingException()) {");
                    source.line("                        return context.finishUncaughtAsyncExceptionScalar(activeCoroutine, 0x" +
                        hexId(function.returnTypeId) + "ULL, typedOutResult.f_0);");
                    source.line("                    }");
                    source.line("                    return false;");
                    source.line("                }");
                    source.line("                if (!aot::typedUnboxScalar(" +
                        primitiveExpression(instruction.elementType) + ", awaitedBoxedVal, " + result + ")) {");
                    source.line("                    return typedFail(runtime::ErrorCode::TypeMismatch, \"AOT await result type mismatch\");");
                    source.line("                }");
                    if (instruction.elementType == semantic::PrimitiveType::Array ||
                        instruction.elementType == semantic::PrimitiveType::Object) {
                        emitRetainRegResult("                ");
                    }
                    source.line("            }");
                }
                break;
            }
            case mir::Opcode::AsyncResolve: {
                source.line("            if constexpr (!TrustedRelease) context.setActiveGeneratedLine(typedActiveGenLine);");
                if (instruction.operands.empty()) {
                    source.line("            if (!context.asyncResolveVoidScalar(activeCoroutine, " +
                        result + ".f_0)) return false;");
                } else {
                    const auto payloadSlot = operandSlot(0);
                    if (payloadSlot.primitive == semantic::PrimitiveType::Int) {
                        source.line("            if (!context.asyncResolveIntScalar(activeCoroutine, " +
                            operand(0) + ", false, " + result + ".f_0)) return false;");
                    } else if (payloadSlot.primitive == semantic::PrimitiveType::Long) {
                        source.line("            if (!context.asyncResolveIntScalar(activeCoroutine, " +
                            operand(0) + ", true, " + result + ".f_0)) return false;");
                    } else if (payloadSlot.primitive == semantic::PrimitiveType::Double) {
                        source.line("            if (!context.asyncResolveDoubleScalar(activeCoroutine, " +
                            operand(0) + ", " + result + ".f_0)) return false;");
                    } else if (payloadSlot.primitive == semantic::PrimitiveType::Bool) {
                        source.line("            if (!context.asyncResolveBoolScalar(activeCoroutine, " +
                            operand(0) + ", " + result + ".f_0)) return false;");
                    } else {
                        source.line("            if (!context.asyncResolveBoxedScalar(activeCoroutine, aot::typedBoxScalar(" +
                            primitiveExpression(payloadSlot.primitive) + ", " + operand(0) +
                            "), " + result + ".f_0)) return false;");
                    }
                }
                break;
            }
            default:
                break;
            }
        }

        if (activeSourceLine) {
            emitGeneratedLineReset(source, options.emitLineDirectives);
            activeSourceLine = false;
        }
        emitTypedRawConsume(
            source, rawConsumeBatches[block.instructions.size()], "            ");
        emitTypedConsume(source, block.terminator.kind, "            ");
        bool explicitTermPoint = false;
        const auto resolvedTermPoint = resolveSequencePoint(
            module,
            function,
            block.id,
            static_cast<std::uint32_t>(block.instructions.size()),
            true,
            block.terminator.sourceSpan,
            &explicitTermPoint);
        switch (block.terminator.kind) {
        case mir::TerminatorKind::ReturnVoid:
            emitLineDirective(
                source, module, resolvedTermPoint, options.emitLineDirectives,
                &sourceMap, function.symbolId, explicitTermPoint);
            source.line("            return true;");
            break;
        case mir::TerminatorKind::ReturnValue: {
            const auto value = typedValueExpression(block.terminator.value);
            emitLineDirective(
                source, module, resolvedTermPoint, options.emitLineDirectives,
                &sourceMap, function.symbolId, explicitTermPoint);
            const auto retValIdx = static_cast<std::size_t>(block.terminator.value);
            const auto retValPrim = (block.terminator.value >= 0 && retValIdx < types.size())
                ? types[retValIdx].primitive
                : function.returnType;
            if (retValPrim == semantic::PrimitiveType::Null &&
                function.returnType == semantic::PrimitiveType::Object) {
                source.line("            typedOutResult = runtime::Value{runtime::NullObject{}};");
            } else if (retValPrim == semantic::PrimitiveType::Null &&
                       function.returnType == semantic::PrimitiveType::Array) {
                source.line("            typedOutResult = runtime::Value{runtime::NullArray{}};");
            } else {
                source.line("            typedOutResult = " + value + ";");
            }
            source.line("            return true;");
            break;
        }
        case mir::TerminatorKind::Jump:
            if (const auto* targetBlock = findBlock(function, block.terminator.target)) {
                source.line("            if constexpr (FastAccounting && !TrustedRelease) {");
                source.line("                rawAccounting.branch();");
                source.line("            } else if constexpr (DeterminismAccounting && !TrustedRelease) {");
                source.line("                determinismAccounting.branch(" +
                    std::to_string(block.terminator.target) + ");");
                source.line("            } else if constexpr (!TrustedRelease) {");
                source.line("                if (!context.branch(" +
                    std::to_string(block.terminator.target) + ")) return false;");
                source.line("            }");
                emitLineDirective(
                    source, module, resolvedTermPoint, options.emitLineDirectives,
                    &sourceMap, function.symbolId, explicitTermPoint);
                for (std::size_t idx = 0;
                     idx < block.terminator.arguments.size() && idx < targetBlock->parameters.size();
                     ++idx) {
                    source.line("            const auto typedEdge_" + std::to_string(idx) + " = " +
                        typedValueExpression(block.terminator.arguments[idx]) + ";");
                }
                for (std::size_t idx = 0;
                     idx < block.terminator.arguments.size() && idx < targetBlock->parameters.size();
                     ++idx) {
                    const auto paramVal = targetBlock->parameters[idx].value;
                    source.line("            " +
                        typedValueExpression(paramVal) +
                        " = typedEdge_" + std::to_string(idx) + ";");
                    if (paramVal >= 0) {
                        const auto rIdx = static_cast<std::size_t>(paramVal);
                        if (rIdx < regGuardSlot.size() &&
                            regGuardSlot[rIdx] != std::numeric_limits<std::size_t>::max()) {
                            source.line("            if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
                            source.line("                aot::retainTypedGcSlot(context.rawHeap(), typedLocalGcGuards[" +
                                std::to_string(regGuardSlot[rIdx]) + "], " +
                                typedValueExpression(paramVal) + ");");
                            source.line("            }");
                        }
                    }
                }
                source.line("            goto typedBlock_" + std::to_string(block.terminator.target) + ";");
            }
            break;
        case mir::TerminatorKind::Branch:
            emitLineDirective(
                source, module, resolvedTermPoint, options.emitLineDirectives,
                &sourceMap, function.symbolId, explicitTermPoint);
            source.line("            if (" +
                typedValueExpression(block.terminator.condition) + ") {");
            emitGeneratedLineReset(source, options.emitLineDirectives);
            emitTypedTransfer(
                source, function, block.terminator.target,
                block.terminator.arguments, "typedTrueEdge", "                ",
                &regGuardSlot);
            source.line("            } else {");
            emitTypedTransfer(
                source, function, block.terminator.falseTarget,
                block.terminator.falseArguments, "typedFalseEdge", "                ",
                &regGuardSlot);
            source.line("            }");
            break;
        case mir::TerminatorKind::Throw:
        case mir::TerminatorKind::None:
            break;
        }
        emitGeneratedLineReset(source, options.emitLineDirectives);
        source.line("    }");
    }
    source.line("}");
    source.line();
    source.line("template <bool FastAccounting, bool DeterminismAccounting, bool TrustedRelease = false>");
    source.line("static bool " + view.cppName + "_typed(");
    source.line("    ExecutionContext& context,");
    source.line("    const runtime::Value* arguments,");
    source.line("    std::size_t argumentCount,");
    source.line("    runtime::Value& result) {");
    source.line("    (void)arguments;");
    source.line("    (void)argumentCount;");
    source.line("    (void)result;");
    source.line("    std::conditional_t<!FastAccounting && !DeterminismAccounting && !TrustedRelease, std::vector<runtime::PersistentRoot>, aot::EmptyGcGuards> typedGcGuards;");
    const bool guardResumeArgs = isAsyncFunction && !function.parameterTypes.empty();
    if (guardResumeArgs) {
        source.line("    const bool isResumingCoro = context.isResumingCoroutine(0x" +
            hexId(function.symbolId) + "ULL);");
    }
    source.line("    if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
    if (guardResumeArgs) {
        source.line("        if (RS_AOT_LIKELY(!isResumingCoro) && context.heap() != nullptr) {");
    } else {
        source.line("        if (context.heap() != nullptr) {");
    }
    source.line("            for (std::size_t idx = 0; idx < argumentCount; ++idx) {");
    source.line("                if (std::holds_alternative<runtime::ObjectRef>(arguments[idx])) {");
    source.line("                    typedGcGuards.push_back(context.heap()->retain(arguments[idx]));");
    source.line("                }");
    source.line("            }");
    source.line("        }");
    source.line("    }");
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        const auto typeId = index < function.parameterTypeIds.size()
            ? function.parameterTypeIds[index]
            : 0;
        const TypedSlotType paramSlot{function.parameterTypes[index], typeId};
        const auto argName = "unboxedArg_" + std::to_string(index);
        source.line("    " + typedCppType(paramSlot) + " " + argName + "{};");
    }
    if (!function.parameterTypes.empty()) {
        if (guardResumeArgs) {
            source.line("    if (RS_AOT_LIKELY(!isResumingCoro)) {");
        }
        const std::string pInd = guardResumeArgs ? "        " : "    ";
        for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
            const auto typeId = index < function.parameterTypeIds.size()
                ? function.parameterTypeIds[index]
                : 0;
            const TypedSlotType paramSlot{function.parameterTypes[index], typeId};
            const auto argName = "unboxedArg_" + std::to_string(index);
            source.line(pInd + "if (" + std::to_string(index) +
                "u >= argumentCount) return context.fail("
                "runtime::ErrorCode::InvalidArguments, "
                "\"AOT parameter index is invalid\");");
            source.line(pInd + "if (!aot::typedUnboxScalar(" +
                primitiveExpression(paramSlot.primitive) + ", arguments[" +
                std::to_string(index) + "], " + argName +
                ")) return context.fail(runtime::ErrorCode::TypeMismatch, "
                "\"AOT parameter type mismatch\");");
        }
        if (guardResumeArgs) {
            source.line("    }");
        }
    }
    std::string scalarCallArgs = "context";
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        scalarCallArgs += ", unboxedArg_" + std::to_string(index);
    }
    if (function.returnType != semantic::PrimitiveType::Void) {
        const TypedSlotType retSlot{function.returnType, function.returnTypeId};
        source.line("    " + typedCppType(retSlot) + " typedOutResult{};");
        scalarCallArgs += ", typedOutResult";
        source.line("    if (!" + view.cppName +
            "_scalar<FastAccounting, DeterminismAccounting, TrustedRelease>(" +
            scalarCallArgs + ")) return false;");
        source.line("    result = aot::typedBoxScalar(" +
            primitiveExpression(function.returnType) + ", typedOutResult);");
        source.line("    return true;");
    } else {
        source.line("    if (!" + view.cppName +
            "_scalar<FastAccounting, DeterminismAccounting, TrustedRelease>(" +
            scalarCallArgs + ")) return false;");
        source.line("    result = std::monostate{};");
        source.line("    return true;");
    }
    source.line("}");
    source.line();
    source.line("static bool " + view.cppName + "(");
    source.line("    ExecutionContext& context,");
    source.line("    const runtime::Value* arguments,");
    source.line("    std::size_t argumentCount,");
    source.line("    runtime::Value& result) {");
    if (options.trustedRelease) {
        source.line("    if (context.trustedReleaseEnabled() || context.fastAccountingEnabled()) {");
        source.line("        return " + view.cppName +
            "_typed<false, false, true>(context, arguments, argumentCount, result);");
        source.line("    }");
    } else {
        source.line("    if (context.trustedReleaseEnabled()) {");
        source.line("        return " + view.cppName +
            "_typed<false, false, true>(context, arguments, argumentCount, result);");
        source.line("    }");
        source.line("    if (context.fastAccountingEnabled()) {");
        source.line("        return " + view.cppName +
            "_typed<true, false, false>(context, arguments, argumentCount, result);");
        source.line("    }");
    }
    source.line("    if (context.typedDeterminismAccountingEnabled()) {");
    source.line("        return " + view.cppName +
        "_typed<false, true, false>(context, arguments, argumentCount, result);");
    source.line("    }");
    source.line("    return " + view.cppName +
        "_typed<false, false, false>(context, arguments, argumentCount, result);");
    source.line("}");
    source.line();
}

} // namespace realscript::aot::detail
