#include "InterpreterInternal.h"

namespace realscript::runtime::interp_detail {

bool executeFunction(
    State& state,
    const ProgramImage::FunctionLocation& location,
    const ArgumentSource& arguments,
    Value& result,
    const detail::ExecutionFunction* cachedExecution,
    bool argumentsPrevalidated,
    std::uint32_t callerStructReturnAbsOffset,
    std::uint16_t callerStructReturnSchemaId) {
    const auto& function = *location.function;
    struct TelemetrySelfInstructionsScope {
        State& state;
        semantic::SymbolId symbolId;
        std::uint64_t entryExecuted;
        std::uint64_t savedParentChildExecuted;
        TelemetrySelfInstructionsScope(State& st, semantic::SymbolId sym) noexcept
            : state(st),
              symbolId(sym),
              entryExecuted(st.executed),
              savedParentChildExecuted(st.telemetryChildExecuted) {
            st.telemetryChildExecuted = 0;
        }
        ~TelemetrySelfInstructionsScope() noexcept {
            if (state.telemetry != nullptr) {
                const std::uint64_t totalSubtree = state.executed - entryExecuted;
                const std::uint64_t selfInst = totalSubtree >= state.telemetryChildExecuted
                    ? (totalSubtree - state.telemetryChildExecuted)
                    : 0u;
                if (selfInst > 0) {
                    state.telemetry->recordFunctionInstructions(symbolId, selfInst);
                }
                state.telemetryChildExecuted = savedParentChildExecuted + totalSubtree;
            }
        }
    } telemetryScope{state, function.symbolId};
    const auto* execution = cachedExecution
        ? cachedExecution
        : detail::findExecutionFunction(
            detail::executionMirror(*state.program), function);
    if (!execution || execution->entry == detail::InvalidBlockIndex) {
        return fail(state, ErrorCode::InvalidProgram,
            "function execution mirror is missing");
    }
    if (state.stack.size() >= state.limits.recursionLimit) {
        return fail(state, ErrorCode::RecursionLimitExceeded, "recursion limit exceeded");
    }
    CoroutineFrameHandle activeCoroutine{};
    CoroutineFrame* activeCoroutineFrame = nullptr;
    if (state.resumingFramePtr != nullptr &&
        state.resumingCoroutine.valid() &&
        function.symbolId == state.resumingFramePtr->functionSymbolId) {
        activeCoroutine = state.resumingCoroutine;
        activeCoroutineFrame = state.resumingFramePtr;
        state.resumingCoroutine = CoroutineFrameHandle::invalid();
        state.resumingFramePtr = nullptr;
        argumentsPrevalidated = true;
    }
    if (!argumentsPrevalidated &&
        arguments.size() != function.parameterTypes.size()) {
        return fail(state, ErrorCode::InvalidArguments, "argument count does not match function signature");
    }
    if (!argumentsPrevalidated) {
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (!expectSignatureType(
                    state,
                    arguments[index],
                    function.parameterTypes[index],
                    typeIdAt(function.parameterTypeIds, index),
                    "argument")) {
                return false;
            }
        }
    }

    if (state.fastAccounting && execution->directNativeReference != nullptr) {
        const std::uint64_t cost = execution->nativeForwarderInstructionCost;
        if (state.executed > state.limits.instructionBudget ||
            cost > state.limits.instructionBudget - state.executed) {
            state.executed = state.limits.instructionBudget;
            return fail(state, ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        state.executed += cost;
        ++state.statistics.functionCalls;
        ++state.statistics.externalCalls;
        if (state.stack.size() + 1 > state.statistics.maximumCallDepth) {
            state.statistics.maximumCallDepth = state.stack.size() + 1;
        }

        const auto& reference = *execution->directNativeReference;
        std::array<Value, 8> stackArguments{};
        std::vector<Value> heapArguments;
        ValueSpan argSpan;
        if (arguments.empty()) {
            argSpan = ValueSpan(nullptr, 0);
        } else if (!arguments.indices) {
            argSpan = ValueSpan(
                arguments.values->data() + arguments.offset,
                arguments.count);
        } else if (arguments.count == 1) {
            argSpan = ValueSpan(
                arguments.values->data() + arguments.offset + arguments.indices[0],
                1);
        } else {
            bool contiguous = true;
            const auto firstIndex = arguments.indices[0];
            for (std::size_t index = 1; index < arguments.count; ++index) {
                if (arguments.indices[index] != firstIndex + index) {
                    contiguous = false;
                    break;
                }
            }
            if (contiguous) {
                argSpan = ValueSpan(
                    arguments.values->data() + arguments.offset + firstIndex,
                    arguments.count);
            } else if (arguments.count <= stackArguments.size()) {
                for (std::size_t index = 0; index < arguments.count; ++index) {
                    stackArguments[index] = arguments[index];
                }
                argSpan = ValueSpan(stackArguments.data(), arguments.count);
            } else {
                heapArguments.reserve(arguments.count);
                for (std::size_t index = 0; index < arguments.count; ++index) {
                    heapArguments.push_back(arguments[index]);
                }
                argSpan = ValueSpan(heapArguments);
            }
        }

        RuntimeError externalError;
        std::optional<Value> externalResult;
        const ExternalFunction* fallback =
            state.externalResolver && *state.externalResolver
            ? state.externalResolver
            : nullptr;
        std::vector<Value> detachedNativeArgs;
        for (std::size_t idx = 0; idx < argSpan.size(); ++idx) {
            if (argSpan[idx].rawTag() == ValueTag::Struct &&
                argSpan[idx].asStructUnchecked().isFrameSlice()) {
                detachedNativeArgs.resize(argSpan.size());
                for (std::size_t k = 0; k < argSpan.size(); ++k) {
                    detachedNativeArgs[k] = detachFrameSliceValue(argSpan[k]);
                }
                argSpan = ValueSpan(detachedNativeArgs);
                break;
            }
        }
        if (!state.determinism.invokeExternal(
                state.bindings,
                fallback,
                reference,
                argSpan,
                externalResult,
                externalError)) {
            state.stack.push_back(&location.qualifiedName);
            state.error = std::move(externalError);
            if (state.error.code == ErrorCode::None) {
                state.error.code = ErrorCode::ExternalFunctionUnresolved;
                state.error.message = "external function '" + reference.name + "' failed";
            }
            state.error.stackTrace = materializeStack(state.stack);
            std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
            populateInterpreterSourceLocations(state);
            state.stack.pop_back();
            return false;
        }
        if (function.returnType == semantic::PrimitiveType::Void) {
            result = std::monostate{};
            return true;
        }
        if (!expectSignatureType(
                state,
                *externalResult,
                function.returnType,
                function.returnTypeId,
                "external return value")) {
            return false;
        }
        result = std::move(*externalResult);
        return true;
    }

    if (state.fastAccounting && execution->canUseFlatFastPath) {
#if defined(__GNUC__) || defined(__clang__)
        #define RS_FLAT_LIKELY(x) __builtin_expect(!!(x), 1)
        #define RS_FLAT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
        #define RS_FLAT_LIKELY(x) (x)
        #define RS_FLAT_UNLIKELY(x) (x)
#endif
        const auto previousFunctionId = state.currentFunctionId;
        state.currentFunctionId = function.symbolId;
        struct InlineTelemetryFrame {
            semantic::SymbolId symbolId = 0;
            std::uint64_t enterLocalExecuted = 0;
            std::uint64_t nestedChildInst = 0;
        };
        std::array<InlineTelemetryFrame, 8> inlineTelemetryStack{};
        std::uint32_t inlineTelemetryDepth = 0;
        struct RestoreFastFunctionId {
            State& state;
            semantic::SymbolId previous;
            ~RestoreFastFunctionId() { state.currentFunctionId = previous; }
        } restoreFastFunctionId{state, previousFunctionId};

        state.stack.push_back(&location.qualifiedName);
        ++state.statistics.functionCalls;
        if (state.stack.size() > state.statistics.maximumCallDepth) {
            state.statistics.maximumCallDepth = state.stack.size();
        }

        const std::size_t frameOffset = state.valueTop;
        const std::size_t frameEnd = frameOffset + execution->totalFrameSlots;
        const std::size_t previousSize = state.values.size();
        if (RS_FLAT_UNLIKELY(frameEnd > previousSize)) {
            if (frameEnd > state.values.capacity()) {
                state.values.reserve(std::max<std::size_t>(frameEnd, state.values.capacity() * 2 + 256));
            }
            state.values.resize(frameEnd);
        }

        Value* __restrict frameBase = state.values.data() + frameOffset;
        const std::size_t argCount = activeCoroutineFrame
            ? activeCoroutineFrame->savedArguments.size()
            : arguments.size();
        std::uint32_t initialEntryOffset = execution->entryStreamOffset;
        std::uint64_t prologueSpanBonus = 0;
        if (activeCoroutineFrame != nullptr) {
            const std::size_t dirtyEnd = std::min(frameEnd, previousSize);
            if (frameOffset < dirtyEnd) {
                std::memset(static_cast<void*>(frameBase), 0, (dirtyEnd - frameOffset) * sizeof(Value));
            }
            const std::size_t nArgs = std::min(argCount, function.parameterTypes.size());
            for (std::size_t i = 0; i < nArgs; ++i) {
                storeValueIntoFlatFrameSlot(
                    state, *execution, frameOffset, frameBase,
                    static_cast<std::uint16_t>(execution->paramOffset + i),
                    activeCoroutineFrame->savedArguments[i]);
            }
            const std::size_t nLocs = std::min(
                activeCoroutineFrame->savedLocals.size(), function.localTypes.size());
            for (std::size_t i = 0; i < nLocs; ++i) {
                storeValueIntoFlatFrameSlot(
                    state, *execution, frameOffset, frameBase,
                    static_cast<std::uint16_t>(execution->localOffset + i),
                    activeCoroutineFrame->savedLocals[i]);
            }
            const std::size_t nRegs = std::min(
                activeCoroutineFrame->savedRegisters.size(), function.registerTypes.size());
            for (std::size_t i = 0; i < nRegs; ++i) {
                storeValueIntoFlatFrameSlot(
                    state, *execution, frameOffset, frameBase,
                    static_cast<std::uint16_t>(execution->registerOffset + i),
                    activeCoroutineFrame->savedRegisters[i]);
            }
            const auto foundResumeBlk = execution->blockIndices.find(
                activeCoroutineFrame->resumeBlockId);
            if (foundResumeBlk == execution->blockIndices.end()) {
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram, "invalid coroutine resume block id");
            }
            initialEntryOffset = execution->blockStreamOffsets[foundResumeBlk->second];
        } else if (execution->hasFastParamToLocalPrologue &&
            state.limits.gcWorkBudget == 0 &&
            execution->entryStreamOffset == 0 &&
            argCount > 0 &&
            RS_FLAT_LIKELY(state.executed + 2u * argCount <= state.limits.instructionBudget)) {
            const std::size_t locOff = execution->localOffset;
            const std::size_t nLocals = function.localTypes.size();
            Value* __restrict localBase = frameBase + locOff;
            for (std::size_t i = 0; i < argCount; ++i) {
                localBase[i] = arguments[i];
            }
            if (nLocals > argCount) {
                std::memset(static_cast<void*>(localBase + argCount), 0, (nLocals - argCount) * sizeof(Value));
            }
            prologueSpanBonus = 2u * argCount;
            initialEntryOffset = static_cast<std::uint32_t>(argCount);
        } else if (RS_FLAT_UNLIKELY(execution->hasStructPayloads && state.flatBankId != 0u)) {
            // Note: Copy incoming arguments BEFORE zeroing dirty slots, or copy from caller frame (< frameOffset)
            for (std::size_t i = 0; i < argCount; ++i) {
                frameBase[i] = arguments[i];
            }
            const std::size_t dirtyEnd = std::min(frameEnd, previousSize);
            if (frameOffset + argCount < dirtyEnd) {
                std::memset(static_cast<void*>(frameBase + argCount), 0,
                    (dirtyEnd - (frameOffset + argCount)) * sizeof(Value));
            }
            for (std::size_t i = 0; i < argCount; ++i) {
                const auto pSlot = static_cast<std::uint16_t>(execution->paramOffset + i);
                if (execution->slotStructSchemaIds[pSlot] != 0u) {
                    const Value incoming = frameBase[pSlot];
                    storeValueIntoFlatFrameSlot(state, *execution, frameOffset, frameBase, pSlot, incoming);
                }
            }
            const std::size_t nLocals = function.localTypes.size();
            for (std::size_t l = 0; l < nLocals; ++l) {
                const auto lSlot = static_cast<std::uint16_t>(execution->localOffset + l);
                const std::uint16_t sid = execution->slotStructSchemaIds[lSlot];
                if (sid != 0u) {
                    const std::uint16_t payOff = execution->slotPayloadOffsets[lSlot];
                    const auto* sc = getFlatStructSchema(sid);
                    if (payOff != detail::InvalidCompactReg && sc != nullptr) {
                        std::memcpy(
                            static_cast<void*>(frameBase + payOff),
                            static_cast<const void*>(sc->prototypeLeaves.data()),
                            static_cast<std::size_t>(sc->flatSlotCount) * sizeof(Value));
                        frameBase[lSlot] = StructValue::makeFrameSlice(
                            sc->structTypeId,
                            state.flatBankId,
                            static_cast<std::uint32_t>(frameOffset + payOff),
                            sid);
                    }
                }
            }
        } else {
            for (std::size_t i = 0; i < argCount; ++i) {
                frameBase[i] = arguments[i];
            }
            const std::size_t dirtyEnd = std::min(frameEnd, previousSize);
            if (frameOffset + argCount < dirtyEnd) {
                std::memset(static_cast<void*>(frameBase + argCount), 0,
                    (dirtyEnd - (frameOffset + argCount)) * sizeof(Value));
            }
        }
        state.valueTop = frameEnd;

        const auto routeFastException = [&](bytecode::BlockId protectedBlock) -> std::uint32_t {
            if (!state.pendingException ||
                !std::holds_alternative<ObjectRef>(*state.pendingException)) {
                return detail::InvalidStreamOffset;
            }
            const auto reference = std::get<ObjectRef>(*state.pendingException);
            const auto actual = state.heap->objectTypeId(reference);
            if (!actual) return detail::InvalidStreamOffset;
            for (const auto& handler : function.exceptionHandlers) {
                if (std::find(handler.protectedBlocks.begin(),
                              handler.protectedBlocks.end(), protectedBlock) ==
                    handler.protectedBlocks.end()) {
                    continue;
                }
                if (handler.catchTypeId != 0 &&
                    !runtimeAssignable(state, *actual, handler.catchTypeId)) {
                    continue;
                }
                if (handler.exceptionLocal >= function.localTypes.size()) {
                    return detail::InvalidStreamOffset;
                }
                frameBase[execution->localOffset + handler.exceptionLocal] = *state.pendingException;
                state.pendingException.reset();
                const auto foundBlk = execution->blockIndices.find(handler.handlerBlock);
                if (foundBlk == execution->blockIndices.end()) {
                    return detail::InvalidStreamOffset;
                }
                return execution->blockStreamOffsets[foundBlk->second];
            }
            return detail::InvalidStreamOffset;
        };

        std::uint64_t localExecuted = state.executed + prologueSpanBonus;
        std::uint64_t gcAccounted = localExecuted;
        const bool gcBudgetEnabled = (state.limits.gcWorkBudget != 0 && state.heap != nullptr);
        bool gcActive = gcBudgetEnabled &&
            (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
        struct FlatArrayCacheEntry {
            ObjectRef ref{};
            ArrayFastView view{};
            PackedArrayFastView packedView{};
            FlatStructArrayFastView flatStructView{};
            bool isPacked = false;
            bool isFlatStruct = false;
            bool valid = false;
        };
        struct FlatObjectCacheEntry {
            ObjectRef ref{};
            ObjectFastView view{};
            semantic::SymbolId verifiedBaseTid = 0;
            bool valid = false;
        };
        std::array<FlatArrayCacheEntry, 4> arrayCache{};
        std::array<FlatObjectCacheEntry, 4> objectCache{};
        semantic::SymbolId cachedAssignableSubTid = 0;
        semantic::SymbolId cachedAssignableBaseTid = 0;

        const auto invalidateAllFlatCaches = [&]() noexcept {
            arrayCache[0].valid = false;
            arrayCache[1].valid = false;
            arrayCache[2].valid = false;
            arrayCache[3].valid = false;
            objectCache[0].valid = false;
            objectCache[1].valid = false;
            objectCache[2].valid = false;
            objectCache[3].valid = false;
        };

        const auto bindArrayCacheSlot = [&](const Value& arrVal, FlatArrayCacheEntry& ce) noexcept -> bool {
            if (RS_FLAT_UNLIKELY(arrVal.rawTag() != ValueTag::ObjectRef || state.heap == nullptr)) {
                return false;
            }
            const ObjectRef ref = arrVal.u_.objectRef;
            if (state.heap->tryBindPackedArrayFastView(ref, ce.packedView)) {
                ce.ref = ref;
                ce.view = {};
                ce.view.length = ce.packedView.length;
                ce.view.elementType = ce.packedView.elementType;
                ce.view.elementTypeId = ce.packedView.elementTypeId;
                ce.isPacked = true;
                ce.isFlatStruct = false;
                ce.valid = true;
                return true;
            }
            if (state.heap->tryBindFlatStructArrayFastView(ref, ce.flatStructView)) {
                ce.ref = ref;
                ce.view = {};
                ce.view.length = ce.flatStructView.length;
                ce.view.elementType = semantic::PrimitiveType::Struct;
                ce.view.elementTypeId = ce.flatStructView.elementTypeId;
                ce.isPacked = false;
                ce.isFlatStruct = true;
                ce.valid = true;
                return true;
            }
            if (RS_FLAT_UNLIKELY(!state.heap->tryBindArrayFastView(ref, ce.view))) {
                return false;
            }
            ce.ref = ref;
            ce.isPacked = false;
            ce.isFlatStruct = false;
            ce.valid = true;
            return true;
        };

        const auto bindObjectCacheEntry = [&](const ObjectRef& ref, FlatObjectCacheEntry& oce) noexcept -> bool {
            if (RS_FLAT_UNLIKELY(state.heap == nullptr || !state.heap->tryBindObjectFastView(ref, oce.view))) {
                return false;
            }
            oce.ref = ref;
            oce.verifiedBaseTid = 0;
            oce.valid = true;
            return true;
        };

        const auto flushFlatGc = [&]() {
            if (localExecuted > gcAccounted) {
                state.executed = localExecuted;
                stepGcIfNeeded(state, localExecuted - gcAccounted);
                gcAccounted = localExecuted;
                invalidateAllFlatCaches();
            }
            gcActive = gcBudgetEnabled &&
                (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
        };
        const auto syncAfterFlatSubcall = [&]() {
            localExecuted = state.executed;
            gcAccounted = localExecuted;
            invalidateAllFlatCaches();
            if (gcBudgetEnabled) {
                gcActive = (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
            }
        };
        const std::uint64_t budgetLimit = state.limits.instructionBudget;
        const detail::FlatInstruction* const streamBase = execution->flatStream.data();
        const detail::FlatColdMeta* const flatColdBase = execution->flatCold.data();
        const detail::FlatInstruction* ip = streamBase + initialEntryOffset;

#if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wpedantic"
        static constexpr const void* kFlatDispatchTable[] = {
            &&L_MoveSlot,
            &&L_ConstantInt,
            &&L_AddIntChecked,
            &&L_AddIntUnchecked,
            &&L_SubIntChecked,
            &&L_SubIntUnchecked,
            &&L_MulIntChecked,
            &&L_MulIntUnchecked,
            &&L_DivInt,
            &&L_RemInt,
            &&L_LessInt,
            &&L_LessOrEqualInt,
            &&L_GreaterInt,
            &&L_GreaterOrEqualInt,
            &&L_EqualInt,
            &&L_NotEqualInt,
            &&L_SuperMoveParamStoreLocal,
            &&L_SuperBinaryLocalIntConst,
            &&L_SuperBinaryLocalIntConstStore,
            &&L_SuperBinaryLocalIntConstStoreElided,
            &&L_SuperBinaryLocalIntLocal,
            &&L_SuperBinaryLocalIntLocalStore,
            &&L_SuperBinaryLocalIntLocalStoreElided,
            &&L_SuperBinaryLocalIntReg,
            &&L_SuperBinaryLocalIntRegStore,
            &&L_SuperBinaryLocalIntRegStoreElided,
            &&L_SuperBinaryLocalIntRegChainStore,
            &&L_SuperBinaryLocalIntRegChainStoreElided,
            &&L_SuperLessLocalRegBranch,
            &&L_SuperLessLocalConstBranch,
            &&L_CheckNotNullFast,
            &&L_LoadElementIntFast,
            &&L_StoreElementIntFast,
            &&L_CallDirect0,
            &&L_CallDirect1,
            &&L_CallDirect2,
            &&L_CallDirectN,
            &&L_CallPolledFast,
            &&L_ConstantDouble,
            &&L_AddDouble,
            &&L_SubDouble,
            &&L_MulDouble,
            &&L_DivDouble,
            &&L_LessDouble,
            &&L_LessOrEqualDouble,
            &&L_GreaterDouble,
            &&L_GreaterOrEqualDouble,
            &&L_LoadFieldFast,
            &&L_StoreFieldFast,
            &&L_LoadStructFieldFast,
            &&L_StoreStructFieldFast,
            &&L_ConstantBool,
            &&L_ConstantLong,
            &&L_ArrayLengthFast,
            &&L_ConvertIntToLongFast,
            &&L_ConvertIntToDoubleFast,
            &&L_ConvertLongToDoubleFast,
            &&L_SuperBinaryLocalDoubleConstStore,
            &&L_SuperBinaryLocalDoubleConstStoreElided,
            &&L_SuperBinaryLocalDoubleLocalStore,
            &&L_SuperBinaryLocalDoubleLocalStoreElided,
            &&L_NegateIntUnchecked,
            &&L_NegateDouble,
            &&L_LogicalNot,
            &&L_NewArrayFast,
            &&L_AddFloat,
            &&L_SubFloat,
            &&L_MulFloat,
            &&L_DivFloat,
            &&L_LessFloat,
            &&L_LessOrEqualFloat,
            &&L_GreaterFloat,
            &&L_GreaterOrEqualFloat,
            &&L_EqualFloat,
            &&L_NotEqualFloat,
            &&L_NegateFloat,
            &&L_ConvertIntToFloatFast,
            &&L_ConvertDoubleToFloatFast,
            &&L_ConvertFloatToIntFast,
            &&L_ConvertFloatToDoubleFast,
            &&L_ConvertLongToIntFast,
            &&L_SuperConstFloat,
            &&L_SuperConstFloatStoreLocal,
            &&L_SuperBinaryLocalFloatLocalStore,
            &&L_SuperBinaryLocalFloatLocalStoreElided,
            &&L_SuperBinaryLocalFloatReg,
            &&L_SuperBinaryLocalFloatRegElided,
            &&L_SuperBinaryLocalFloatRegStore,
            &&L_SuperBinaryLocalFloatRegStoreElided,
            &&L_SuperBinaryRegFloatStoreElided,
            &&L_NewStructFast,
            &&L_LoadFixedRawFast,
            &&L_StoreFixedRawFast,
            &&L_LoadVec3FieldFast,
            &&L_StoreVec3FieldFast,
            &&L_LoadQuatFieldFast,
            &&L_StoreQuatFieldFast,
            &&L_MathFixedConst,
            &&L_MathFixedFromRaw,
            &&L_MathFixedFromInt,
            &&L_MathFixedRatio,
            &&L_MathFixedNeg,
            &&L_MathFixedAdd,
            &&L_MathFixedSub,
            &&L_MathFixedMul,
            &&L_MathFixedMulInt,
            &&L_MathFixedDiv,
            &&L_MathFixedDivInt,
            &&L_MathFixedSin,
            &&L_MathFixedCos,
            &&L_MathFixedSqrt,
            &&L_MathSqrt64,
            &&L_MathVec3Const,
            &&L_MathVec3Raw,
            &&L_MathVec3Of,
            &&L_MathVec3Neg,
            &&L_MathVec3Add,
            &&L_MathVec3Sub,
            &&L_MathVec3MulFixed,
            &&L_MathVec3MulInt,
            &&L_MathVec3DivFixed,
            &&L_MathVec3DivInt,
            &&L_MathVec3Dot,
            &&L_MathVec3Cross,
            &&L_MathVec2Add,
            &&L_MathVec2Sub,
            &&L_MathVec2MulFixed,
            &&L_MathVec2Dot,
            &&L_MathQuatIdentity,
            &&L_MathQuatDot,
            &&L_MathQuatMul,
            &&L_MathQuatRotate,
            &&L_MathQuatRotateVec3,
            &&L_SuperQuatRotateBoxed,
            &&L_MathQuatNormalize,
            &&L_SuperQuatNormalizedBoxed,
            &&L_MathQuatFromEuler,
            &&L_MathQuatLerp,
            &&L_SuperLoadVec3RawFast,
            &&L_SuperLoadQuatRawFast,
            &&L_SuperFixedMulAdd,
            &&L_SuperVec3MulAdd,
            &&L_SuperVec3AddQuatRotate,
            &&L_SuperQuatMulNormalized,
            &&L_SuperQuatMulChain,
            &&L_SuperQuatMulRotateVec3,
            &&L_SuperMoveParamStoreLocalElided,
            &&L_SuperBinaryLocalIntConstStoreJump,
            &&L_SuperBinaryLocalIntLocalStoreJump,
            &&L_SuperBinaryLocalIntRegStoreJump,
            &&L_SuperCheckNotNullCallPolledFast,
            &&L_InlineCallEnter,
            &&L_InlineReturnVoid,
            &&L_InlineReturnVoidJump,
            &&L_InlineReturnScalar,
            &&L_InlineReturnScalarJump,
            &&L_LoadElementTypedFast,
            &&L_StoreElementPrimFast,
            &&L_StoreElementStructFast,
            &&L_StoreElementObjFast,
            &&L_SuperLessLocalArrayLengthBranch,
            &&L_NewObjectFast,
            &&L_LoadFieldExactFast,
            &&L_StoreFieldExactFast,
            &&L_IsTypeObjFast,
            &&L_AsTypeObjFast,
            &&L_NewDelegateFast,
            &&L_CombineDelegateFast,
            &&L_RemoveDelegateFast,
            &&L_InvokeDelegateFast,
            &&L_AsyncEnter,
            &&L_AwaitSuspend,
            &&L_AwaitResume,
            &&L_AsyncResolve,
            &&L_AddIntTrustedChecked,
            &&L_AddIntTrustedUnchecked,
            &&L_SubIntTrustedChecked,
            &&L_SubIntTrustedUnchecked,
            &&L_MulIntTrustedChecked,
            &&L_MulIntTrustedUnchecked,
            &&L_LessIntTrusted,
            &&L_AddDoubleTrusted,
            &&L_SubDoubleTrusted,
            &&L_MulDoubleTrusted,
            &&L_DivDoubleTrusted,
            &&L_AddFloatTrusted,
            &&L_SubFloatTrusted,
            &&L_MulFloatTrusted,
            &&L_InitStructSlice,
            &&L_AliasStructSlice,
            &&L_CopyStructSlice,
            &&L_LoadStructSliceField,
            &&L_StoreStructSliceField,
            &&L_StoreLocalStructFieldInPlace,
            &&L_StoreElementStructFieldInPlace,
            &&L_InitStackRefCell,
            &&L_BindLocalStackRef,
            &&L_WritebackLocalStackRef,
            &&L_LoadStackRefCell,
            &&L_StoreStackRefCell,
            &&L_StoreStackRefStructFieldInPlace,
            &&L_GenericBytecode,
            &&L_TermJumpNoArgs,
            &&L_TermBranchBoolNoArgs,
            &&L_TermBranchGeneric,
            &&L_TermReturnVoid,
            &&L_TermReturnScalarFast,
            &&L_TermReturnGeneric,
            &&L_TermThrow,
        };
        static_assert(
            sizeof(kFlatDispatchTable) / sizeof(kFlatDispatchTable[0]) ==
                static_cast<std::size_t>(detail::FlatOpcode::TermThrow) + 1u,
            "kFlatDispatchTable must match FlatOpcode enum order and size");
        #define RS_FLAT_DISPATCH_NEXT() goto *kFlatDispatchTable[static_cast<std::uint8_t>((++ip)->flatOpcode)]
        #define RS_FLAT_DISPATCH_AT_IP() goto *kFlatDispatchTable[static_cast<std::uint8_t>(ip->flatOpcode)]
        #define RS_FLAT_CASE(op) L_##op:
        RS_FLAT_DISPATCH_AT_IP();
#else
        #define RS_FLAT_DISPATCH_NEXT() ++ip; continue
        #define RS_FLAT_DISPATCH_AT_IP() continue
        #define RS_FLAT_CASE(op) case detail::FlatOpcode::op:
        for (;;) {
            switch (ip->flatOpcode) {
#endif

            RS_FLAT_CASE(MoveSlot) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot] = frameBase[ip->src0Slot];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConstantInt) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot].storeInt64(ip->integerImmediate);
                RS_FLAT_DISPATCH_NEXT();
            }

            #define RS_FLAT_SPAN_CHECK() \
                const std::uint64_t spanCost = ip->span; \
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) { \
                    if (spanCost > 1u) goto L_FlatSuperFallback; \
                    goto L_FlatBudgetExceeded; \
                }

            RS_FLAT_CASE(AddIntChecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                const std::int64_t res = a + b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                    res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AddIntUnchecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a + b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubIntChecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                const std::int64_t res = a - b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                    res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubIntUnchecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a - b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulIntChecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                const std::int64_t res = a * b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                    res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulIntUnchecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a * b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(DivInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                if (RS_FLAT_UNLIKELY(b == 0)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::DivisionByZero, "division by zero");
                    goto L_FlatError;
                }
                if (RS_FLAT_UNLIKELY(a == std::numeric_limits<std::int32_t>::min() && b == -1)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer division overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(a / b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(RemInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                if (RS_FLAT_UNLIKELY(b == 0)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::DivisionByZero, "remainder by zero");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(
                    (a == std::numeric_limits<std::int32_t>::min() && b == -1)
                        ? std::int64_t{0}
                        : (a % b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a < b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessOrEqualInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a <= b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a > b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterOrEqualInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a >= b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(EqualInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a == b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NotEqualInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a != b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperMoveParamStoreLocal) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                const auto& val = frameBase[ip->src0Slot];
                frameBase[ip->dstSlot] = val;
                frameBase[ip->storeSlot] = val;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntConst) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) goto L_FlatTypeMismatch;
                const std::int64_t right = ip->integerImmediate;
                frameBase[ip->dstSlot].storeInt64(left);
                frameBase[ip->aux0Slot].storeInt64(right);
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->aux1Slot]))) {
                    goto L_FlatError;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntConstStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) goto L_FlatTypeMismatch;
                const std::int64_t right = ip->integerImmediate;
                frameBase[ip->dstSlot].storeInt64(left);
                frameBase[ip->aux0Slot].storeInt64(right);
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(
                        state, ip->binaryOpcode1, ip->checked1(), left, right,
                        frameBase[ip->aux1Slot], &frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntConstStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if ((ip->arithFlags & detail::FlatInstruction::FlagTrustedSrc0) != 0u) {
                    left = frameBase[ip->src0Slot].asInt64Unchecked();
                } else if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) {
                    goto L_FlatTypeMismatch;
                }
                const std::int64_t right = ip->integerImmediate;
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntLocal) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                frameBase[ip->aux0Slot].storeInt64(right);
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->aux1Slot]))) {
                    goto L_FlatError;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntLocalStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                frameBase[ip->aux0Slot].storeInt64(right);
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(
                        state, ip->binaryOpcode1, ip->checked1(), left, right,
                        frameBase[ip->aux1Slot], &frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntLocalStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntReg) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                localExecuted += 2;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->aux1Slot]))) {
                    goto L_FlatError;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                localExecuted += 2;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(
                        state, ip->binaryOpcode1, ip->checked1(), left, right,
                        frameBase[ip->aux1Slot], &frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                localExecuted += 2;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegChainStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right1 = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right1))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                localExecuted += 2;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right1, frameBase[ip->aux0Slot]))) {
                    goto L_FlatError;
                }
                const auto midInt = frameBase[ip->aux0Slot].asInt64Unchecked();
                std::int64_t right2 = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src2Slot].tryGetInt64(right2))) goto L_FlatTypeMismatch;
                ++localExecuted;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(
                        state, ip->binaryOpcode2, ip->checked2(), midInt, right2,
                        frameBase[ip->aux1Slot], &frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegChainStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right1 = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right1))) goto L_FlatTypeMismatch;
                localExecuted += 2;
                state.executed = localExecuted;
                Value midOutput;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right1, midOutput))) {
                    goto L_FlatError;
                }
                const auto midInt = midOutput.asInt64Unchecked();
                std::int64_t right2 = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src2Slot].tryGetInt64(right2))) goto L_FlatTypeMismatch;
                ++localExecuted;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode2, ip->checked2(), midInt, right2, frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperLessLocalRegBranch) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if ((ip->arithFlags & (detail::FlatInstruction::FlagTrustedSrc0 | detail::FlatInstruction::FlagTrustedSrc1)) ==
                    (detail::FlatInstruction::FlagTrustedSrc0 | detail::FlatInstruction::FlagTrustedSrc1)) {
                    left = frameBase[ip->src0Slot].asInt64Unchecked();
                    right = frameBase[ip->src1Slot].asInt64Unchecked();
                } else if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                            !frameBase[ip->src1Slot].tryGetInt64(right))) {
                    goto L_FlatTypeMismatch;
                }
                localExecuted += 3;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                bool cond = false;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::LessInt: cond = (left < right); break;
                case detail::QuickOpcode::LessOrEqualInt: cond = (left <= right); break;
                case detail::QuickOpcode::GreaterInt: cond = (left > right); break;
                case detail::QuickOpcode::GreaterOrEqualInt: cond = (left >= right); break;
                case detail::QuickOpcode::EqualInt: cond = (left == right); break;
                case detail::QuickOpcode::NotEqualInt: cond = (left != right); break;
                default: goto L_FlatSuperFallback;
                }
                ++state.statistics.branchesTaken;
                ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(SuperLessLocalConstBranch) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if ((ip->arithFlags & detail::FlatInstruction::FlagTrustedSrc0) != 0u) {
                    left = frameBase[ip->src0Slot].asInt64Unchecked();
                } else if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) {
                    goto L_FlatTypeMismatch;
                }
                localExecuted += 4;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const std::int64_t right = ip->integerImmediate;
                bool cond = false;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::LessInt: cond = (left < right); break;
                case detail::QuickOpcode::LessOrEqualInt: cond = (left <= right); break;
                case detail::QuickOpcode::GreaterInt: cond = (left > right); break;
                case detail::QuickOpcode::GreaterOrEqualInt: cond = (left >= right); break;
                case detail::QuickOpcode::EqualInt: cond = (left == right); break;
                case detail::QuickOpcode::NotEqualInt: cond = (left != right); break;
                default: goto L_FlatSuperFallback;
                }
                ++state.statistics.branchesTaken;
                ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(CheckNotNullFast) {
                const Value& val = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(val.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = val.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) ||
                                       bindObjectCacheEntry(ref, oce) ||
                                       (ref.isStackSlotRef() && ref.generation == state.flatBankId))) {
                        RS_FLAT_SPAN_CHECK();
                        localExecuted += spanCost;
                        frameBase[ip->dstSlot] = val;
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(LoadElementIntFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                if (RS_FLAT_LIKELY(ce.isPacked)) {
                    if (RS_FLAT_UNLIKELY(ce.packedView.encoding != PackedElemEncoding::Int32)) {
                        goto L_FlatMathFallback;
                    }
                    RS_FLAT_SPAN_CHECK();
                    const auto* rawInts = static_cast<const std::int32_t*>(ce.packedView.rawData);
                    if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                        const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                        localExecuted += spanCost;
                        frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(rawInts[uIdx]));
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    std::int64_t idx = 0;
                    if ((ip->arithFlags & detail::FlatInstruction::FlagTrustedSrc0) != 0u) {
                        idx = frameBase[ip->src1Slot].asInt64Unchecked();
                    } else if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx))) {
                        goto L_FlatMathFallback;
                    }
                    if (RS_FLAT_UNLIKELY(static_cast<std::uint64_t>(idx) >= ce.packedView.length)) {
                        goto L_FlatMathFallback;
                    }
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(rawInts[static_cast<std::size_t>(idx)]));
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_UNLIKELY(ce.view.elementType != semantic::PrimitiveType::Int)) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_SPAN_CHECK();
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot] = ce.view.data[uIdx];
                    RS_FLAT_DISPATCH_NEXT();
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = ce.view.data[static_cast<std::size_t>(idx)];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreElementIntFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const Value& storeVal = frameBase[ip->src2Slot];
                if (RS_FLAT_LIKELY(ce.isPacked)) {
                    if (RS_FLAT_UNLIKELY(ce.packedView.encoding != PackedElemEncoding::Int32)) {
                        goto L_FlatMathFallback;
                    }
                    std::int64_t val64 = 0;
                    if ((ip->arithFlags & detail::FlatInstruction::FlagTrustedSrc1) != 0u) {
                        val64 = storeVal.asInt64Unchecked();
                    } else if (RS_FLAT_UNLIKELY(!storeVal.tryGetInt64(val64))) {
                        goto L_FlatMathFallback;
                    }
                    if (RS_FLAT_UNLIKELY(val64 < std::numeric_limits<std::int32_t>::min() ||
                                         val64 > std::numeric_limits<std::int32_t>::max())) {
                        goto L_FlatMathFallback;
                    }
                    RS_FLAT_SPAN_CHECK();
                    auto* rawInts = static_cast<std::int32_t*>(ce.packedView.rawData);
                    if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                        const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                        localExecuted += spanCost;
                        rawInts[uIdx] = static_cast<std::int32_t>(val64);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    std::int64_t idx = 0;
                    if ((ip->arithFlags & detail::FlatInstruction::FlagTrustedSrc0) != 0u) {
                        idx = frameBase[ip->src1Slot].asInt64Unchecked();
                    } else if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx))) {
                        goto L_FlatMathFallback;
                    }
                    if (RS_FLAT_UNLIKELY(static_cast<std::uint64_t>(idx) >= ce.packedView.length)) {
                        goto L_FlatMathFallback;
                    }
                    localExecuted += spanCost;
                    rawInts[static_cast<std::size_t>(idx)] = static_cast<std::int32_t>(val64);
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_UNLIKELY(ce.view.elementType != semantic::PrimitiveType::Int ||
                                     storeVal.rawTag() != ValueTag::Int64)) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_SPAN_CHECK();
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    localExecuted += spanCost;
                    ce.view.data[uIdx] = storeVal;
                    RS_FLAT_DISPATCH_NEXT();
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                ce.view.data[static_cast<std::size_t>(idx)] = storeVal;
                RS_FLAT_DISPATCH_NEXT();
            }

            #define RS_RESOLVE_CALLER_STRUCT_RETURN(dstSlotExpr, outAbsOff, outSchemaId) \
                std::uint32_t outAbsOff = detail::InvalidStreamOffset; \
                std::uint16_t outSchemaId = 0u; \
                if (RS_FLAT_UNLIKELY(execution->hasStructPayloads && state.flatBankId != 0u && \
                                     (dstSlotExpr) < execution->slotStructSchemaIds.size())) { \
                    outSchemaId = execution->slotStructSchemaIds[(dstSlotExpr)]; \
                    if (outSchemaId != 0u) { \
                        const std::uint16_t payOff = execution->slotPayloadOffsets[(dstSlotExpr)]; \
                        if (payOff != detail::InvalidCompactReg) { \
                            outAbsOff = static_cast<std::uint32_t>(frameOffset + payOff); \
                        } else { \
                            outSchemaId = 0u; \
                        } \
                    } \
                }

            #define RS_COMMIT_CALL_RESULT(dstSlotExpr, callResVar, retSchemaIdVar) \
                if ((dstSlotExpr) != detail::InvalidCompactReg) { \
                    if (RS_FLAT_UNLIKELY((retSchemaIdVar) != 0u && \
                        !((callResVar).rawTag() == ValueTag::Struct && \
                          (callResVar).asStructUnchecked().isFrameSlice()))) { \
                        storeValueIntoFlatFrameSlot(state, *execution, frameOffset, frameBase, (dstSlotExpr), (callResVar)); \
                    } else { \
                        frameBase[(dstSlotExpr)] = std::move(callResVar); \
                    } \
                }

            RS_FLAT_CASE(CallDirect0) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                RS_RESOLVE_CALLER_STRUCT_RETURN(ip->dstSlot, retAbsOff, retSid);
                const ValueWindow regWin{&state.values, frameOffset + execution->registerOffset, function.registerTypes.size()};
                const bool ok = executeFunction(
                    state, *cold.directTarget, ArgumentSource(regWin),
                    callResult, cold.directExecution, true, retAbsOff, retSid);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                RS_COMMIT_CALL_RESULT(ip->dstSlot, callResult, retSid);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallDirect1) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                RS_RESOLVE_CALLER_STRUCT_RETURN(ip->dstSlot, retAbsOff, retSid);
                const ValueWindow singleWin{&state.values, frameOffset + ip->src0Slot, 1};
                const bool ok = executeFunction(
                    state, *cold.directTarget,
                    ArgumentSource(singleWin, static_cast<bytecode::Register>(0)),
                    callResult, cold.directExecution, true, retAbsOff, retSid);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                RS_COMMIT_CALL_RESULT(ip->dstSlot, callResult, retSid);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallDirect2) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                RS_RESOLVE_CALLER_STRUCT_RETURN(ip->dstSlot, retAbsOff, retSid);
                const ValueWindow baseWin{&state.values, frameOffset, execution->totalFrameSlots};
                const bool ok = executeFunction(
                    state, *cold.directTarget,
                    ArgumentSource(
                        baseWin,
                        static_cast<bytecode::Register>(ip->src0Slot),
                        static_cast<bytecode::Register>(ip->src1Slot)),
                    callResult, cold.directExecution, true, retAbsOff, retSid);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                RS_COMMIT_CALL_RESULT(ip->dstSlot, callResult, retSid);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallDirectN) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                RS_RESOLVE_CALLER_STRUCT_RETURN(ip->dstSlot, retAbsOff, retSid);
                const ValueWindow regWin{&state.values, frameOffset + execution->registerOffset, function.registerTypes.size()};
                const bool ok = executeFunction(
                    state, *cold.directTarget,
                    ArgumentSource(regWin, cold.bytecode->operands),
                    callResult, cold.directExecution, true, retAbsOff, retSid);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                RS_COMMIT_CALL_RESULT(ip->dstSlot, callResult, retSid);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallPolledFast) {
            L_CallPolledFastBody:
                const std::uint64_t baseCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || baseCost > budgetLimit - localExecuted)) {
                    if (baseCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const semantic::SymbolId actualTid = oce.view.typeId;
                        const std::uint32_t siteId = ip->auxImm32;
                        const PolymorphicPicWay* hit = nullptr;
                        if (RS_FLAT_LIKELY(state.polyCallCache != nullptr &&
                                           siteId < state.polyCallCache->size())) {
                            auto& pic = (*state.polyCallCache)[siteId];
                            if (pic.ways[0].receiverTypeId == actualTid) hit = &pic.ways[0];
                            else if (pic.ways[1].receiverTypeId == actualTid) hit = &pic.ways[1];
                            else if (pic.ways[2].receiverTypeId == actualTid) hit = &pic.ways[2];
                            else if (pic.ways[3].receiverTypeId == actualTid) hit = &pic.ways[3];
                            else if (state.mirror && siteId < state.mirror->polyCallSites.size()) {
                                if (const auto* pre = resolvePrelinkedPolymorphicTarget(
                                        *state.mirror, actualTid, state.mirror->polyCallSites[siteId])) {
                                    auto& slotWay = pic.ways[pic.nextReplace & 3u];
                                    pic.nextReplace = static_cast<std::uint8_t>((pic.nextReplace + 1u) & 3u);
                                    slotWay = {actualTid, pre->target, pre->execution};
                                    hit = &slotWay;
                                }
                            }
                        }
                        if (RS_FLAT_LIKELY(hit != nullptr)) {
                            const auto* calleeExec = hit->execution;
                            const auto tKind = calleeExec->trivialKind;
                            if (tKind != detail::TrivialMethodKind::None &&
                                RS_FLAT_LIKELY(state.stack.size() < state.limits.recursionLimit)) {
                                const std::uint64_t totalCost = baseCost + calleeExec->trivialInstCost;
                                if (RS_FLAT_LIKELY(totalCost <= budgetLimit - localExecuted)) {
                                    bool trivialHandled = false;
                                    Value trivialOut{};
                                    switch (tKind) {
                                    case detail::TrivialMethodKind::ReturnVoid:
                                        trivialHandled = true;
                                        break;
                                    case detail::TrivialMethodKind::ReturnConst:
                                        trivialOut = calleeExec->trivialConstValue;
                                        trivialHandled = true;
                                        break;
                                    case detail::TrivialMethodKind::ReturnParam: {
                                        const std::uint16_t pIdx = calleeExec->trivialParamIndex;
                                        if (pIdx == 0) {
                                            trivialOut = recvVal;
                                            trivialHandled = true;
                                        } else if (pIdx == 1 && ip->inlineArgCount >= 2u) {
                                            trivialOut = frameBase[ip->src1Slot];
                                            trivialHandled = true;
                                        } else if (pIdx == 2 && ip->inlineArgCount >= 3u) {
                                            trivialOut = frameBase[ip->src2Slot];
                                            trivialHandled = true;
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::ReturnThisFieldExact: {
                                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                                        if (RS_FLAT_LIKELY(f0 < oce.view.fieldCount)) {
                                            const Value& fv = oce.view.fields[f0];
                                            if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(fv.rawTag()) ==
                                                               calleeExec->trivialFieldTag0)) {
                                                trivialOut = fv;
                                                trivialHandled = true;
                                            }
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::StoreThisFieldExact: {
                                        const std::uint16_t pIdx = calleeExec->trivialParamIndex;
                                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                                        if (pIdx == 1 && ip->inlineArgCount >= 2u &&
                                            RS_FLAT_LIKELY(f0 < oce.view.fieldCount)) {
                                            const Value& rhs = frameBase[ip->src1Slot];
                                            if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(rhs.rawTag()) ==
                                                               calleeExec->trivialFieldTag0)) {
                                                oce.view.fields[f0] = rhs;
                                                if (RS_FLAT_UNLIKELY(state.heap->hasDirtyTracking())) {
                                                    state.heap->notifyFieldMutated(oce.ref, f0);
                                                }
                                                trivialHandled = true;
                                            }
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::ReturnThisFieldAddField:
                                    case detail::TrivialMethodKind::ReturnThisFieldSubField: {
                                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                                        const std::uint16_t f1 = calleeExec->trivialFieldIndex1;
                                        std::int64_t a = 0, b = 0;
                                        if (RS_FLAT_LIKELY(f0 < oce.view.fieldCount &&
                                                           f1 < oce.view.fieldCount &&
                                                           oce.view.fields[f0].tryGetInt64(a) &&
                                                           oce.view.fields[f1].tryGetInt64(b))) {
                                            const std::int64_t r =
                                                (tKind == detail::TrivialMethodKind::ReturnThisFieldAddField)
                                                    ? (a + b)
                                                    : (a - b);
                                            if (!calleeExec->trivialCheckedArith) {
                                                trivialOut.storeInt64(wrapInt32(r));
                                                trivialHandled = true;
                                            } else if (RS_FLAT_LIKELY(isValidInt32Payload(r))) {
                                                trivialOut.storeInt64(r);
                                                trivialHandled = true;
                                            }
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::ReturnThisFieldAddConst:
                                    case detail::TrivialMethodKind::ReturnThisFieldSubConst:
                                    case detail::TrivialMethodKind::ReturnThisFieldMulConst: {
                                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                                        std::int64_t a = 0;
                                        if (RS_FLAT_LIKELY(f0 < oce.view.fieldCount &&
                                                           oce.view.fields[f0].tryGetInt64(a))) {
                                            const std::int64_t c =
                                                calleeExec->trivialConstValue.asInt64Unchecked();
                                            std::int64_t r = 0;
                                            bool ovf = false;
                                            if (tKind == detail::TrivialMethodKind::ReturnThisFieldAddConst) {
                                                r = a + c;
                                            } else if (tKind == detail::TrivialMethodKind::ReturnThisFieldSubConst) {
                                                r = a - c;
                                            } else {
#if defined(__GNUC__) || defined(__clang__)
                                                ovf = __builtin_mul_overflow(a, c, &r);
#else
                                                if (RS_FLAT_LIKELY(isValidInt32Payload(a) && isValidInt32Payload(c))) {
                                                    r = a * c;
                                                } else {
                                                    ovf = true;
                                                }
#endif
                                            }
                                            if (!ovf) {
                                                if (!calleeExec->trivialCheckedArith) {
                                                    trivialOut.storeInt64(wrapInt32(r));
                                                    trivialHandled = true;
                                                } else if (RS_FLAT_LIKELY(isValidInt32Payload(r))) {
                                                    trivialOut.storeInt64(r);
                                                    trivialHandled = true;
                                                }
                                            }
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::None:
                                        break;
                                    }
                                    if (RS_FLAT_LIKELY(trivialHandled)) {
                                        localExecuted += totalCost;
                                        ++state.statistics.functionCalls;
                                        if (state.stack.size() + 1 > state.statistics.maximumCallDepth) {
                                            state.statistics.maximumCallDepth = state.stack.size() + 1;
                                        }
                                        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                                        if (ip->dstSlot != detail::InvalidCompactReg) {
                                            frameBase[ip->dstSlot] = trivialOut;
                                        }
                                        RS_FLAT_DISPATCH_NEXT();
                                    }
                                }
                            }
                            localExecuted += baseCost;
                            if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                            state.executed = localExecuted;
                            Value callResult;
                            RS_RESOLVE_CALLER_STRUCT_RETURN(ip->dstSlot, retAbsOff, retSid);
                            const ValueWindow baseWin{&state.values, frameOffset, execution->totalFrameSlots};
                            bool ok = false;
                            if (ip->inlineArgCount == 1u) {
                                ok = executeFunction(
                                    state, *hit->target,
                                    ArgumentSource(baseWin, static_cast<bytecode::Register>(ip->src0Slot)),
                                    callResult, calleeExec, true, retAbsOff, retSid);
                            } else if (ip->inlineArgCount == 2u) {
                                ok = executeFunction(
                                    state, *hit->target,
                                    ArgumentSource(
                                        baseWin,
                                        static_cast<bytecode::Register>(ip->src0Slot),
                                        static_cast<bytecode::Register>(ip->src1Slot)),
                                    callResult, calleeExec, true, retAbsOff, retSid);
                            } else if (ip->inlineArgCount == 3u) {
                                ok = executeFunction(
                                    state, *hit->target,
                                    ArgumentSource(
                                        baseWin,
                                        static_cast<bytecode::Register>(ip->src0Slot),
                                        static_cast<bytecode::Register>(ip->src1Slot),
                                        static_cast<bytecode::Register>(ip->src2Slot)),
                                    callResult, calleeExec, true, retAbsOff, retSid);
                            } else {
                                const auto& cold = flatColdBase[ip - streamBase];
                                const ValueWindow regWin{
                                    &state.values,
                                    frameOffset + execution->registerOffset,
                                    function.registerTypes.size()};
                                ok = executeFunction(
                                    state, *hit->target,
                                    ArgumentSource(regWin, cold.bytecode->operands),
                                    callResult, calleeExec, true, retAbsOff, retSid);
                            }
                            frameBase = state.values.data() + frameOffset;
                            if (RS_FLAT_LIKELY(calleeExec->isSmallLeafInlineCandidate && !gcBudgetEnabled)) {
                                localExecuted = state.executed;
                                gcAccounted = localExecuted;
                            } else {
                                syncAfterFlatSubcall();
                            }
                            if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                            RS_COMMIT_CALL_RESULT(ip->dstSlot, callResult, retSid);
                            RS_FLAT_DISPATCH_NEXT();
                        }
                    }
                }
                if (baseCost > 1u) goto L_FlatSuperFallback;
                ++localExecuted;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                const ValueWindow regWin{&state.values, frameOffset + execution->registerOffset, function.registerTypes.size()};
                const bool ok = executeCall(state, *location.module, *cold.bytecode, regWin, callResult);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    storeValueIntoFlatFrameSlot(state, *execution, frameOffset, frameBase, ip->dstSlot, callResult);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConstantDouble) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                double dVal = 0.0;
                std::memcpy(&dVal, &ip->integerImmediate, sizeof(double));
                frameBase[ip->dstSlot].storeDouble(dVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AddDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(aVal + bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(aVal - bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(aVal * bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(DivDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(aVal / bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal < bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessOrEqualDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal <= bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal > bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterOrEqualDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal >= bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadFieldFast) {
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool ownerOk = (oce.view.typeId == expectedOwnerTid) ||
                                       (oce.verifiedBaseTid == expectedOwnerTid);
                        if (RS_FLAT_UNLIKELY(!ownerOk)) {
                            if (runtimeAssignable(state, oce.view.typeId, expectedOwnerTid)) {
                                oce.verifiedBaseTid = expectedOwnerTid;
                                ownerOk = true;
                            }
                        }
                        const std::uint16_t fIdx = ip->aux0Slot;
                        if (RS_FLAT_LIKELY(ownerOk && fIdx < oce.view.fieldCount)) {
                            const Value& val = oce.view.fields[fIdx];
                            const auto fk = static_cast<detail::FastFieldKind>(
                                static_cast<std::uint8_t>(ip->binaryOpcode2));
                            if (RS_FLAT_LIKELY(fk == detail::FastFieldKind::ExactValueTag)) {
                                if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(val.rawTag()) ==
                                                   static_cast<std::uint8_t>(ip->binaryOpcode1))) {
                                    RS_FLAT_SPAN_CHECK();
                                    localExecuted += spanCost;
                                    frameBase[ip->dstSlot] = val;
                                    RS_FLAT_DISPATCH_NEXT();
                                }
                            } else {
                                const ValueTag vt = val.rawTag();
                                bool fieldOk = false;
                                switch (fk) {
                                case detail::FastFieldKind::Quat:
                                    fieldOk = (vt == ValueTag::QuatInline || vt == ValueTag::QuatArena);
                                    break;
                                case detail::FastFieldKind::Quatf:
                                    fieldOk = (vt == ValueTag::QuatfInline || vt == ValueTag::QuatfArena);
                                    break;
                                case detail::FastFieldKind::String:
                                    fieldOk = (vt == ValueTag::NullString || vt == ValueTag::String ||
                                               (vt == ValueTag::ObjectRef &&
                                                val.u_.objectRef.kind == ObjectKind::String &&
                                                state.heap->isAlive(val.u_.objectRef)));
                                    break;
                                case detail::FastFieldKind::Object:
                                    if (vt == ValueTag::NullObject) {
                                        fieldOk = true;
                                    } else if (vt == ValueTag::ObjectRef) {
                                        const auto expTid = detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId;
                                        const auto actTid = state.heap->objectTypeIdFast(val.u_.objectRef);
                                        if (actTid != 0) {
                                            if (actTid == expTid || expTid == 0 ||
                                                (actTid == cachedAssignableSubTid && expTid == cachedAssignableBaseTid)) {
                                                fieldOk = true;
                                            } else if (runtimeAssignable(state, actTid, expTid)) {
                                                cachedAssignableSubTid = actTid;
                                                cachedAssignableBaseTid = expTid;
                                                fieldOk = true;
                                            }
                                        }
                                    }
                                    break;
                                case detail::FastFieldKind::Array:
                                    if (vt == ValueTag::NullArray) {
                                        fieldOk = true;
                                    } else if (vt == ValueTag::ObjectRef) {
                                        const auto expTid = detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId;
                                        const auto actTid = state.heap->arrayTypeId(val.u_.objectRef);
                                        fieldOk = (actTid.has_value() && (*actTid == expTid || expTid == 0));
                                    }
                                    break;
                                case detail::FastFieldKind::UserStruct:
                                    fieldOk = (vt == ValueTag::Struct &&
                                               val.u_.structVal.valid() &&
                                               val.u_.structVal.typeId == detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId);
                                    break;
                                case detail::FastFieldKind::Enum: {
                                    EnumValue ev{};
                                    fieldOk = (val.tryGetEnum(ev) &&
                                               ev.typeId == detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId);
                                    break;
                                }
                                default:
                                    break;
                                }
                                if (RS_FLAT_LIKELY(fieldOk)) {
                                    RS_FLAT_SPAN_CHECK();
                                    localExecuted += spanCost;
                                    if (RS_FLAT_UNLIKELY(fk == detail::FastFieldKind::UserStruct &&
                                                         execution->hasStructPayloads)) {
                                        storeValueIntoFlatFrameSlot(
                                            state, *execution, frameOffset, frameBase, ip->dstSlot, val);
                                    } else {
                                        frameBase[ip->dstSlot] = val;
                                    }
                                    RS_FLAT_DISPATCH_NEXT();
                                }
                            }
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(StoreFieldFast) {
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool ownerOk = (oce.view.typeId == expectedOwnerTid) ||
                                       (oce.verifiedBaseTid == expectedOwnerTid);
                        if (RS_FLAT_UNLIKELY(!ownerOk)) {
                            if (runtimeAssignable(state, oce.view.typeId, expectedOwnerTid)) {
                                oce.verifiedBaseTid = expectedOwnerTid;
                                ownerOk = true;
                            }
                        }
                        const std::uint16_t fIdx = ip->aux0Slot;
                        if (RS_FLAT_LIKELY(ownerOk && fIdx < oce.view.fieldCount)) {
                            const Value& rhs = frameBase[ip->src1Slot];
                            const auto fk = static_cast<detail::FastFieldKind>(
                                static_cast<std::uint8_t>(ip->binaryOpcode2));
                            if (RS_FLAT_LIKELY(fk == detail::FastFieldKind::ExactValueTag)) {
                                if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(rhs.rawTag()) ==
                                                   static_cast<std::uint8_t>(ip->binaryOpcode1))) {
                                    RS_FLAT_SPAN_CHECK();
                                    localExecuted += spanCost;
                                    oce.view.fields[fIdx] = rhs;
                                    if (RS_FLAT_UNLIKELY(state.heap->hasDirtyTracking())) {
                                        state.heap->notifyFieldMutated(ref, fIdx);
                                    }
                                    RS_FLAT_DISPATCH_NEXT();
                                }
                            } else {
                                const ValueTag vt = rhs.rawTag();
                                bool rhsOk = false;
                                bool needsPin = false;
                                bool needsBarrier = false;
                                switch (fk) {
                                case detail::FastFieldKind::Quat:
                                    if (vt == ValueTag::QuatInline) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::QuatArena) {
                                        rhsOk = true;
                                        needsPin = true;
                                    }
                                    break;
                                case detail::FastFieldKind::Quatf:
                                    if (vt == ValueTag::QuatfInline) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::QuatfArena) {
                                        rhsOk = true;
                                        needsPin = true;
                                    }
                                    break;
                                case detail::FastFieldKind::String:
                                    if (vt == ValueTag::NullString) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::String) {
                                        rhsOk = true;
                                        needsPin = true;
                                    } else if (vt == ValueTag::ObjectRef &&
                                               rhs.u_.objectRef.kind == ObjectKind::String &&
                                               state.heap->isAlive(rhs.u_.objectRef)) {
                                        rhsOk = true;
                                        needsBarrier = true;
                                    }
                                    break;
                                case detail::FastFieldKind::Object:
                                    if (vt == ValueTag::NullObject) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::ObjectRef) {
                                        const auto expTid = detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId;
                                        const auto actTid = state.heap->objectTypeIdFast(rhs.u_.objectRef);
                                        if (actTid != 0) {
                                            if (actTid == expTid || expTid == 0 ||
                                                (actTid == cachedAssignableSubTid && expTid == cachedAssignableBaseTid)) {
                                                rhsOk = true;
                                                needsBarrier = true;
                                            } else if (runtimeAssignable(state, actTid, expTid)) {
                                                cachedAssignableSubTid = actTid;
                                                cachedAssignableBaseTid = expTid;
                                                rhsOk = true;
                                                needsBarrier = true;
                                            }
                                        }
                                    }
                                    break;
                                case detail::FastFieldKind::Array:
                                    if (vt == ValueTag::NullArray) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::ObjectRef) {
                                        const auto expTid = detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId;
                                        const auto actTid = state.heap->arrayTypeId(rhs.u_.objectRef);
                                        if (actTid.has_value() && (*actTid == expTid || expTid == 0)) {
                                            rhsOk = true;
                                            needsBarrier = true;
                                        }
                                    }
                                    break;
                                case detail::FastFieldKind::UserStruct:
                                    if (vt == ValueTag::Struct &&
                                        rhs.u_.structVal.valid() &&
                                        rhs.u_.structVal.typeId == detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId) {
                                        rhsOk = true;
                                        needsPin = true;
                                        needsBarrier = !rhs.u_.structVal.isInline && rhs.u_.structVal.storage != nullptr;
                                    }
                                    break;
                                case detail::FastFieldKind::Enum: {
                                    EnumValue ev{};
                                    if (rhs.tryGetEnum(ev) &&
                                        ev.typeId == detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId) {
                                        rhsOk = true;
                                        needsPin = (vt == ValueTag::EnumArena);
                                    }
                                    break;
                                }
                                default:
                                    break;
                                }
                                if (RS_FLAT_LIKELY(rhsOk)) {
                                    RS_FLAT_SPAN_CHECK();
                                    if (RS_FLAT_UNLIKELY(fk == detail::FastFieldKind::UserStruct &&
                                                         rhs.u_.structVal.isFrameSlice())) {
                                        Value detached = detachFrameSliceValue(rhs);
                                        pinValueInPools(detached);
                                        const bool detBarrier =
                                            detached.rawTag() == ValueTag::Struct &&
                                            !detached.u_.structVal.isInline &&
                                            detached.u_.structVal.storage != nullptr;
                                        if (detBarrier &&
                                            (oce.view.needsRememberedBarrier || gcActive || state.heap->phase() != GcPhase::Idle)) {
                                            state.heap->recordObjectWriteBarrier(oce.ref, detached);
                                            oce.view.needsRememberedBarrier = false;
                                        }
                                        localExecuted += spanCost;
                                        oce.view.fields[fIdx] = std::move(detached);
                                        if (RS_FLAT_UNLIKELY(state.heap->hasDirtyTracking())) {
                                            state.heap->notifyFieldMutated(ref, fIdx);
                                        }
                                        RS_FLAT_DISPATCH_NEXT();
                                    }
                                    if (needsPin) pinValueInPools(rhs);
                                    if (needsBarrier &&
                                        (oce.view.needsRememberedBarrier || gcActive || state.heap->phase() != GcPhase::Idle)) {
                                        state.heap->recordObjectWriteBarrier(oce.ref, rhs);
                                        oce.view.needsRememberedBarrier = false;
                                    }
                                    localExecuted += spanCost;
                                    oce.view.fields[fIdx] = rhs;
                                    if (RS_FLAT_UNLIKELY(state.heap->hasDirtyTracking())) {
                                        state.heap->notifyFieldMutated(ref, fIdx);
                                    }
                                    RS_FLAT_DISPATCH_NEXT();
                                }
                            }
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(LoadStructFieldFast) {
                const auto* type = typeDescriptor(*location.module, ip->auxImm32);
                const auto& recv = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(type != nullptr && structValueTypeId(recv) == type->id)) {
                    const auto expectedFieldType = type->fields[ip->aux0Slot].type;
                    const auto expectedFieldTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                    if (auto inlinedField = loadInlineStructField(recv, ip->aux0Slot);
                        RS_FLAT_LIKELY(inlinedField.has_value() &&
                        valueType(*inlinedField) == expectedFieldType &&
                        (expectedFieldTid == 0 || structValueTypeId(*inlinedField) == expectedFieldTid))) {
                        if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                        frameBase[ip->dstSlot] = std::move(*inlinedField);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatGenericFallback;
            }

            RS_FLAT_CASE(StoreStructFieldFast) {
                const auto* type = typeDescriptor(*location.module, ip->auxImm32);
                const auto& recv = frameBase[ip->src0Slot];
                const auto& rhs = frameBase[ip->src1Slot];
                const auto expectedFieldTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (RS_FLAT_LIKELY(type != nullptr && structValueTypeId(recv) == type->id &&
                    valueType(rhs) == type->fields[ip->aux0Slot].type &&
                    (expectedFieldTid == 0 || structValueTypeId(rhs) == expectedFieldTid))) {
                    if (auto updated = storeInlineStructField(recv, ip->aux0Slot, rhs);
                        RS_FLAT_LIKELY(updated.has_value())) {
                        if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                        frameBase[ip->dstSlot] = std::move(*updated);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatGenericFallback;
            }

            RS_FLAT_CASE(ConstantBool) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot] = (ip->integerImmediate != 0);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConstantLong) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot].storeLong(ip->integerImmediate);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ArrayLengthFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY((ce.valid &&
                                    arrVal.rawTag() == ValueTag::ObjectRef &&
                                    arrVal.u_.objectRef == ce.ref) ||
                                   bindArrayCacheSlot(arrVal, ce))) {
                    RS_FLAT_SPAN_CHECK();
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(ce.view.length));
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (const auto* ref = std::get_if<ObjectRef>(&arrVal); RS_FLAT_LIKELY(ref != nullptr && state.heap)) {
                    if (const auto len = state.heap->arrayLength(*ref);
                        RS_FLAT_LIKELY(len.has_value() &&
                        *len <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))) {
                        RS_FLAT_SPAN_CHECK();
                        localExecuted += spanCost;
                        frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(*len));
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(ConvertIntToLongFast) {
                const auto* vPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeLong(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertIntToDoubleFast) {
                const auto* vPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = static_cast<double>(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertLongToDoubleFast) {
                const auto* vPtr = std::get_if<LongValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = static_cast<double>(vPtr->value);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalDoubleConstStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                const auto* leftPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!leftPtr)) goto L_FlatSuperFallback;
                const double left = *leftPtr;
                double right = 0.0;
                std::memcpy(&right, &ip->integerImmediate, sizeof(double));
                double out = 0.0;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddDouble: out = left + right; break;
                case detail::QuickOpcode::SubtractDouble: out = left - right; break;
                case detail::QuickOpcode::MultiplyDouble: out = left * right; break;
                case detail::QuickOpcode::DivideDouble: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->dstSlot].storeDouble(left);
                frameBase[ip->aux0Slot].storeDouble(right);
                frameBase[ip->aux1Slot].storeDouble(out);
                frameBase[ip->storeSlot].storeDouble(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalDoubleConstStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                const auto* leftPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!leftPtr)) goto L_FlatSuperFallback;
                const double left = *leftPtr;
                double right = 0.0;
                std::memcpy(&right, &ip->integerImmediate, sizeof(double));
                double out = 0.0;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddDouble: out = left + right; break;
                case detail::QuickOpcode::SubtractDouble: out = left - right; break;
                case detail::QuickOpcode::MultiplyDouble: out = left * right; break;
                case detail::QuickOpcode::DivideDouble: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->storeSlot].storeDouble(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalDoubleLocalStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                const auto* leftPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                const auto* rightPtr = std::get_if<double>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!leftPtr || !rightPtr)) goto L_FlatSuperFallback;
                const double left = *leftPtr;
                const double right = *rightPtr;
                double out = 0.0;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddDouble: out = left + right; break;
                case detail::QuickOpcode::SubtractDouble: out = left - right; break;
                case detail::QuickOpcode::MultiplyDouble: out = left * right; break;
                case detail::QuickOpcode::DivideDouble: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->dstSlot].storeDouble(left);
                frameBase[ip->aux0Slot].storeDouble(right);
                frameBase[ip->aux1Slot].storeDouble(out);
                frameBase[ip->storeSlot].storeDouble(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalDoubleLocalStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                const auto* leftPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                const auto* rightPtr = std::get_if<double>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!leftPtr || !rightPtr)) goto L_FlatSuperFallback;
                const double left = *leftPtr;
                const double right = *rightPtr;
                double out = 0.0;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddDouble: out = left + right; break;
                case detail::QuickOpcode::SubtractDouble: out = left - right; break;
                case detail::QuickOpcode::MultiplyDouble: out = left * right; break;
                case detail::QuickOpcode::DivideDouble: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->storeSlot].storeDouble(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NegateIntUnchecked) {
                const auto* vPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = (*vPtr == std::numeric_limits<std::int32_t>::min())
                    ? *vPtr
                    : -(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NegateDouble) {
                const auto* vPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = -(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LogicalNot) {
                const auto* vPtr = std::get_if<bool>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = !(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NewArrayFast) {
                std::int64_t lenVal = 0;
                if (RS_FLAT_LIKELY(
                        frameBase[ip->src0Slot].tryGetInt64(lenVal) &&
                        lenVal >= 0 &&
                        static_cast<std::uint64_t>(lenVal) <=
                            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) &&
                        state.heap != nullptr)) {
                    RS_FLAT_SPAN_CHECK();
                    localExecuted += spanCost;
                    if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                    else gcAccounted = localExecuted;

                    const auto elemType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1));
                    const auto arrayTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                    Value initVal;
                    semantic::SymbolId elemTid = 0;
                    if (ip->aux0Slot == 1) {
                        switch (elemType) {
                        case semantic::PrimitiveType::Int: initVal.storeInt64(0); break;
                        case semantic::PrimitiveType::Float: initVal.storeFloat(0.0f); break;
                        case semantic::PrimitiveType::Double: initVal.storeDouble(0.0); break;
                        case semantic::PrimitiveType::Long: initVal.storeLong(0); break;
                        case semantic::PrimitiveType::Bool: initVal.storeBool(false); break;
                        case semantic::PrimitiveType::Object:
                            initVal = NullObject{};
                            elemTid = flatColdBase[ip - streamBase].bytecode->elementTypeId;
                            break;
                        case semantic::PrimitiveType::String: initVal = NullString{}; break;
                        default:
                            initVal = defaultValue(*location.module, elemType, 0);
                            break;
                        }
                    } else if (ip->aux0Slot >= 2 && ip->aux0Slot <= 10) {
                        elemTid = flatColdBase[ip - streamBase].bytecode->elementTypeId;
                        switch (ip->aux0Slot) {
                        case 2: initVal = FixedValue{}; break;
                        case 3: initVal = Fixed16Value{}; break;
                        case 4: initVal = Vec2Value{}; break;
                        case 5: initVal = Vec3Value{}; break;
                        case 6: initVal = QuatValue{}; break;
                        case 7: initVal = Fixed64Value{}; break;
                        case 8: initVal = Vec2fValue{}; break;
                        case 9: initVal = Vec3fValue{}; break;
                        default: initVal = QuatfValue{}; break;
                        }
                    } else if (ip->aux0Slot == 11) {
                        elemTid = flatColdBase[ip - streamBase].bytecode->elementTypeId;
                        initVal = Value{};
                    } else {
                        const auto& inst = *flatColdBase[ip - streamBase].bytecode;
                        elemTid = inst.elementTypeId;
                        initVal = defaultValue(*location.module, elemType, elemTid);
                    }

                    bool slotsMoved = false;
                    RuntimeError allocErr;
                    const auto ref = state.heap->allocateTypedArray(
                        arrayTid,
                        elemType,
                        elemTid,
                        static_cast<std::size_t>(lenVal),
                        std::move(initVal),
                        &allocErr,
                        &slotsMoved);
                    if (RS_FLAT_LIKELY(ref.has_value())) {
                        if (RS_FLAT_UNLIKELY(slotsMoved)) {
                            invalidateAllFlatCaches();
                        }
                        if (gcBudgetEnabled) {
                            gcActive = (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
                        }
                        frameBase[ip->dstSlot] = *ref;
                        auto& ce = arrayCache[ip->dstSlot & 3u];
                        if (state.heap->tryBindPackedArrayFastView(*ref, ce.packedView)) {
                            if (RS_FLAT_UNLIKELY(state.telemetry != nullptr)) {
                                state.telemetry->noteCompactHeapObject(*ref);
                            }
                            ce.ref = *ref;
                            ce.view = {};
                            ce.view.length = ce.packedView.length;
                            ce.view.elementType = ce.packedView.elementType;
                            ce.view.elementTypeId = ce.packedView.elementTypeId;
                            ce.isPacked = true;
                            ce.valid = true;
                        } else if (state.heap->tryBindFlatStructArrayFastView(*ref, ce.flatStructView)) {
                            ce.ref = *ref;
                            ce.view = {};
                            ce.view.length = ce.flatStructView.length;
                            ce.view.elementType = semantic::PrimitiveType::Struct;
                            ce.view.elementTypeId = ce.flatStructView.elementTypeId;
                            ce.isPacked = false;
                            ce.isFlatStruct = true;
                            ce.valid = true;
                        } else if (state.heap->tryBindArrayFastView(*ref, ce.view)) {
                            ce.ref = *ref;
                            ce.isPacked = false;
                            ce.isFlatStruct = false;
                            ce.valid = true;
                        }
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    if (spanCost > 1u) {
                        localExecuted -= spanCost;
                        goto L_FlatSuperFallback;
                    }
                    state.executed = localExecuted;
                    fail(state,
                        allocErr.code == ErrorCode::None ? ErrorCode::OutOfMemory : allocErr.code,
                        allocErr.message.empty() ? "managed array allocation failed" : allocErr.message);
                    goto L_FlatError;
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(AddFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(aVal + bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(aVal - bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(aVal * bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(DivFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(aVal / bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal < bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessOrEqualFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal <= bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal > bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterOrEqualFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal >= bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(EqualFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal == bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NotEqualFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal != bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NegateFloat) {
                const auto* vPtr = std::get_if<FloatValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(-vPtr->value);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertIntToFloatFast) {
                const auto* vPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(static_cast<float>(*vPtr));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertDoubleToFloatFast) {
                const auto* vPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                const double d = *vPtr;
                if (ip->checked1()) {
                    if (RS_FLAT_UNLIKELY(!(d >= -static_cast<double>(std::numeric_limits<float>::max()) &&
                                           d <= static_cast<double>(std::numeric_limits<float>::max())))) {
                        goto L_FlatMathFallback;
                    }
                }
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(static_cast<float>(d));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertFloatToIntFast) {
                const auto* vPtr = std::get_if<FloatValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                const double d = static_cast<double>(vPtr->value);
                if (RS_FLAT_UNLIKELY(!(d >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
                                       d <= static_cast<double>(std::numeric_limits<std::int32_t>::max())))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = static_cast<std::int64_t>(d);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertFloatToDoubleFast) {
                const auto* vPtr = std::get_if<FloatValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = static_cast<double>(vPtr->value);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertLongToIntFast) {
                const auto* vPtr = std::get_if<LongValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                const std::int64_t val = vPtr->value;
                if (ip->checked1()) {
                    if (RS_FLAT_UNLIKELY(val < std::numeric_limits<std::int32_t>::min() ||
                                         val > std::numeric_limits<std::int32_t>::max())) {
                        goto L_FlatMathFallback;
                    }
                    RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                    frameBase[ip->dstSlot] = val;
                } else {
                    RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                    frameBase[ip->dstSlot] = wrapInt32(val);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperConstFloat) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float fImm = 0.0f;
                std::memcpy(&fImm, &ip->integerImmediate, sizeof(float));
                localExecuted += 2;
                frameBase[ip->dstSlot].storeFloat(fImm);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperConstFloatStoreLocal) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float fImm = 0.0f;
                std::memcpy(&fImm, &ip->integerImmediate, sizeof(float));
                localExecuted += 3;
                frameBase[ip->storeSlot].storeFloat(fImm);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatLocalStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->dstSlot].storeFloat(left);
                frameBase[ip->aux0Slot].storeFloat(right);
                frameBase[ip->aux1Slot].storeFloat(out);
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatLocalStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatReg) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                frameBase[ip->dstSlot].storeFloat(left);
                frameBase[ip->aux1Slot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatRegElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                frameBase[ip->aux1Slot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatRegStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 3;
                frameBase[ip->dstSlot].storeFloat(left);
                frameBase[ip->aux1Slot].storeFloat(out);
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatRegStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 3;
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryRegFloatStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NewStructFast) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                switch (ip->aux0Slot) {
                case 1: frameBase[ip->dstSlot] = FixedValue{}; break;
                case 2: frameBase[ip->dstSlot] = Fixed16Value{}; break;
                case 3: frameBase[ip->dstSlot] = Vec2Value{}; break;
                case 4: frameBase[ip->dstSlot] = Vec3Value{}; break;
                case 5: frameBase[ip->dstSlot] = QuatValue{}; break;
                case 6: frameBase[ip->dstSlot] = Fixed64Value{}; break;
                case 7: frameBase[ip->dstSlot] = Vec2fValue{}; break;
                case 8: frameBase[ip->dstSlot] = Vec3fValue{}; break;
                case 9: frameBase[ip->dstSlot] = QuatfValue{}; break;
                default:
                    --localExecuted;
                    goto L_FlatGenericFallback;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadFixedRawFast) {
                const auto* fx = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!fx)) goto L_FlatMathFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += cost;
                frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(fx->raw));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreFixedRawFast) {
                const auto* fx = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                const auto* rawPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!fx || !rawPtr)) goto L_FlatGenericFallback;
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(*rawPtr));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadVec3FieldFast) {
                const auto* v3 = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!v3)) goto L_FlatMathFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += cost;
                const std::int32_t raw = (ip->aux0Slot == 0)
                    ? v3->x
                    : ((ip->aux0Slot == 1) ? v3->y : v3->z);
                frameBase[ip->dstSlot].storeFixed(raw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreVec3FieldFast) {
                const auto* v3 = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                const auto* fx = std::get_if<FixedValue>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!v3 || !fx)) goto L_FlatGenericFallback;
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                Vec3Value out = *v3;
                if (ip->aux0Slot == 0) out.x = fx->raw;
                else if (ip->aux0Slot == 1) out.y = fx->raw;
                else out.z = fx->raw;
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadQuatFieldFast) {
                QuatValue q{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(q))) goto L_FlatMathFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += cost;
                std::int32_t raw = 0;
                switch (ip->aux0Slot) {
                case 0: raw = q.x; break;
                case 1: raw = q.y; break;
                case 2: raw = q.z; break;
                default: raw = q.w; break;
                }
                frameBase[ip->dstSlot].storeFixed(raw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreQuatFieldFast) {
                QuatValue out{};
                std::int32_t fxRaw = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(out) ||
                                     !frameBase[ip->src1Slot].tryGetFixed(fxRaw))) {
                    goto L_FlatGenericFallback;
                }
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                switch (ip->aux0Slot) {
                case 0: out.x = fxRaw; break;
                case 1: out.y = fxRaw; break;
                case 2: out.z = fxRaw; break;
                default: out.w = fxRaw; break;
                }
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            #define RS_FLAT_MATH_CALL_ACCOUNTING() \
                const std::uint64_t mathCost = ip->aux0Slot; \
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu; \
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu; \
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || \
                                     mathCost > budgetLimit - localExecuted || \
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) { \
                    goto L_FlatMathFallback; \
                } \
                localExecuted += mathCost; \
                state.statistics.functionCalls += mathCalls; \
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) { \
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth; \
                } \
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc()

            RS_FLAT_CASE(MathFixedConst) {
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(ip->integerImmediate));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedFromRaw) {
                const auto* aPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !isValidInt32Payload(*aPtr))) goto L_FlatMathFallback;
                const std::int32_t raw = static_cast<std::int32_t>(*aPtr);
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(raw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedFromInt) {
                const auto* aPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || *aPtr < -8388608 || *aPtr > 8388607)) {
                    goto L_FlatMathFallback;
                }
                const std::int32_t scaled = static_cast<std::int32_t>(*aPtr * 256);
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(scaled);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedRatio) {
                const auto* numPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                const auto* denPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(
                        !numPtr || !denPtr || *denPtr == 0 ||
                        !isValidInt32Payload(*numPtr) || !isValidInt32Payload(*denPtr))) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t res = (*numPtr * 256) / *denPtr;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedNeg) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || aPtr->raw == std::numeric_limits<std::int32_t>::min())) {
                    goto L_FlatMathFallback;
                }
                const std::int32_t neg = -aPtr->raw;
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(neg);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedAdd) {
                std::int32_t aRaw = 0, bRaw = 0;
                const bool okFx = frameBase[ip->src0Slot].tryGetFixed(aRaw) && frameBase[ip->src1Slot].tryGetFixed(bRaw);
                if (RS_FLAT_UNLIKELY(!okFx)) goto L_FlatMathFallback;
                const std::int64_t res =
                    static_cast<std::int64_t>(aRaw) + static_cast<std::int64_t>(bRaw);
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedSub) {
                std::int32_t aRaw = 0, bRaw = 0;
                const bool okFx = frameBase[ip->src0Slot].tryGetFixed(aRaw) && frameBase[ip->src1Slot].tryGetFixed(bRaw);
                if (RS_FLAT_UNLIKELY(!okFx)) goto L_FlatMathFallback;
                const std::int64_t res =
                    static_cast<std::int64_t>(aRaw) - static_cast<std::int64_t>(bRaw);
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedMul) {
                std::int32_t aRaw = 0, bRaw = 0;
                const bool okFx = frameBase[ip->src0Slot].tryGetFixed(aRaw) && frameBase[ip->src1Slot].tryGetFixed(bRaw);
                if (RS_FLAT_UNLIKELY(!okFx)) goto L_FlatMathFallback;
                const std::int64_t res =
                    (static_cast<std::int64_t>(aRaw) * static_cast<std::int64_t>(bRaw)) / 256;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedMulInt) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr || !isValidInt32Payload(*bPtr))) goto L_FlatMathFallback;
                const std::int64_t res = static_cast<std::int64_t>(aPtr->raw) * (*bPtr);
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedDiv) {
                std::int32_t aRaw = 0, bRaw = 0;
                const bool okFx = frameBase[ip->src0Slot].tryGetFixed(aRaw) && frameBase[ip->src1Slot].tryGetFixed(bRaw);
                if (RS_FLAT_UNLIKELY(!okFx || bRaw == 0)) goto L_FlatMathFallback;
                const std::int64_t res =
                    (static_cast<std::int64_t>(aRaw) * 256) / static_cast<std::int64_t>(bRaw);
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedDivInt) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr || *bPtr == 0 || !isValidInt32Payload(*bPtr) ||
                                     (aPtr->raw == std::numeric_limits<std::int32_t>::min() && *bPtr == -1))) {
                    goto L_FlatMathFallback;
                }
                const std::int32_t res = static_cast<std::int32_t>(aPtr->raw / *bPtr);
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedSin) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr)) goto L_FlatMathFallback;
                std::uint64_t extraInst = 0;
                std::uint64_t branches = 0;
                const std::int32_t outRaw = fastEvalFixedSin(
                    aPtr->raw,
                    static_cast<std::uint64_t>(ip->integerImmediate),
                    extraInst,
                    branches);
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + extraInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeFixed(outRaw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedCos) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr)) goto L_FlatMathFallback;
                const std::int64_t shifted = static_cast<std::int64_t>(aPtr->raw) + 402LL;
                if (RS_FLAT_UNLIKELY(!isValidInt32Payload(shifted))) goto L_FlatMathFallback;
                std::uint64_t extraInst = 0;
                std::uint64_t branches = 0;
                const std::int32_t outRaw = fastEvalFixedSin(
                    static_cast<std::int32_t>(shifted),
                    static_cast<std::uint64_t>(ip->integerImmediate),
                    extraInst,
                    branches);
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + extraInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeFixed(outRaw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedSqrt) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr)) goto L_FlatMathFallback;
                const std::size_t baseCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                const auto packed = static_cast<std::uint64_t>(ip->integerImmediate);
                const std::uint64_t loopIterCost = packed & 0xFFu;
                const std::uint64_t negCost = (packed >> 8u) & 0xFFFFu;
                std::uint64_t mathCost = 0;
                std::size_t mathCalls = 0;
                std::uint64_t branches = 0;
                std::int32_t outRaw = 0;
                if (aPtr->raw <= 0) {
                    mathCost = negCost;
                    mathCalls = baseCalls - 1u;
                    branches = 1;
                } else {
                    std::uint64_t loopInst = 0;
                    std::uint64_t sqBr = 0;
                    const std::int64_t ans = fastEvalSqrt64Pos(
                        static_cast<std::int64_t>(aPtr->raw) * 256LL,
                        loopIterCost,
                        loopInst,
                        sqBr);
                    outRaw = static_cast<std::int32_t>(ans);
                    mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                    mathCalls = baseCalls;
                    branches = 1u + sqBr;
                }
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeFixed(outRaw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathSqrt64) {
                const auto* aPtr = std::get_if<LongValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr)) goto L_FlatMathFallback;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                const auto packed = static_cast<std::uint64_t>(ip->integerImmediate);
                const std::uint64_t loopIterCost = packed & 0xFFu;
                const std::uint64_t negCost = (packed >> 8u) & 0xFFFFu;
                std::uint64_t mathCost = 0;
                std::uint64_t branches = 0;
                std::int64_t outVal = 0;
                if (aPtr->value <= 0) {
                    mathCost = negCost;
                    branches = 1;
                } else {
                    std::uint64_t loopInst = 0;
                    outVal = fastEvalSqrt64Pos(aPtr->value, loopIterCost, loopInst, branches);
                    mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                }
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeLong(outVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Const) {
                RS_FLAT_MATH_CALL_ACCOUNTING();
                const auto c = static_cast<std::int32_t>(ip->integerImmediate);
                frameBase[ip->dstSlot].storeVec3(c, c, c);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Raw) {
                const auto* xPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                const auto* yPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                const auto* zPtr = std::get_if<std::int64_t>(&frameBase[ip->src2Slot]);
                if (RS_FLAT_UNLIKELY(
                        !xPtr || !yPtr || !zPtr ||
                        !isValidInt32Payload(*xPtr) ||
                        !isValidInt32Payload(*yPtr) ||
                        !isValidInt32Payload(*zPtr))) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(*xPtr),
                    static_cast<std::int32_t>(*yPtr),
                    static_cast<std::int32_t>(*zPtr)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Of) {
                const auto* xPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                const auto* yPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                const auto* zPtr = std::get_if<std::int64_t>(&frameBase[ip->src2Slot]);
                if (RS_FLAT_UNLIKELY(
                        !xPtr || !yPtr || !zPtr ||
                        *xPtr < -8388608 || *xPtr > 8388607 ||
                        *yPtr < -8388608 || *yPtr > 8388607 ||
                        *zPtr < -8388608 || *zPtr > 8388607)) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(*xPtr * 256),
                    static_cast<std::int32_t>(*yPtr * 256),
                    static_cast<std::int32_t>(*zPtr * 256)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Neg) {
                const auto* vPtr = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr ||
                                     vPtr->x == std::numeric_limits<std::int32_t>::min() ||
                                     vPtr->y == std::numeric_limits<std::int32_t>::min() ||
                                     vPtr->z == std::numeric_limits<std::int32_t>::min())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{-vPtr->x, -vPtr->y, -vPtr->z};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Add) {
                Vec3Value aVal{}, bVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(bVal))) goto L_FlatMathFallback;
                const std::int64_t rx = static_cast<std::int64_t>(aVal.x) + bVal.x;
                const std::int64_t ry = static_cast<std::int64_t>(aVal.y) + bVal.y;
                const std::int64_t rz = static_cast<std::int64_t>(aVal.z) + bVal.z;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Sub) {
                Vec3Value aVal{}, bVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(bVal))) goto L_FlatMathFallback;
                const std::int64_t rx = static_cast<std::int64_t>(aVal.x) - bVal.x;
                const std::int64_t ry = static_cast<std::int64_t>(aVal.y) - bVal.y;
                const std::int64_t rz = static_cast<std::int64_t>(aVal.z) - bVal.z;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3MulFixed) {
                Vec3Value vVal{};
                std::int32_t sRaw = 0;
                const bool okOps = frameBase[ip->src0Slot].tryGetVec3(vVal) && frameBase[ip->src1Slot].tryGetFixed(sRaw);
                if (RS_FLAT_UNLIKELY(!okOps)) goto L_FlatMathFallback;
                const std::int64_t sr = sRaw;
                const std::int64_t rx = (static_cast<std::int64_t>(vVal.x) * sr) / 256;
                const std::int64_t ry = (static_cast<std::int64_t>(vVal.y) * sr) / 256;
                const std::int64_t rz = (static_cast<std::int64_t>(vVal.z) * sr) / 256;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3MulInt) {
                const auto* vPtr = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                const auto* sPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr || !sPtr || !isValidInt32Payload(*sPtr))) goto L_FlatMathFallback;
                const std::int64_t s = *sPtr;
                const std::int64_t rx = static_cast<std::int64_t>(vPtr->x) * s;
                const std::int64_t ry = static_cast<std::int64_t>(vPtr->y) * s;
                const std::int64_t rz = static_cast<std::int64_t>(vPtr->z) * s;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3DivFixed) {
                Vec3Value vVal{};
                std::int32_t sRaw = 0;
                const bool okOps = frameBase[ip->src0Slot].tryGetVec3(vVal) && frameBase[ip->src1Slot].tryGetFixed(sRaw);
                if (RS_FLAT_UNLIKELY(!okOps || sRaw == 0)) goto L_FlatMathFallback;
                const std::int64_t sr = sRaw;
                const std::int64_t rx = (static_cast<std::int64_t>(vVal.x) * 256) / sr;
                const std::int64_t ry = (static_cast<std::int64_t>(vVal.y) * 256) / sr;
                const std::int64_t rz = (static_cast<std::int64_t>(vVal.z) * 256) / sr;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3DivInt) {
                const auto* vPtr = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                const auto* sPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr || !sPtr || *sPtr == 0 || !isValidInt32Payload(*sPtr))) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t s = *sPtr;
                if (RS_FLAT_UNLIKELY(s == -1 &&
                                     (vPtr->x == std::numeric_limits<std::int32_t>::min() ||
                                      vPtr->y == std::numeric_limits<std::int32_t>::min() ||
                                      vPtr->z == std::numeric_limits<std::int32_t>::min()))) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(vPtr->x / s),
                    static_cast<std::int32_t>(vPtr->y / s),
                    static_cast<std::int32_t>(vPtr->z / s)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Dot) {
                Vec3Value aVal{}, bVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(bVal))) goto L_FlatMathFallback;
                const std::int64_t px = (static_cast<std::int64_t>(aVal.x) * bVal.x) / 256;
                const std::int64_t py = (static_cast<std::int64_t>(aVal.y) * bVal.y) / 256;
                const std::int64_t pz = (static_cast<std::int64_t>(aVal.z) * bVal.z) / 256;
                if (RS_FLAT_UNLIKELY(px < std::numeric_limits<std::int32_t>::min() || px > std::numeric_limits<std::int32_t>::max() ||
                                     py < std::numeric_limits<std::int32_t>::min() || py > std::numeric_limits<std::int32_t>::max() ||
                                     pz < std::numeric_limits<std::int32_t>::min() || pz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t xy = px + py;
                if (RS_FLAT_UNLIKELY(xy < std::numeric_limits<std::int32_t>::min() || xy > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t xyz = xy + pz;
                if (RS_FLAT_UNLIKELY(xyz < std::numeric_limits<std::int32_t>::min() || xyz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(xyz));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Cross) {
                Vec3Value aVal{}, bVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(bVal))) goto L_FlatMathFallback;
                const std::int64_t p1 = (static_cast<std::int64_t>(aVal.y) * bVal.z) / 256;
                const std::int64_t p2 = (static_cast<std::int64_t>(aVal.z) * bVal.y) / 256;
                const std::int64_t p3 = (static_cast<std::int64_t>(aVal.z) * bVal.x) / 256;
                const std::int64_t p4 = (static_cast<std::int64_t>(aVal.x) * bVal.z) / 256;
                const std::int64_t p5 = (static_cast<std::int64_t>(aVal.x) * bVal.y) / 256;
                const std::int64_t p6 = (static_cast<std::int64_t>(aVal.y) * bVal.x) / 256;
                if (RS_FLAT_UNLIKELY(p1 < std::numeric_limits<std::int32_t>::min() || p1 > std::numeric_limits<std::int32_t>::max() ||
                                     p2 < std::numeric_limits<std::int32_t>::min() || p2 > std::numeric_limits<std::int32_t>::max() ||
                                     p3 < std::numeric_limits<std::int32_t>::min() || p3 > std::numeric_limits<std::int32_t>::max() ||
                                     p4 < std::numeric_limits<std::int32_t>::min() || p4 > std::numeric_limits<std::int32_t>::max() ||
                                     p5 < std::numeric_limits<std::int32_t>::min() || p5 > std::numeric_limits<std::int32_t>::max() ||
                                     p6 < std::numeric_limits<std::int32_t>::min() || p6 > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t cx = p1 - p2;
                const std::int64_t cy = p3 - p4;
                const std::int64_t cz = p5 - p6;
                if (RS_FLAT_UNLIKELY(cx < std::numeric_limits<std::int32_t>::min() || cx > std::numeric_limits<std::int32_t>::max() ||
                                     cy < std::numeric_limits<std::int32_t>::min() || cy > std::numeric_limits<std::int32_t>::max() ||
                                     cz < std::numeric_limits<std::int32_t>::min() || cz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(cx),
                    static_cast<std::int32_t>(cy),
                    static_cast<std::int32_t>(cz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec2Add) {
                const auto* aPtr = std::get_if<Vec2Value>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<Vec2Value>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr)) goto L_FlatMathFallback;
                const std::int64_t rx = static_cast<std::int64_t>(aPtr->x) + bPtr->x;
                const std::int64_t ry = static_cast<std::int64_t>(aPtr->y) + bPtr->y;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec2Value out{static_cast<std::int32_t>(rx), static_cast<std::int32_t>(ry)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec2(out.x, out.y);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec2Sub) {
                const auto* aPtr = std::get_if<Vec2Value>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<Vec2Value>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr)) goto L_FlatMathFallback;
                const std::int64_t rx = static_cast<std::int64_t>(aPtr->x) - bPtr->x;
                const std::int64_t ry = static_cast<std::int64_t>(aPtr->y) - bPtr->y;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec2Value out{static_cast<std::int32_t>(rx), static_cast<std::int32_t>(ry)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec2(out.x, out.y);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec2MulFixed) {
                const auto* vPtr = std::get_if<Vec2Value>(&frameBase[ip->src0Slot]);
                const auto* sPtr = std::get_if<FixedValue>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr || !sPtr)) goto L_FlatMathFallback;
                const std::int64_t sr = sPtr->raw;
                const std::int64_t rx = (static_cast<std::int64_t>(vPtr->x) * sr) / 256;
                const std::int64_t ry = (static_cast<std::int64_t>(vPtr->y) * sr) / 256;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec2Value out{static_cast<std::int32_t>(rx), static_cast<std::int32_t>(ry)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec2(out.x, out.y);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec2Dot) {
                const auto* aPtr = std::get_if<Vec2Value>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<Vec2Value>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr)) goto L_FlatMathFallback;
                const std::int64_t px = (static_cast<std::int64_t>(aPtr->x) * bPtr->x) / 256;
                const std::int64_t py = (static_cast<std::int64_t>(aPtr->y) * bPtr->y) / 256;
                if (RS_FLAT_UNLIKELY(px < std::numeric_limits<std::int32_t>::min() || px > std::numeric_limits<std::int32_t>::max() ||
                                     py < std::numeric_limits<std::int32_t>::min() || py > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t xy = px + py;
                if (RS_FLAT_UNLIKELY(xy < std::numeric_limits<std::int32_t>::min() || xy > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(xy));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatIdentity) {
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeQuat(QuatValue{0, 0, 0, 256});
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatDot) {
                QuatValue a{}, b{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b))) {
                    goto L_FlatMathFallback;
                }
                std::int32_t dotRaw = 0;
                if (RS_FLAT_UNLIKELY(!fastQuatDot(a, b, dotRaw))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(dotRaw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatMul) {
                QuatValue a{}, b{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b))) {
                    goto L_FlatMathFallback;
                }
                QuatValue out{};
                std::int32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, s0 = 0, s1 = 0;
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(a.w, b.x, t0) && fastFixedMul(a.x, b.w, t1) && fastFixedAdd(t0, t1, s0) &&
                          fastFixedMul(a.y, b.z, t2) && fastFixedAdd(s0, t2, s1) &&
                          fastFixedMul(a.z, b.y, t3) && fastFixedSub(s1, t3, out.x)))) {
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(a.w, b.y, t0) && fastFixedMul(a.x, b.z, t1) && fastFixedSub(t0, t1, s0) &&
                          fastFixedMul(a.y, b.w, t2) && fastFixedAdd(s0, t2, s1) &&
                          fastFixedMul(a.z, b.x, t3) && fastFixedAdd(s1, t3, out.y)))) {
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(a.w, b.z, t0) && fastFixedMul(a.x, b.y, t1) && fastFixedAdd(t0, t1, s0) &&
                          fastFixedMul(a.y, b.x, t2) && fastFixedSub(s0, t2, s1) &&
                          fastFixedMul(a.z, b.w, t3) && fastFixedAdd(s1, t3, out.z)))) {
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(a.w, b.w, t0) && fastFixedMul(a.x, b.x, t1) && fastFixedSub(t0, t1, s0) &&
                          fastFixedMul(a.y, b.y, t2) && fastFixedSub(s0, t2, s1) &&
                          fastFixedMul(a.z, b.z, t3) && fastFixedSub(s1, t3, out.w)))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatRotate) {
                const auto* ref = std::get_if<ObjectRef>(&frameBase[ip->src0Slot]);
                const auto* vPtr = std::get_if<Vec3Value>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!ref || ref->kind != ObjectKind::Record || !vPtr || !state.heap)) {
                    goto L_FlatMathFallback;
                }
                semantic::SymbolId actualTid = 0;
                const Value* fPtr = state.heap->fieldGetFast(*ref, 0, &actualTid);
                QuatValue qVal{};
                if (RS_FLAT_UNLIKELY(!fPtr || !fPtr->tryGetQuat(qVal))) goto L_FlatMathFallback;
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(!fastQuatRotateVec3(qVal, *vPtr, out))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatRotateVec3) {
                QuatValue qVal{};
                Vec3Value vVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(qVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(vVal))) {
                    goto L_FlatMathFallback;
                }
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(!fastQuatRotateVec3(qVal, vVal, out))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatRotateBoxed) {
                QuatValue qVal{};
                Vec3Value vVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(qVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(vVal))) {
                    goto L_FlatSuperFallback;
                }
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(!fastQuatRotateVec3(qVal, vVal, out))) {
                    goto L_FlatSuperFallback;
                }
                const std::uint64_t mathCost = ip->aux0Slot;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->storeSlot != detail::InvalidCompactReg) {
                    frameBase[ip->storeSlot].storeQuat(qVal);
                }
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatNormalize) {
                QuatValue qVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(qVal))) goto L_FlatMathFallback;
                QuatValue out{};
                std::uint64_t loopInst = 0;
                std::uint64_t branches = 0;
                if (RS_FLAT_UNLIKELY(!fastQuatNormalize(
                        qVal,
                        static_cast<std::uint64_t>(ip->integerImmediate),
                        out,
                        loopInst,
                        branches))) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatNormalizedBoxed) {
                QuatValue qVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(qVal))) goto L_FlatSuperFallback;
                QuatValue out{};
                std::uint64_t loopInst = 0;
                std::uint64_t branches = 0;
                if (RS_FLAT_UNLIKELY(!fastQuatNormalize(
                        qVal,
                        static_cast<std::uint64_t>(ip->integerImmediate),
                        out,
                        loopInst,
                        branches))) {
                    goto L_FlatSuperFallback;
                }
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->storeSlot != detail::InvalidCompactReg) {
                    frameBase[ip->storeSlot].storeQuat(qVal);
                }
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatFromEuler) {
                const auto* pPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                const auto* yPtr = std::get_if<FixedValue>(&frameBase[ip->src1Slot]);
                const auto* rPtr = std::get_if<FixedValue>(&frameBase[ip->src2Slot]);
                if (RS_FLAT_UNLIKELY(!pPtr || !yPtr || !rPtr)) goto L_FlatMathFallback;
                const std::int32_t hp = pPtr->raw / 2;
                const std::int32_t hy = yPtr->raw / 2;
                const std::int32_t hr = rPtr->raw / 2;
                const std::uint64_t wrapExtra = static_cast<std::uint64_t>(ip->integerImmediate);
                std::uint64_t extraInst = 0;
                std::uint64_t branches = 0;
                const std::int32_t sp = fastEvalFixedSin(hp, wrapExtra, extraInst, branches);
                const std::int32_t cp = fastEvalFixedSin(hp + 402, wrapExtra, extraInst, branches);
                const std::int32_t sy = fastEvalFixedSin(hy, wrapExtra, extraInst, branches);
                const std::int32_t cy = fastEvalFixedSin(hy + 402, wrapExtra, extraInst, branches);
                const std::int32_t sr = fastEvalFixedSin(hr, wrapExtra, extraInst, branches);
                const std::int32_t cr = fastEvalFixedSin(hr + 402, wrapExtra, extraInst, branches);

                QuatValue out{};
                std::int32_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(cy, sp, a0) && fastFixedMul(a0, cr, a1) &&
                          fastFixedMul(sy, cp, b0) && fastFixedMul(b0, sr, b1) &&
                          fastFixedAdd(a1, b1, out.x) &&
                          fastFixedMul(sy, cp, a0) && fastFixedMul(a0, cr, a1) &&
                          fastFixedMul(cy, sp, b0) && fastFixedMul(b0, sr, b1) &&
                          fastFixedSub(a1, b1, out.y) &&
                          fastFixedMul(cy, cp, a0) && fastFixedMul(a0, sr, a1) &&
                          fastFixedMul(sy, sp, b0) && fastFixedMul(b0, cr, b1) &&
                          fastFixedSub(a1, b1, out.z) &&
                          fastFixedMul(cy, cp, a0) && fastFixedMul(a0, cr, a1) &&
                          fastFixedMul(sy, sp, b0) && fastFixedMul(b0, sr, b1) &&
                          fastFixedAdd(a1, b1, out.w)))) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + extraInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatLerp) {
                QuatValue a{}, target{};
                std::int32_t tRaw = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(target) ||
                                     !frameBase[ip->src2Slot].tryGetFixed(tRaw))) {
                    goto L_FlatMathFallback;
                }
                std::int32_t dotRaw = 0;
                if (RS_FLAT_UNLIKELY(!fastQuatDot(a, target, dotRaw))) {
                    goto L_FlatMathFallback;
                }
                const auto packed = static_cast<std::uint64_t>(ip->integerImmediate);
                const std::uint64_t loopIterCost = packed & 0xFFu;
                const std::uint64_t dotNegExtraCost = (packed >> 8u) & 0xFFFFu;
                const std::size_t dotNegExtraCalls = static_cast<std::size_t>((packed >> 24u) & 0xFFu);
                std::uint64_t extraInst = 0;
                std::size_t extraCalls = 0;
                std::uint64_t branches = 2; // Lerp dot<0 branch + Normalized len<=0 branch
                if (dotRaw < 0) {
                    if (RS_FLAT_UNLIKELY(
                            !(fastFixedNeg(target.x, target.x) &&
                              fastFixedNeg(target.y, target.y) &&
                              fastFixedNeg(target.z, target.z) &&
                              fastFixedNeg(target.w, target.w)))) {
                        goto L_FlatMathFallback;
                    }
                    extraInst += dotNegExtraCost;
                    extraCalls += dotNegExtraCalls;
                    ++branches; // bb1 unconditional jump -> bb2
                }
                QuatValue res{};
                std::int32_t dx = 0, dy = 0, dz = 0, dw = 0;
                std::int32_t sx = 0, sy = 0, sz = 0, sw = 0;
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedSub(target.x, a.x, dx) && fastFixedMul(dx, tRaw, sx) && fastFixedAdd(a.x, sx, res.x) &&
                          fastFixedSub(target.y, a.y, dy) && fastFixedMul(dy, tRaw, sy) && fastFixedAdd(a.y, sy, res.y) &&
                          fastFixedSub(target.z, a.z, dz) && fastFixedMul(dz, tRaw, sz) && fastFixedAdd(a.z, sz, res.z) &&
                          fastFixedSub(target.w, a.w, dw) && fastFixedMul(dw, tRaw, sw) && fastFixedAdd(a.w, sw, res.w)))) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t xx = static_cast<std::int64_t>(res.x) * res.x;
                const std::int64_t yy = static_cast<std::int64_t>(res.y) * res.y;
                const std::int64_t zz = static_cast<std::int64_t>(res.z) * res.z;
                const std::int64_t ww = static_cast<std::int64_t>(res.w) * res.w;
                if (RS_FLAT_UNLIKELY(
                        xx > std::numeric_limits<std::int64_t>::max() - yy ||
                        xx + yy > std::numeric_limits<std::int64_t>::max() - zz ||
                        xx + yy + zz > std::numeric_limits<std::int64_t>::max() - ww)) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t r2 = xx + yy + zz + ww;
                if (RS_FLAT_UNLIKELY(r2 <= 0)) {
                    goto L_FlatMathFallback;
                }
                std::uint64_t loopInst = 0;
                std::uint64_t sqBranches = 0;
                const std::int64_t len64 = fastEvalSqrt64Pos(r2, loopIterCost, loopInst, sqBranches);
                if (RS_FLAT_UNLIKELY(!isValidInt32Payload(len64) || len64 <= 0)) {
                    goto L_FlatMathFallback;
                }
                const auto lenRaw = static_cast<std::int32_t>(len64);
                QuatValue out{};
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedDiv(res.x, lenRaw, out.x) &&
                          fastFixedDiv(res.y, lenRaw, out.y) &&
                          fastFixedDiv(res.z, lenRaw, out.z) &&
                          fastFixedDiv(res.w, lenRaw, out.w)))) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t mathCost =
                    static_cast<std::uint64_t>(ip->aux0Slot) + extraInst + loopInst;
                const std::size_t mathCalls = (ip->aux1Slot & 0xFFu) + extraCalls;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches + sqBranches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperLoadVec3RawFast) {
                const auto* v3 = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!v3)) goto L_FlatSuperFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                const std::int32_t raw = (ip->aux0Slot == 0)
                    ? v3->x
                    : ((ip->aux0Slot == 1) ? v3->y : v3->z);
                frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(raw));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperLoadQuatRawFast) {
                QuatValue q{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(q))) goto L_FlatSuperFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                std::int32_t raw = 0;
                switch (ip->aux0Slot) {
                case 0: raw = q.x; break;
                case 1: raw = q.y; break;
                case 2: raw = q.z; break;
                default: raw = q.w; break;
                }
                frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(raw));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperFixedMulAdd) {
                std::int32_t aRaw = 0, bRaw = 0, cRaw = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFixed(aRaw) ||
                                     !frameBase[ip->src1Slot].tryGetFixed(bRaw) ||
                                     !frameBase[ip->src2Slot].tryGetFixed(cRaw))) goto L_FlatSuperFallback;
                std::int32_t prod = 0;
                std::int32_t sum = 0;
                if (RS_FLAT_UNLIKELY(!fastFixedMul(bRaw, cRaw, prod) ||
                                     !fastFixedAdd(aRaw, prod, sum))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(sum);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperVec3MulAdd) {
                Vec3Value a{}, b{};
                std::int32_t sr = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(a) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(b) ||
                                     !frameBase[ip->src2Slot].tryGetFixed(sr))) goto L_FlatSuperFallback;
                Vec3Value prod{};
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(b.x, sr, prod.x) &&
                          fastFixedMul(b.y, sr, prod.y) &&
                          fastFixedMul(b.z, sr, prod.z) &&
                          fastFixedAdd(a.x, prod.x, out.x) &&
                          fastFixedAdd(a.y, prod.y, out.y) &&
                          fastFixedAdd(a.z, prod.z, out.z)))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperVec3AddQuatRotate) {
                Vec3Value a{}, v{};
                QuatValue q{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(q) ||
                                     !frameBase[ip->src2Slot].tryGetVec3(v))) {
                    goto L_FlatSuperFallback;
                }
                Vec3Value rot{};
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(
                        !fastQuatRotateVec3(q, v, rot) ||
                        !(fastFixedAdd(a.x, rot.x, out.x) &&
                          fastFixedAdd(a.y, rot.y, out.y) &&
                          fastFixedAdd(a.z, rot.z, out.z)))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                if (ip->storeSlot != detail::InvalidCompactReg) {
                    frameBase[ip->storeSlot].storeQuat(q);
                }
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatMulNormalized) {
                QuatValue a{}, b{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b))) {
                    goto L_FlatSuperFallback;
                }
                QuatValue prod{};
                QuatValue out{};
                std::uint64_t loopInst = 0;
                std::uint64_t branches = 0;
                if (RS_FLAT_UNLIKELY(
                        !fastQuatMul(a, b, prod) ||
                        !fastQuatNormalize(
                            prod,
                            static_cast<std::uint64_t>(ip->integerImmediate),
                            out,
                            loopInst,
                            branches))) {
                    goto L_FlatSuperFallback;
                }
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->storeSlot != detail::InvalidCompactReg) {
                    frameBase[ip->storeSlot].storeQuat(prod);
                }
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatMulChain) {
                QuatValue a{}, b{}, c{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b) ||
                                     !frameBase[ip->src2Slot].tryGetQuat(c))) {
                    goto L_FlatSuperFallback;
                }
                QuatValue mid{};
                QuatValue out{};
                if (RS_FLAT_UNLIKELY(!fastQuatMul(a, b, mid) || !fastQuatMul(mid, c, out))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatMulRotateVec3) {
                QuatValue a{}, b{};
                Vec3Value v{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b) ||
                                     !frameBase[ip->src2Slot].tryGetVec3(v))) {
                    goto L_FlatSuperFallback;
                }
                QuatValue mid{};
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(!fastQuatMul(a, b, mid) || !fastQuatRotateVec3(mid, v, out))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            #undef RS_FLAT_MATH_CALL_ACCOUNTING
            #undef RS_FLAT_SPAN_CHECK

            RS_FLAT_CASE(SuperMoveParamStoreLocalElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                frameBase[ip->storeSlot] = frameBase[ip->src0Slot];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntConstStoreJump) {
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) goto L_FlatSuperFallback;
                if (RS_FLAT_UNLIKELY(!tryEvaluateQuickIntNoFail(
                        ip->binaryOpcode1, ip->checked1(), left, ip->integerImmediate, frameBase[ip->dstSlot]))) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.statistics.branchesTaken +=
                    ((ip->arithFlags & detail::FlatInstruction::FlagPreJump1) != 0u) ? 2u : 1u;
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntLocalStoreJump) {
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatSuperFallback;
                if (RS_FLAT_UNLIKELY(!tryEvaluateQuickIntNoFail(
                        ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->dstSlot]))) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.statistics.branchesTaken +=
                    ((ip->arithFlags & detail::FlatInstruction::FlagPreJump1) != 0u) ? 2u : 1u;
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegStoreJump) {
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatSuperFallback;
                if (RS_FLAT_UNLIKELY(!tryEvaluateQuickIntNoFail(
                        ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->dstSlot]))) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.statistics.branchesTaken +=
                    ((ip->arithFlags & detail::FlatInstruction::FlagPreJump1) != 0u) ? 2u : 1u;
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(SuperCheckNotNullCallPolledFast) {
                // Directly share CallPolledFast's 4-Way PIC + multi-arg + TrivialMethodKind fast path!
                goto L_CallPolledFastBody;
            }

            RS_FLAT_CASE(InlineCallEnter) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(state.stack.size() >= state.limits.recursionLimit)) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::RecursionLimitExceeded, "recursion limit exceeded");
                    goto L_FlatError;
                }
                ++state.statistics.functionCalls;
                if (state.stack.size() + 1 > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + 1;
                }
                if (RS_FLAT_UNLIKELY(state.telemetry != nullptr &&
                                     inlineTelemetryDepth < inlineTelemetryStack.size())) {
                    const auto* callBc = flatColdBase[ip - streamBase].bytecode;
                    const semantic::SymbolId calleeSym =
                        (callBc && location.module &&
                         callBc->index < location.module->functionReferences.size())
                            ? location.module->functionReferences[callBc->index].symbolId
                            : 0;
                    inlineTelemetryStack[inlineTelemetryDepth++] = InlineTelemetryFrame{
                        calleeSym,
                        localExecuted - 1u,
                        0u,
                    };
                }
                Value* const calleeBase = frameBase + ip->dstSlot;
                const std::uint32_t nArgs = ip->inlineArgCount;
                const std::uint32_t nLocals = ip->aux0Slot;
                if ((ip->arithFlags & detail::FlatInstruction::FlagFastParamPrologue) != 0u &&
                    RS_FLAT_LIKELY(2u * nArgs <= budgetLimit - localExecuted)) {
                    Value* const calleeLocals = calleeBase + nArgs;
                    if (nArgs >= 1u) calleeLocals[0] = frameBase[ip->src0Slot];
                    if (nArgs >= 2u) calleeLocals[1] = frameBase[ip->src1Slot];
                    if (nArgs >= 3u) calleeLocals[2] = frameBase[ip->src2Slot];
                    if (nArgs >= 4u) calleeLocals[3] = frameBase[ip->aux1Slot];
                    if (nLocals > nArgs) {
                        std::memset(static_cast<void*>(calleeLocals + nArgs), 0, (nLocals - nArgs) * sizeof(Value));
                    }
                    localExecuted += 2u * nArgs;
                    ip += 1u + nArgs;
                    RS_FLAT_DISPATCH_AT_IP();
                }
                if (nArgs >= 1u) calleeBase[0] = frameBase[ip->src0Slot];
                if (nArgs >= 2u) calleeBase[1] = frameBase[ip->src1Slot];
                if (nArgs >= 3u) calleeBase[2] = frameBase[ip->src2Slot];
                if (nArgs >= 4u) calleeBase[3] = frameBase[ip->aux1Slot];
                if (nLocals != 0u) {
                    std::memset(static_cast<void*>(calleeBase + nArgs), 0, nLocals * sizeof(Value));
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InlineReturnVoid) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(state.telemetry != nullptr && inlineTelemetryDepth > 0u)) {
                    const auto inlFrame = inlineTelemetryStack[--inlineTelemetryDepth];
                    const std::uint64_t totalSub = localExecuted >= inlFrame.enterLocalExecuted
                        ? (localExecuted - inlFrame.enterLocalExecuted)
                        : 0u;
                    const std::uint64_t selfInst = totalSub >= inlFrame.nestedChildInst
                        ? (totalSub - inlFrame.nestedChildInst)
                        : 0u;
                    if (selfInst > 0u && inlFrame.symbolId != 0) {
                        state.telemetry->recordFunctionInstructions(inlFrame.symbolId, selfInst);
                    }
                    if (inlineTelemetryDepth > 0u) {
                        inlineTelemetryStack[inlineTelemetryDepth - 1u].nestedChildInst += totalSub;
                    } else {
                        state.telemetryChildExecuted += totalSub;
                    }
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->integerImmediate != 0) {
                    const auto packedClear = static_cast<std::uint64_t>(ip->integerImmediate);
                    std::memset(static_cast<void*>(frameBase + (packedClear & 0xFFFFu)), 0,
                        static_cast<std::size_t>(packedClear >> 16u) * sizeof(Value));
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InlineReturnVoidJump) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(state.telemetry != nullptr && inlineTelemetryDepth > 0u)) {
                    const auto inlFrame = inlineTelemetryStack[--inlineTelemetryDepth];
                    const std::uint64_t totalSub = localExecuted >= inlFrame.enterLocalExecuted
                        ? (localExecuted - inlFrame.enterLocalExecuted)
                        : 0u;
                    const std::uint64_t selfInst = totalSub >= inlFrame.nestedChildInst
                        ? (totalSub - inlFrame.nestedChildInst)
                        : 0u;
                    if (selfInst > 0u && inlFrame.symbolId != 0) {
                        state.telemetry->recordFunctionInstructions(inlFrame.symbolId, selfInst);
                    }
                    if (inlineTelemetryDepth > 0u) {
                        inlineTelemetryStack[inlineTelemetryDepth - 1u].nestedChildInst += totalSub;
                    } else {
                        state.telemetryChildExecuted += totalSub;
                    }
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->integerImmediate != 0) {
                    const auto packedClear = static_cast<std::uint64_t>(ip->integerImmediate);
                    std::memset(static_cast<void*>(frameBase + (packedClear & 0xFFFFu)), 0,
                        static_cast<std::size_t>(packedClear >> 16u) * sizeof(Value));
                }
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(InlineReturnScalar) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(state.telemetry != nullptr && inlineTelemetryDepth > 0u)) {
                    const auto inlFrame = inlineTelemetryStack[--inlineTelemetryDepth];
                    const std::uint64_t totalSub = localExecuted >= inlFrame.enterLocalExecuted
                        ? (localExecuted - inlFrame.enterLocalExecuted)
                        : 0u;
                    const std::uint64_t selfInst = totalSub >= inlFrame.nestedChildInst
                        ? (totalSub - inlFrame.nestedChildInst)
                        : 0u;
                    if (selfInst > 0u && inlFrame.symbolId != 0) {
                        state.telemetry->recordFunctionInstructions(inlFrame.symbolId, selfInst);
                    }
                    if (inlineTelemetryDepth > 0u) {
                        inlineTelemetryStack[inlineTelemetryDepth - 1u].nestedChildInst += totalSub;
                    } else {
                        state.telemetryChildExecuted += totalSub;
                    }
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (RS_FLAT_UNLIKELY(ip->integerImmediate != 0)) {
                    const Value retVal = frameBase[ip->src0Slot];
                    const auto packedClear = static_cast<std::uint64_t>(ip->integerImmediate);
                    std::memset(static_cast<void*>(frameBase + (packedClear & 0xFFFFu)), 0,
                        static_cast<std::size_t>(packedClear >> 16u) * sizeof(Value));
                    if (ip->dstSlot != detail::InvalidCompactReg) {
                        frameBase[ip->dstSlot] = retVal;
                    }
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = frameBase[ip->src0Slot];
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InlineReturnScalarJump) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(state.telemetry != nullptr && inlineTelemetryDepth > 0u)) {
                    const auto inlFrame = inlineTelemetryStack[--inlineTelemetryDepth];
                    const std::uint64_t totalSub = localExecuted >= inlFrame.enterLocalExecuted
                        ? (localExecuted - inlFrame.enterLocalExecuted)
                        : 0u;
                    const std::uint64_t selfInst = totalSub >= inlFrame.nestedChildInst
                        ? (totalSub - inlFrame.nestedChildInst)
                        : 0u;
                    if (selfInst > 0u && inlFrame.symbolId != 0) {
                        state.telemetry->recordFunctionInstructions(inlFrame.symbolId, selfInst);
                    }
                    if (inlineTelemetryDepth > 0u) {
                        inlineTelemetryStack[inlineTelemetryDepth - 1u].nestedChildInst += totalSub;
                    } else {
                        state.telemetryChildExecuted += totalSub;
                    }
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (RS_FLAT_UNLIKELY(ip->integerImmediate != 0)) {
                    const Value retVal = frameBase[ip->src0Slot];
                    const auto packedClear = static_cast<std::uint64_t>(ip->integerImmediate);
                    std::memset(static_cast<void*>(frameBase + (packedClear & 0xFFFFu)), 0,
                        static_cast<std::size_t>(packedClear >> 16u) * sizeof(Value));
                    if (ip->dstSlot != detail::InvalidCompactReg) {
                        frameBase[ip->dstSlot] = retVal;
                    }
                    ip = streamBase + ip->trueTargetOffset;
                    RS_FLAT_DISPATCH_AT_IP();
                }
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = frameBase[ip->src0Slot];
                }
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(LoadElementTypedFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const auto expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1));
                const auto expectedTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (ce.isPacked) {
                    if (RS_FLAT_UNLIKELY(ce.packedView.elementType != expectedType ||
                                         ce.packedView.elementTypeId != expectedTid)) {
                        goto L_FlatMathFallback;
                    }
                    const std::uint64_t spanCost = ip->span;
                    if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                        if (spanCost > 1u) goto L_FlatSuperFallback;
                        goto L_FlatBudgetExceeded;
                    }
                    std::size_t uIdx = 0;
                    if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                        uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    } else {
                        std::int64_t idx = 0;
                        if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                             static_cast<std::uint64_t>(idx) >= ce.packedView.length)) {
                            goto L_FlatMathFallback;
                        }
                        uIdx = static_cast<std::size_t>(idx);
                    }
                    const auto* rawBytes = static_cast<const std::uint8_t*>(ce.packedView.rawData);
                    switch (ce.packedView.encoding) {
                    case PackedElemEncoding::Float64:
                        frameBase[ip->dstSlot].storeDouble(reinterpret_cast<const double*>(rawBytes)[uIdx]);
                        break;
                    case PackedElemEncoding::Float32:
                        frameBase[ip->dstSlot].storeFloat(reinterpret_cast<const float*>(rawBytes)[uIdx]);
                        break;
                    case PackedElemEncoding::Int32:
                        frameBase[ip->dstSlot].storeInt64(
                            static_cast<std::int64_t>(reinterpret_cast<const std::int32_t*>(rawBytes)[uIdx]));
                        break;
                    case PackedElemEncoding::UInt32:
                        frameBase[ip->dstSlot] = UIntValue{reinterpret_cast<const std::uint32_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::Int64:
                        frameBase[ip->dstSlot].storeLong(reinterpret_cast<const std::int64_t*>(rawBytes)[uIdx]);
                        break;
                    case PackedElemEncoding::UInt64:
                        frameBase[ip->dstSlot] = ULongValue{reinterpret_cast<const std::uint64_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::Int64Wide:
                        frameBase[ip->dstSlot].storeInt64(reinterpret_cast<const std::int64_t*>(rawBytes)[uIdx]);
                        break;
                    case PackedElemEncoding::Bool:
                        frameBase[ip->dstSlot].storeBool(rawBytes[uIdx] != 0u);
                        break;
                    case PackedElemEncoding::Byte:
                        frameBase[ip->dstSlot] = ByteValue{rawBytes[uIdx]};
                        break;
                    case PackedElemEncoding::SByte:
                        frameBase[ip->dstSlot] = SByteValue{static_cast<std::int8_t>(rawBytes[uIdx])};
                        break;
                    case PackedElemEncoding::Short:
                        frameBase[ip->dstSlot] = ShortValue{reinterpret_cast<const std::int16_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::UShort:
                        frameBase[ip->dstSlot] = UShortValue{reinterpret_cast<const std::uint16_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::Char:
                        frameBase[ip->dstSlot] = CharValue{reinterpret_cast<const std::uint16_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::EnumInline:
                        frameBase[ip->dstSlot] = EnumValue{
                            expectedTid,
                            static_cast<std::int64_t>(reinterpret_cast<const std::int32_t*>(rawBytes)[uIdx])};
                        break;
                    case PackedElemEncoding::EnumWide:
                        frameBase[ip->dstSlot] = EnumValue{
                            expectedTid,
                            reinterpret_cast<const std::int64_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::Fixed24_8:
                        frameBase[ip->dstSlot] = FixedValue{
                            reinterpret_cast<const std::int32_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::Fixed48_16:
                        frameBase[ip->dstSlot] = Fixed16Value{
                            reinterpret_cast<const std::int64_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::Fixed32_32:
                        frameBase[ip->dstSlot] = Fixed64Value{
                            reinterpret_cast<const std::int64_t*>(rawBytes)[uIdx]};
                        break;
                    case PackedElemEncoding::Vec2: {
                        Vec2Value v{};
                        std::memcpy(&v, rawBytes + uIdx * sizeof(Vec2Value), sizeof(v));
                        frameBase[ip->dstSlot] = v;
                        break;
                    }
                    case PackedElemEncoding::Vec3: {
                        Vec3Value v{};
                        std::memcpy(&v, rawBytes + uIdx * sizeof(Vec3Value), sizeof(v));
                        frameBase[ip->dstSlot] = v;
                        break;
                    }
                    case PackedElemEncoding::Quat: {
                        QuatValue v{};
                        std::memcpy(&v, rawBytes + uIdx * sizeof(QuatValue), sizeof(v));
                        frameBase[ip->dstSlot] = v;
                        break;
                    }
                    case PackedElemEncoding::Vec2f: {
                        Vec2fValue v{};
                        std::memcpy(&v, rawBytes + uIdx * sizeof(Vec2fValue), sizeof(v));
                        frameBase[ip->dstSlot] = v;
                        break;
                    }
                    case PackedElemEncoding::Vec3f: {
                        Vec3fValue v{};
                        std::memcpy(&v, rawBytes + uIdx * sizeof(Vec3fValue), sizeof(v));
                        frameBase[ip->dstSlot] = v;
                        break;
                    }
                    case PackedElemEncoding::Quatf: {
                        QuatfValue v{};
                        std::memcpy(&v, rawBytes + uIdx * sizeof(QuatfValue), sizeof(v));
                        frameBase[ip->dstSlot] = v;
                        break;
                    }
                    case PackedElemEncoding::BlittableStruct: {
                        if (execution->hasStructPayloads &&
                            state.flatBankId != 0u &&
                            ip->dstSlot < execution->slotStructSchemaIds.size()) {
                            const std::uint16_t sid = execution->slotStructSchemaIds[ip->dstSlot];
                            const std::uint16_t payOff = execution->slotPayloadOffsets[ip->dstSlot];
                            const auto* sc = sid != 0u ? getFlatStructSchema(sid) : nullptr;
                            if (sc != nullptr && payOff != detail::InvalidCompactReg &&
                                decodePackedStructToLeaves(
                                    rawBytes,
                                    uIdx,
                                    ce.packedView.blittableSchemaId,
                                    *sc,
                                    frameBase + payOff)) {
                                frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                                    expectedTid,
                                    state.flatBankId,
                                    static_cast<std::uint32_t>(frameOffset + payOff),
                                    sid);
                                break;
                            }
                        }
                        frameBase[ip->dstSlot] = readPackedStructElement(
                            rawBytes,
                            uIdx,
                            PackedElemEncoding::BlittableStruct,
                            expectedTid,
                            ce.packedView.blittableSchemaId);
                        break;
                    }
                    default:
                        goto L_FlatMathFallback;
                    }
                    localExecuted += spanCost;
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (ce.isFlatStruct) {
                    if (RS_FLAT_UNLIKELY(expectedType != semantic::PrimitiveType::Struct ||
                                         ce.flatStructView.elementTypeId != expectedTid)) {
                        goto L_FlatMathFallback;
                    }
                    const std::uint64_t spanCost = ip->span;
                    if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                        if (spanCost > 1u) goto L_FlatSuperFallback;
                        goto L_FlatBudgetExceeded;
                    }
                    std::size_t uIdx = 0;
                    if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                        uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    } else {
                        std::int64_t idx = 0;
                        if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                             static_cast<std::uint64_t>(idx) >= ce.flatStructView.length)) {
                            goto L_FlatMathFallback;
                        }
                        uIdx = static_cast<std::size_t>(idx);
                    }
                    const std::size_t stride = ce.flatStructView.flatStride;
                    const Value* elemSrc = ce.flatStructView.data + uIdx * stride;
                    if (RS_FLAT_LIKELY(execution->hasStructPayloads &&
                                       state.flatBankId != 0u &&
                                       ip->dstSlot < execution->slotStructSchemaIds.size() &&
                                       execution->slotStructSchemaIds[ip->dstSlot] == ce.flatStructView.flatSchemaId)) {
                        const std::uint16_t payOff = execution->slotPayloadOffsets[ip->dstSlot];
                        if (RS_FLAT_LIKELY(payOff != detail::InvalidCompactReg)) {
                            std::memcpy(
                                static_cast<void*>(frameBase + payOff),
                                static_cast<const void*>(elemSrc),
                                stride * sizeof(Value));
                            frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                                expectedTid,
                                state.flatBankId,
                                static_cast<std::uint32_t>(frameOffset + payOff),
                                ce.flatStructView.flatSchemaId);
                            localExecuted += spanCost;
                            RS_FLAT_DISPATCH_NEXT();
                        }
                    }
                    if (const auto* sc = getFlatStructSchema(ce.flatStructView.flatSchemaId)) {
                        frameBase[ip->dstSlot] = reconstructDetachedStructFromLeaves(*sc, elemSrc);
                        localExecuted += spanCost;
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(ce.view.elementType != expectedType ||
                                     ce.view.elementTypeId != expectedTid)) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot] = ce.view.data[uIdx];
                    RS_FLAT_DISPATCH_NEXT();
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = ce.view.data[static_cast<std::size_t>(idx)];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreElementPrimFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const auto expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1));
                const auto expectedTag = static_cast<ValueTag>(static_cast<std::uint8_t>(ip->binaryOpcode2));
                const Value& storeVal = frameBase[ip->src2Slot];
                if (ce.isPacked) {
                    if (RS_FLAT_UNLIKELY(ce.packedView.elementType != expectedType ||
                                         ce.packedView.elementTypeId != 0 ||
                                         storeVal.rawTag() != expectedTag)) {
                        goto L_FlatMathFallback;
                    }
                    const std::uint64_t spanCost = ip->span;
                    if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                        if (spanCost > 1u) goto L_FlatSuperFallback;
                        goto L_FlatBudgetExceeded;
                    }
                    std::size_t uIdx = 0;
                    if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                        uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    } else {
                        std::int64_t idx = 0;
                        if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                             static_cast<std::uint64_t>(idx) >= ce.packedView.length)) {
                            goto L_FlatMathFallback;
                        }
                        uIdx = static_cast<std::size_t>(idx);
                    }
                    auto* rawBytes = static_cast<std::uint8_t*>(ce.packedView.rawData);
                    switch (ce.packedView.encoding) {
                    case PackedElemEncoding::Float64:
                        reinterpret_cast<double*>(rawBytes)[uIdx] = storeVal.asDoubleUnchecked();
                        break;
                    case PackedElemEncoding::Float32:
                        reinterpret_cast<float*>(rawBytes)[uIdx] = storeVal.asFloatUnchecked();
                        break;
                    case PackedElemEncoding::Int32: {
                        const std::int64_t iv = storeVal.asInt64Unchecked();
                        if (RS_FLAT_UNLIKELY(iv < std::numeric_limits<std::int32_t>::min() ||
                                             iv > std::numeric_limits<std::int32_t>::max())) {
                            goto L_FlatMathFallback;
                        }
                        reinterpret_cast<std::int32_t*>(rawBytes)[uIdx] = static_cast<std::int32_t>(iv);
                        break;
                    }
                    case PackedElemEncoding::UInt32:
                        reinterpret_cast<std::uint32_t*>(rawBytes)[uIdx] = storeVal.u_.uintCell.payload.value;
                        break;
                    case PackedElemEncoding::Int64:
                        reinterpret_cast<std::int64_t*>(rawBytes)[uIdx] = storeVal.asLongUnchecked();
                        break;
                    case PackedElemEncoding::UInt64:
                        reinterpret_cast<std::uint64_t*>(rawBytes)[uIdx] = storeVal.u_.ulongCell.payload.value;
                        break;
                    case PackedElemEncoding::Int64Wide:
                        reinterpret_cast<std::int64_t*>(rawBytes)[uIdx] = storeVal.asInt64Unchecked();
                        break;
                    case PackedElemEncoding::Bool:
                        rawBytes[uIdx] = storeVal.asBoolUnchecked() ? 1u : 0u;
                        break;
                    case PackedElemEncoding::Byte:
                        rawBytes[uIdx] = storeVal.u_.byteCell.payload.value;
                        break;
                    case PackedElemEncoding::SByte:
                        rawBytes[uIdx] = static_cast<std::uint8_t>(storeVal.u_.sbyteCell.payload.value);
                        break;
                    case PackedElemEncoding::Short:
                        reinterpret_cast<std::int16_t*>(rawBytes)[uIdx] = storeVal.u_.shortCell.payload.value;
                        break;
                    case PackedElemEncoding::UShort:
                        reinterpret_cast<std::uint16_t*>(rawBytes)[uIdx] = storeVal.u_.ushortCell.payload.value;
                        break;
                    case PackedElemEncoding::Char:
                        reinterpret_cast<std::uint16_t*>(rawBytes)[uIdx] = storeVal.u_.charCell.payload.value;
                        break;
                    default:
                        goto L_FlatMathFallback;
                    }
                    localExecuted += spanCost;
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_UNLIKELY(ce.view.elementType != expectedType ||
                                     ce.view.elementTypeId != 0 ||
                                     storeVal.rawTag() != expectedTag)) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    localExecuted += spanCost;
                    ce.view.data[uIdx] = storeVal;
                    RS_FLAT_DISPATCH_NEXT();
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                ce.view.data[static_cast<std::size_t>(idx)] = storeVal;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreElementStructFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const auto expectedTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (ce.isPacked) {
                    if (RS_FLAT_UNLIKELY(ce.packedView.elementType != semantic::PrimitiveType::Struct ||
                                         ce.packedView.elementTypeId != expectedTid)) {
                        goto L_FlatMathFallback;
                    }
                    const std::uint64_t spanCost = ip->span;
                    if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                        if (spanCost > 1u) goto L_FlatSuperFallback;
                        goto L_FlatBudgetExceeded;
                    }
                    std::size_t uIdx = 0;
                    if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                        uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    } else {
                        std::int64_t idx = 0;
                        if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                             static_cast<std::uint64_t>(idx) >= ce.packedView.length)) {
                            goto L_FlatMathFallback;
                        }
                        uIdx = static_cast<std::size_t>(idx);
                    }
                    const Value& storeVal = frameBase[ip->src2Slot];
                    bool needsDemote = false;
                    if (RS_FLAT_UNLIKELY(!writePackedStructElement(
                            ce.packedView.rawData,
                            uIdx,
                            ce.packedView.encoding,
                            expectedTid,
                            ce.packedView.blittableSchemaId,
                            storeVal,
                            needsDemote))) {
                        ce.valid = false;
                        goto L_FlatMathFallback;
                    }
                    localExecuted += spanCost;
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (ce.isFlatStruct) {
                    if (RS_FLAT_UNLIKELY(ce.flatStructView.elementTypeId != expectedTid)) {
                        goto L_FlatMathFallback;
                    }
                    const Value& storeVal = frameBase[ip->src2Slot];
                    if (RS_FLAT_UNLIKELY(storeVal.rawTag() != ValueTag::Struct ||
                                         !storeVal.u_.structVal.valid() ||
                                         storeVal.u_.structVal.typeId != expectedTid)) {
                        goto L_FlatMathFallback;
                    }
                    const std::uint64_t spanCost = ip->span;
                    if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                        if (spanCost > 1u) goto L_FlatSuperFallback;
                        goto L_FlatBudgetExceeded;
                    }
                    std::size_t uIdx = 0;
                    if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                        uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    } else {
                        std::int64_t idx = 0;
                        if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                             static_cast<std::uint64_t>(idx) >= ce.flatStructView.length)) {
                            goto L_FlatMathFallback;
                        }
                        uIdx = static_cast<std::size_t>(idx);
                    }
                    const std::size_t stride = ce.flatStructView.flatStride;
                    Value* dstElem = ce.flatStructView.data + uIdx * stride;
                    const auto& sv = storeVal.u_.structVal;
                    if (RS_FLAT_LIKELY(sv.isFrameSlice() &&
                                       sv.frameSchemaId() == ce.flatStructView.flatSchemaId &&
                                       sv.storage.frameSliceBank() == state.flatBankId)) {
                        const std::uint32_t srcAbs = sv.storage.frameSliceOffset();
                        std::memcpy(
                            static_cast<void*>(dstElem),
                            static_cast<const void*>(state.values.data() + srcAbs),
                            stride * sizeof(Value));
                    } else {
                        const auto* sc = getFlatStructSchema(ce.flatStructView.flatSchemaId);
                        if (RS_FLAT_UNLIKELY(!sc || !extractStructLeavesToBuffer(storeVal, *sc, dstElem))) {
                            goto L_FlatMathFallback;
                        }
                    }
                    if (ce.flatStructView.containsPoolValues) {
                        for (std::size_t k = 0; k < stride; ++k) {
                            pinValueInPools(dstElem[k]);
                        }
                    }
                    if (ce.flatStructView.containsManagedReferences) {
                        if (ce.flatStructView.dirtyCards != nullptr) {
                            ce.flatStructView.dirtyCards[uIdx >> ManagedHeap::kCardElementShift] = 1u;
                        }
                        if (RS_FLAT_UNLIKELY(ce.flatStructView.needsRememberedBarrier ||
                                             gcActive ||
                                             state.heap->phase() != GcPhase::Idle)) {
                            state.heap->recordFlatStructArrayWriteBarrier(ce.ref, uIdx);
                            ce.flatStructView.needsRememberedBarrier = false;
                        }
                    }
                    localExecuted += spanCost;
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_UNLIKELY(ce.view.elementType != semantic::PrimitiveType::Struct ||
                                     ce.view.elementTypeId != expectedTid)) {
                    goto L_FlatMathFallback;
                }
                const Value& storeVal = frameBase[ip->src2Slot];
                const ValueTag vtag = storeVal.rawTag();
                bool structOk = false;
                bool needsPoolPin = false;
                bool mayHaveRefs = false;
                switch (ip->aux0Slot) {
                case 1: structOk = (vtag == ValueTag::Fixed); break;
                case 2: structOk = (vtag == ValueTag::Fixed16); break;
                case 3: structOk = (vtag == ValueTag::Vec2); break;
                case 4: structOk = (vtag == ValueTag::Vec3); break;
                case 5:
                    if (vtag == ValueTag::QuatInline) {
                        structOk = true;
                    } else if (vtag == ValueTag::QuatArena) {
                        structOk = true;
                        needsPoolPin = true;
                    }
                    break;
                case 6: structOk = (vtag == ValueTag::Fixed64); break;
                case 7: structOk = (vtag == ValueTag::Vec2f); break;
                case 8: structOk = (vtag == ValueTag::Vec3f); break;
                case 9:
                    if (vtag == ValueTag::QuatfInline) {
                        structOk = true;
                    } else if (vtag == ValueTag::QuatfArena) {
                        structOk = true;
                        needsPoolPin = true;
                    }
                    break;
                default:
                    if (vtag == ValueTag::Struct &&
                        storeVal.u_.structVal.valid() &&
                        storeVal.u_.structVal.typeId == expectedTid) {
                        structOk = true;
                        needsPoolPin = true;
                        mayHaveRefs = !storeVal.u_.structVal.isInline && storeVal.u_.structVal.storage != nullptr;
                    }
                    break;
                }
                if (RS_FLAT_UNLIKELY(!structOk)) goto L_FlatMathFallback;

                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                std::size_t uIdx = 0;
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                } else {
                    std::int64_t idx = 0;
                    if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                         static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                        goto L_FlatMathFallback;
                    }
                    uIdx = static_cast<std::size_t>(idx);
                }
                if (needsPoolPin) {
                    pinValueInPools(storeVal);
                }
                if (mayHaveRefs) {
                    if (ce.view.dirtyCards != nullptr) {
                        ce.view.dirtyCards[uIdx >> ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (RS_FLAT_UNLIKELY(ce.view.needsRememberedBarrier || gcActive || state.heap->phase() != GcPhase::Idle)) {
                        state.heap->recordArrayWriteBarrier(ce.ref, uIdx, storeVal);
                        ce.view.needsRememberedBarrier = false;
                    }
                }
                localExecuted += spanCost;
                if (RS_FLAT_UNLIKELY(vtag == ValueTag::Struct && storeVal.u_.structVal.isFrameSlice())) {
                    Value detached = detachFrameSliceValue(storeVal);
                    pinValueInPools(detached);
                    if (ce.view.dirtyCards != nullptr) {
                        ce.view.dirtyCards[uIdx >> ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (RS_FLAT_UNLIKELY(ce.view.needsRememberedBarrier || gcActive || state.heap->phase() != GcPhase::Idle)) {
                        state.heap->recordArrayWriteBarrier(ce.ref, uIdx, detached);
                        ce.view.needsRememberedBarrier = false;
                    }
                    ce.view.data[uIdx] = std::move(detached);
                } else {
                    ce.view.data[uIdx] = storeVal;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreElementObjFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const auto expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1));
                const auto expectedTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (RS_FLAT_UNLIKELY(ce.isPacked ||
                                     ce.view.elementType != expectedType ||
                                     ce.view.elementTypeId != expectedTid)) {
                    goto L_FlatMathFallback;
                }
                const Value& storeVal = frameBase[ip->src2Slot];
                const ValueTag vtag = storeVal.rawTag();
                bool needsBarrier = false;
                bool needsPoolPin = false;
                if (expectedType == semantic::PrimitiveType::Object) {
                    if (vtag == ValueTag::NullObject) {
                        // Valid null reference; no GC barrier needed
                    } else if (vtag == ValueTag::ObjectRef) {
                        const semantic::SymbolId actualTid = state.heap->objectTypeIdFast(storeVal.u_.objectRef);
                        if (RS_FLAT_UNLIKELY(actualTid == 0)) goto L_FlatMathFallback;
                        if (actualTid != expectedTid && expectedTid != 0) {
                            if (actualTid != cachedAssignableSubTid || expectedTid != cachedAssignableBaseTid) {
                                if (RS_FLAT_UNLIKELY(!runtimeAssignable(state, actualTid, expectedTid))) {
                                    goto L_FlatMathFallback;
                                }
                                cachedAssignableSubTid = actualTid;
                                cachedAssignableBaseTid = expectedTid;
                            }
                        }
                        needsBarrier = true;
                    } else {
                        goto L_FlatMathFallback;
                    }
                } else {
                    // PrimitiveType::String
                    if (vtag == ValueTag::NullString) {
                        // Valid null string
                    } else if (vtag == ValueTag::String) {
                        needsPoolPin = true;
                    } else if (vtag == ValueTag::ObjectRef &&
                               storeVal.u_.objectRef.kind == ObjectKind::String &&
                               state.heap->isAlive(storeVal.u_.objectRef)) {
                        needsBarrier = true;
                    } else {
                        goto L_FlatMathFallback;
                    }
                }

                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                std::size_t uIdx = 0;
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                } else {
                    std::int64_t idx = 0;
                    if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                         static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                        goto L_FlatMathFallback;
                    }
                    uIdx = static_cast<std::size_t>(idx);
                }
                if (needsPoolPin) {
                    pinValueInPools(storeVal);
                }
                if (needsBarrier) {
                    if (ce.view.dirtyCards != nullptr) {
                        ce.view.dirtyCards[uIdx >> ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (RS_FLAT_UNLIKELY(ce.view.needsRememberedBarrier || gcActive || state.heap->phase() != GcPhase::Idle)) {
                        state.heap->recordArrayWriteBarrier(ce.ref, uIdx, storeVal);
                        ce.view.needsRememberedBarrier = false;
                    }
                }
                localExecuted += spanCost;
                ce.view.data[uIdx] = storeVal;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperLessLocalArrayLengthBranch) {
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) {
                    goto L_FlatSuperFallback;
                }
                auto& ce = arrayCache[ip->src1Slot & 3u];
                const Value& arrVal = frameBase[ip->src1Slot];
                std::int64_t right = 0;
                if (RS_FLAT_LIKELY((ce.valid &&
                                    arrVal.rawTag() == ValueTag::ObjectRef &&
                                    arrVal.u_.objectRef == ce.ref) ||
                                   bindArrayCacheSlot(arrVal, ce))) {
                    right = static_cast<std::int64_t>(ce.view.length);
                } else if (arrVal.rawTag() == ValueTag::ObjectRef && state.heap != nullptr) {
                    const auto lenOpt = state.heap->arrayLength(arrVal.u_.objectRef);
                    if (!lenOpt || *lenOpt > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                        goto L_FlatSuperFallback;
                    }
                    right = static_cast<std::int64_t>(*lenOpt);
                } else {
                    goto L_FlatSuperFallback;
                }
                bool cond = false;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::LessInt: cond = (left < right); break;
                case detail::QuickOpcode::LessOrEqualInt: cond = (left <= right); break;
                case detail::QuickOpcode::GreaterInt: cond = (left > right); break;
                case detail::QuickOpcode::GreaterOrEqualInt: cond = (left >= right); break;
                case detail::QuickOpcode::EqualInt: cond = (left == right); break;
                case detail::QuickOpcode::NotEqualInt: cond = (left != right); break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                ++state.statistics.branchesTaken;
                ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(NewObjectFast) {
                if (RS_FLAT_UNLIKELY(state.heap == nullptr)) goto L_FlatMathFallback;
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                ++localExecuted;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                else gcAccounted = localExecuted;

                const auto& tmpl = detail::executionMirror(*state.program).objectTemplates[ip->aux0Slot];
                bool slotsMoved = false;
                RuntimeError allocErr;
                const auto ref = state.heap->allocateObjectFromTemplate(
                    tmpl.typeId,
                    tmpl.defaultFields.data(),
                    tmpl.defaultFields.size(),
                    tmpl.referenceFields,
                    &allocErr,
                    &slotsMoved);
                if (RS_FLAT_LIKELY(ref.has_value())) {
                    if (RS_FLAT_UNLIKELY(slotsMoved)) {
                        invalidateAllFlatCaches();
                    }
                    if (gcBudgetEnabled) {
                        gcActive = (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
                    }
                    if (spanCost > 1u) {
                        localExecuted += (spanCost - 1u);
                    }
                    frameBase[ip->dstSlot] = *ref;
                    auto& oce = objectCache[ref->slot & 3u];
                    if (state.heap->tryBindObjectFastView(*ref, oce.view)) {
                        oce.ref = *ref;
                        oce.verifiedBaseTid = 0;
                        oce.valid = true;
                    }
                    RS_FLAT_DISPATCH_NEXT();
                }
                state.executed = localExecuted;
                fail(state,
                    allocErr.code == ErrorCode::None ? ErrorCode::OutOfMemory : allocErr.code,
                    allocErr.message.empty() ? "managed object allocation failed" : allocErr.message);
                goto L_FlatError;
            }

            RS_FLAT_CASE(LoadFieldExactFast) {
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool ownerOk = (oce.view.typeId == expectedOwnerTid) ||
                                       (oce.verifiedBaseTid == expectedOwnerTid);
                        if (RS_FLAT_UNLIKELY(!ownerOk)) {
                            if (runtimeAssignable(state, oce.view.typeId, expectedOwnerTid)) {
                                oce.verifiedBaseTid = expectedOwnerTid;
                                ownerOk = true;
                            }
                        }
                        const std::uint16_t fIdx = ip->aux0Slot;
                        if (RS_FLAT_LIKELY(ownerOk && fIdx < oce.view.fieldCount)) {
                            const Value& val = oce.view.fields[fIdx];
                            if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(val.rawTag()) ==
                                               static_cast<std::uint8_t>(ip->binaryOpcode1))) {
                                const std::uint64_t spanCost = ip->span;
                                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                                    if (spanCost > 1u) goto L_FlatSuperFallback;
                                    goto L_FlatBudgetExceeded;
                                }
                                localExecuted += spanCost;
                                frameBase[ip->dstSlot] = val;
                                RS_FLAT_DISPATCH_NEXT();
                            }
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(StoreFieldExactFast) {
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool ownerOk = (oce.view.typeId == expectedOwnerTid) ||
                                       (oce.verifiedBaseTid == expectedOwnerTid);
                        if (RS_FLAT_UNLIKELY(!ownerOk)) {
                            if (runtimeAssignable(state, oce.view.typeId, expectedOwnerTid)) {
                                oce.verifiedBaseTid = expectedOwnerTid;
                                ownerOk = true;
                            }
                        }
                        const std::uint16_t fIdx = ip->aux0Slot;
                        if (RS_FLAT_LIKELY(ownerOk && fIdx < oce.view.fieldCount)) {
                            const Value& rhs = frameBase[ip->src1Slot];
                            if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(rhs.rawTag()) ==
                                               static_cast<std::uint8_t>(ip->binaryOpcode1))) {
                                const std::uint64_t spanCost = ip->span;
                                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                                    if (spanCost > 1u) goto L_FlatSuperFallback;
                                    goto L_FlatBudgetExceeded;
                                }
                                localExecuted += spanCost;
                                oce.view.fields[fIdx] = rhs;
                                if (RS_FLAT_UNLIKELY(state.heap->hasDirtyTracking())) {
                                    state.heap->notifyFieldMutated(ref, fIdx);
                                }
                                RS_FLAT_DISPATCH_NEXT();
                            }
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(IsTypeObjFast) {
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& srcVal = frameBase[ip->src0Slot];
                const ValueTag vtag = srcVal.rawTag();
                if (vtag == ValueTag::NullObject || vtag == ValueTag::Monostate) {
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot].storeBool(false);
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_LIKELY(vtag == ValueTag::ObjectRef)) {
                    const ObjectRef ref = srcVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto targetTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool matches = (oce.view.typeId == targetTid) ||
                                       (oce.verifiedBaseTid == targetTid) ||
                                       (cachedAssignableSubTid == oce.view.typeId &&
                                        cachedAssignableBaseTid == targetTid);
                        if (!matches) {
                            matches = runtimeAssignable(state, oce.view.typeId, targetTid);
                            if (matches) {
                                oce.verifiedBaseTid = targetTid;
                                cachedAssignableSubTid = oce.view.typeId;
                                cachedAssignableBaseTid = targetTid;
                            }
                        }
                        localExecuted += spanCost;
                        frameBase[ip->dstSlot].storeBool(matches);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(AsTypeObjFast) {
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& srcVal = frameBase[ip->src0Slot];
                const ValueTag vtag = srcVal.rawTag();
                if (vtag == ValueTag::NullObject || vtag == ValueTag::Monostate) {
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot] = NullObject{};
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_LIKELY(vtag == ValueTag::ObjectRef)) {
                    const ObjectRef ref = srcVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto targetTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool matches = (oce.view.typeId == targetTid) ||
                                       (oce.verifiedBaseTid == targetTid) ||
                                       (cachedAssignableSubTid == oce.view.typeId &&
                                        cachedAssignableBaseTid == targetTid);
                        if (!matches) {
                            matches = runtimeAssignable(state, oce.view.typeId, targetTid);
                            if (matches) {
                                oce.verifiedBaseTid = targetTid;
                                cachedAssignableSubTid = oce.view.typeId;
                                cachedAssignableBaseTid = targetTid;
                            }
                        }
                        localExecuted += spanCost;
                        if (matches) {
                            frameBase[ip->dstSlot] = srcVal;
                        } else {
                            frameBase[ip->dstSlot] = NullObject{};
                        }
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(NewDelegateFast) {
                if (RS_FLAT_UNLIKELY(state.heap == nullptr || state.mirror == nullptr ||
                                     ip->auxImm32 >= state.mirror->newDelegateSites.size())) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const auto& site = state.mirror->newDelegateSites[ip->auxImm32];
                if (RS_FLAT_UNLIKELY(!site.valid)) goto L_FlatMathFallback;

                std::array<Value, 8> fields = site.templateFields;
                if (site.hasReceiverOperand) {
                    const Value& recvVal = frameBase[ip->src0Slot];
                    if (site.expectedReceiverType == semantic::PrimitiveType::Object) {
                        const ValueTag vt = recvVal.rawTag();
                        if (vt == ValueTag::NullObject) {
                            // Valid null receiver target
                        } else if (RS_FLAT_LIKELY(vt == ValueTag::ObjectRef)) {
                            const ObjectRef rRef = recvVal.u_.objectRef;
                            auto& rOce = objectCache[rRef.slot & 3u];
                            if (RS_FLAT_UNLIKELY(!(rOce.valid && rOce.ref == rRef) &&
                                                 !bindObjectCacheEntry(rRef, rOce))) {
                                goto L_FlatMathFallback;
                            }
                            const semantic::SymbolId expTid = site.expectedReceiverTypeId;
                            if (expTid != 0 &&
                                rOce.view.typeId != expTid &&
                                rOce.verifiedBaseTid != expTid &&
                                !(cachedAssignableSubTid == rOce.view.typeId &&
                                  cachedAssignableBaseTid == expTid)) {
                                if (RS_FLAT_UNLIKELY(!runtimeAssignable(state, rOce.view.typeId, expTid))) {
                                    goto L_FlatMathFallback;
                                }
                                rOce.verifiedBaseTid = expTid;
                                cachedAssignableSubTid = rOce.view.typeId;
                                cachedAssignableBaseTid = expTid;
                            }
                        } else {
                            goto L_FlatMathFallback;
                        }
                    } else {
                        goto L_FlatMathFallback;
                    }
                    fields[0] = recvVal;
                }

                localExecuted += spanCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                else gcAccounted = localExecuted;

                bool slotsMoved = false;
                RuntimeError allocErr;
                const auto ref = state.heap->allocateObjectFromTemplate(
                    site.delegateTypeId,
                    fields.data(),
                    fields.size(),
                    delegateReferenceFields(),
                    &allocErr,
                    &slotsMoved);
                if (RS_FLAT_LIKELY(ref.has_value())) {
                    if (RS_FLAT_UNLIKELY(slotsMoved)) {
                        invalidateAllFlatCaches();
                    }
                    if (gcBudgetEnabled) {
                        gcActive = (state.heap->phase() != GcPhase::Idle ||
                                    state.heap->collectionRequested());
                    }
                    frameBase[ip->dstSlot] = *ref;
                    auto& oce = objectCache[ref->slot & 3u];
                    if (state.heap->tryBindObjectFastView(*ref, oce.view)) {
                        oce.ref = *ref;
                        oce.verifiedBaseTid = 0;
                        oce.valid = true;
                    }
                    RS_FLAT_DISPATCH_NEXT();
                }
                state.executed = localExecuted;
                fail(state,
                    allocErr.code == ErrorCode::None ? ErrorCode::OutOfMemory : allocErr.code,
                    allocErr.message.empty() ? "delegate allocation failed" : allocErr.message);
                goto L_FlatError;
            }

            RS_FLAT_CASE(CombineDelegateFast) {
                if (RS_FLAT_UNLIKELY(state.heap == nullptr)) goto L_FlatMathFallback;
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& leftVal = frameBase[ip->src0Slot];
                const Value& rightVal = frameBase[ip->src1Slot];
                const auto delTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                const ValueTag lTag = leftVal.rawTag();
                const ValueTag rTag = rightVal.rawTag();
                if (RS_FLAT_UNLIKELY((lTag != ValueTag::NullObject && lTag != ValueTag::ObjectRef) ||
                                     (rTag != ValueTag::NullObject && rTag != ValueTag::ObjectRef))) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                else gcAccounted = localExecuted;
                state.executed = localExecuted;

                bool slotsMoved = false;
                Value combinedOut;
                if (RS_FLAT_UNLIKELY(!combineOrRemoveDelegateFastImpl(
                        state, delTid, leftVal, rightVal, false, combinedOut, &slotsMoved))) {
                    goto L_FlatError;
                }
                if (RS_FLAT_UNLIKELY(slotsMoved)) {
                    invalidateAllFlatCaches();
                }
                if (gcBudgetEnabled) {
                    gcActive = (state.heap->phase() != GcPhase::Idle ||
                                state.heap->collectionRequested());
                }
                frameBase[ip->dstSlot] = combinedOut;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(RemoveDelegateFast) {
                if (RS_FLAT_UNLIKELY(state.heap == nullptr)) goto L_FlatMathFallback;
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& leftVal = frameBase[ip->src0Slot];
                const Value& rightVal = frameBase[ip->src1Slot];
                const auto delTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                const ValueTag lTag = leftVal.rawTag();
                const ValueTag rTag = rightVal.rawTag();
                if (RS_FLAT_UNLIKELY((lTag != ValueTag::NullObject && lTag != ValueTag::ObjectRef) ||
                                     (rTag != ValueTag::NullObject && rTag != ValueTag::ObjectRef))) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                else gcAccounted = localExecuted;
                state.executed = localExecuted;

                bool slotsMoved = false;
                Value removedOut;
                if (RS_FLAT_UNLIKELY(!combineOrRemoveDelegateFastImpl(
                        state, delTid, leftVal, rightVal, true, removedOut, &slotsMoved))) {
                    goto L_FlatError;
                }
                if (RS_FLAT_UNLIKELY(slotsMoved)) {
                    invalidateAllFlatCaches();
                }
                if (gcBudgetEnabled) {
                    gcActive = (state.heap->phase() != GcPhase::Idle ||
                                state.heap->collectionRequested());
                }
                frameBase[ip->dstSlot] = removedOut;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InvokeDelegateFast) {
                const std::uint64_t baseCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || baseCost > budgetLimit - localExecuted)) {
                    if (baseCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                if (RS_FLAT_UNLIKELY(state.heap == nullptr || state.mirror == nullptr ||
                                     state.delegateCallCache == nullptr ||
                                     ip->auxImm32 >= state.mirror->invokeDelegateSites.size() ||
                                     ip->auxImm32 >= state.delegateCallCache->size())) {
                    goto L_FlatMathFallback;
                }
                const auto& siteDesc = state.mirror->invokeDelegateSites[ip->auxImm32];
                const Value& delVal = frameBase[ip->src0Slot];
                const ValueTag delTag = delVal.rawTag();
                if (RS_FLAT_UNLIKELY(delTag == ValueTag::NullObject || delTag == ValueTag::Monostate)) {
                    localExecuted += baseCost;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::NullReference,
                        "delegate invocation attempted to dereference null");
                    goto L_FlatError;
                }
                if (RS_FLAT_UNLIKELY(delTag != ValueTag::ObjectRef)) goto L_FlatMathFallback;

                const ObjectRef delRef = delVal.u_.objectRef;
                auto& delOce = objectCache[delRef.slot & 3u];
                if (RS_FLAT_UNLIKELY(!(delOce.valid && delOce.ref == delRef) &&
                                     !bindObjectCacheEntry(delRef, delOce))) {
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(delOce.view.typeId != siteDesc.delegateTypeId ||
                                     delOce.view.fieldCount != 8u)) {
                    goto L_FlatMathFallback;
                }

                // Extract and type-validate user arguments into a C++ stack buffer (zero heap malloc!)
                const std::size_t uCount = siteDesc.userParamTypes.size();
                std::array<Value, 8> userArgsStack{};
                std::vector<Value> userArgsHeap;
                Value* userArgsPtr = userArgsStack.data();
                if (RS_FLAT_UNLIKELY(uCount > userArgsStack.size())) {
                    userArgsHeap.resize(uCount);
                    userArgsPtr = userArgsHeap.data();
                }
                if (ip->inlineArgCount >= 1u) {
                    if (uCount >= 1u) userArgsPtr[0] = frameBase[ip->src1Slot];
                    if (uCount >= 2u) userArgsPtr[1] = frameBase[ip->src2Slot];
                } else {
                    const auto& cold = flatColdBase[ip - streamBase];
                    const std::size_t regBase = execution->registerOffset;
                    for (std::size_t k = 0; k < uCount; ++k) {
                        userArgsPtr[k] = frameBase[regBase + cold.bytecode->operands[k + 1u]];
                    }
                }
                bool userArgsValid = true;
                for (std::size_t k = 0; k < uCount; ++k) {
                    const std::uint8_t expTag = siteDesc.userParamExactTags[k];
                    if (expTag != 0u) {
                        if (RS_FLAT_UNLIKELY(static_cast<std::uint8_t>(userArgsPtr[k].rawTag()) != expTag)) {
                            userArgsValid = false;
                            break;
                        }
                    } else {
                        const auto pt = siteDesc.userParamTypes[k];
                        const auto ptId = siteDesc.userParamTypeIds[k];
                        const ValueTag vt = userArgsPtr[k].rawTag();
                        if (pt == semantic::PrimitiveType::Object) {
                            if (vt == ValueTag::NullObject) {
                                // ok
                            } else if (vt == ValueTag::ObjectRef) {
                                if (userArgsPtr[k].u_.objectRef.isStackSlotRef()) {
                                    if (RS_FLAT_UNLIKELY(userArgsPtr[k].u_.objectRef.generation != state.flatBankId)) {
                                        userArgsValid = false;
                                        break;
                                    }
                                } else {
                                const auto actTid = state.heap->objectTypeIdFast(userArgsPtr[k].u_.objectRef);
                                if (actTid == 0) {
                                    userArgsValid = false;
                                    break;
                                }
                                if (ptId != 0 && actTid != ptId &&
                                    !(cachedAssignableSubTid == actTid && cachedAssignableBaseTid == ptId)) {
                                    if (!runtimeAssignable(state, actTid, ptId)) {
                                        userArgsValid = false;
                                        break;
                                    }
                                    cachedAssignableSubTid = actTid;
                                    cachedAssignableBaseTid = ptId;
                                }
                                }
                            } else {
                                userArgsValid = false;
                                break;
                            }
                        } else {
                            // Complex Struct / Enum / String / Array: verify cleanly via expectSignatureType-free check
                            userArgsValid = false;
                            break;
                        }
                    }
                }
                if (RS_FLAT_UNLIKELY(!userArgsValid)) goto L_FlatMathFallback;

                std::int64_t rootFlags = -1;
                if (RS_FLAT_UNLIKELY(!delOce.view.fields[3].tryGetInt64(rootFlags) ||
                                     rootFlags < 0 || rootFlags > 4)) {
                    goto L_FlatMathFallback;
                }

                auto& siteCache = (*state.delegateCallCache)[ip->auxImm32];

                const auto tryEvalTrivialLeaf = [&](const ResolvedDelegateLeaf& leaf,
                                                    std::uint64_t preCallCost,
                                                    Value& outTrivialVal) -> bool {
                    const auto* calleeExec = leaf.execution;
                    const auto tKind = calleeExec->trivialKind;
                    if (tKind == detail::TrivialMethodKind::None ||
                        RS_FLAT_UNLIKELY(state.stack.size() >= state.limits.recursionLimit)) {
                        return false;
                    }
                    const std::uint64_t totalCost = preCallCost + calleeExec->trivialInstCost;
                    if (RS_FLAT_UNLIKELY(totalCost > budgetLimit - localExecuted)) {
                        return false;
                    }
                    const std::size_t bias = leaf.hasBoundReceiver ? 1u : 0u;
                    bool handled = false;
                    switch (tKind) {
                    case detail::TrivialMethodKind::ReturnVoid:
                        handled = true;
                        break;
                    case detail::TrivialMethodKind::ReturnConst:
                        outTrivialVal = calleeExec->trivialConstValue;
                        handled = true;
                        break;
                    case detail::TrivialMethodKind::ReturnParam: {
                        const std::uint16_t pIdx = calleeExec->trivialParamIndex;
                        if (leaf.hasBoundReceiver && pIdx == 0) {
                            outTrivialVal = leaf.receiverVal;
                            handled = true;
                        } else if (pIdx >= bias && (pIdx - bias) < uCount) {
                            outTrivialVal = userArgsPtr[pIdx - bias];
                            handled = true;
                        }
                        break;
                    }
                    case detail::TrivialMethodKind::ReturnThisFieldExact:
                    case detail::TrivialMethodKind::StoreThisFieldExact:
                    case detail::TrivialMethodKind::ReturnThisFieldAddField:
                    case detail::TrivialMethodKind::ReturnThisFieldSubField:
                    case detail::TrivialMethodKind::ReturnThisFieldAddConst:
                    case detail::TrivialMethodKind::ReturnThisFieldSubConst:
                    case detail::TrivialMethodKind::ReturnThisFieldMulConst: {
                        if (!leaf.hasBoundReceiver || leaf.receiverVal.rawTag() != ValueTag::ObjectRef) {
                            break;
                        }
                        auto& rOce = objectCache[leaf.receiverRef.slot & 3u];
                        if (RS_FLAT_UNLIKELY(!(rOce.valid && rOce.ref == leaf.receiverRef) &&
                                             !bindObjectCacheEntry(leaf.receiverRef, rOce))) {
                            break;
                        }
                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                        if (tKind == detail::TrivialMethodKind::ReturnThisFieldExact) {
                            if (RS_FLAT_LIKELY(f0 < rOce.view.fieldCount &&
                                               static_cast<std::uint8_t>(rOce.view.fields[f0].rawTag()) ==
                                                   calleeExec->trivialFieldTag0)) {
                                outTrivialVal = rOce.view.fields[f0];
                                handled = true;
                            }
                        } else if (tKind == detail::TrivialMethodKind::StoreThisFieldExact) {
                            const std::uint16_t pIdx = calleeExec->trivialParamIndex;
                            if (pIdx >= bias && (pIdx - bias) < uCount &&
                                RS_FLAT_LIKELY(f0 < rOce.view.fieldCount)) {
                                const Value& rhs = userArgsPtr[pIdx - bias];
                                if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(rhs.rawTag()) ==
                                                   calleeExec->trivialFieldTag0)) {
                                    rOce.view.fields[f0] = rhs;
                                    if (RS_FLAT_UNLIKELY(state.heap->hasDirtyTracking())) {
                                        state.heap->notifyFieldMutated(leaf.receiverRef, f0);
                                    }
                                    handled = true;
                                }
                            }
                        } else if (tKind == detail::TrivialMethodKind::ReturnThisFieldAddField ||
                                   tKind == detail::TrivialMethodKind::ReturnThisFieldSubField) {
                            const std::uint16_t f1 = calleeExec->trivialFieldIndex1;
                            std::int64_t a = 0, b = 0;
                            if (RS_FLAT_LIKELY(f0 < rOce.view.fieldCount &&
                                               f1 < rOce.view.fieldCount &&
                                               rOce.view.fields[f0].tryGetInt64(a) &&
                                               rOce.view.fields[f1].tryGetInt64(b))) {
                                const std::int64_t r =
                                    (tKind == detail::TrivialMethodKind::ReturnThisFieldAddField)
                                        ? (a + b)
                                        : (a - b);
                                if (!calleeExec->trivialCheckedArith) {
                                    outTrivialVal.storeInt64(wrapInt32(r));
                                    handled = true;
                                } else if (RS_FLAT_LIKELY(isValidInt32Payload(r))) {
                                    outTrivialVal.storeInt64(r);
                                    handled = true;
                                }
                            }
                        } else {
                            std::int64_t a = 0;
                            if (RS_FLAT_LIKELY(f0 < rOce.view.fieldCount &&
                                               rOce.view.fields[f0].tryGetInt64(a))) {
                                const std::int64_t c = calleeExec->trivialConstValue.asInt64Unchecked();
                                std::int64_t r = 0;
                                bool ovf = false;
                                if (tKind == detail::TrivialMethodKind::ReturnThisFieldAddConst) r = a + c;
                                else if (tKind == detail::TrivialMethodKind::ReturnThisFieldSubConst) r = a - c;
                                else {
#if defined(__GNUC__) || defined(__clang__)
                                    ovf = __builtin_mul_overflow(a, c, &r);
#else
                                    if (RS_FLAT_LIKELY(isValidInt32Payload(a) && isValidInt32Payload(c))) {
                                        r = a * c;
                                    } else {
                                        ovf = true;
                                    }
#endif
                                }
                                if (!ovf) {
                                    if (!calleeExec->trivialCheckedArith) {
                                        outTrivialVal.storeInt64(wrapInt32(r));
                                        handled = true;
                                    } else if (RS_FLAT_LIKELY(isValidInt32Payload(r))) {
                                        outTrivialVal.storeInt64(r);
                                        handled = true;
                                    }
                                }
                            }
                        }
                        break;
                    }
                    case detail::TrivialMethodKind::None:
                        break;
                    }
                    if (RS_FLAT_LIKELY(handled)) {
                        localExecuted += totalCost;
                        ++state.statistics.functionCalls;
                        if (state.stack.size() + 1 > state.statistics.maximumCallDepth) {
                            state.statistics.maximumCallDepth = state.stack.size() + 1;
                        }
                        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                        return true;
                    }
                    return false;
                };

                if (RS_FLAT_LIKELY(rootFlags != 4)) {
                    // Single-cast 16-entry direct-mapped IC keyed by immutable delegate ObjectRef
                    auto& slotWay = siteCache.singleWays[delRef.slot & 15u];
                    const ResolvedDelegateLeaf* hit = nullptr;
                    if (RS_FLAT_LIKELY(
                            slotWay.valid &&
                            slotWay.leafRef == delRef &&
                            (!slotWay.hasBoundReceiver ||
                             slotWay.receiverVal.rawTag() != ValueTag::ObjectRef ||
                             state.heap->isAlive(slotWay.receiverRef)))) {
                        hit = &slotWay;
                    } else {
                        ResolvedDelegateLeaf resolved{};
                        if (!resolveDelegateLeafFast(
                                state, *state.mirror, delRef, delOce.view, uCount, siteCache, resolved)) {
                            goto L_FlatMathFallback;
                        }
                        slotWay = std::move(resolved);
                        hit = &slotWay;
                    }

                    Value callResult{};
                    if (tryEvalTrivialLeaf(*hit, baseCost, callResult)) {
                        if (ip->dstSlot != detail::InvalidCompactReg) {
                            frameBase[ip->dstSlot] = callResult;
                        }
                        RS_FLAT_DISPATCH_NEXT();
                    }

                    localExecuted += baseCost;
                    if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                    state.executed = localExecuted;

                    const ResolvedDelegateLeaf hitLeaf = *hit;
                    bool ok = false;
                    if (!hitLeaf.hasBoundReceiver) {
                        ok = executeFunction(
                            state,
                            *hitLeaf.target,
                            ArgumentSource(userArgsPtr, uCount),
                            callResult,
                            hitLeaf.execution,
                            true);
                    } else if (RS_FLAT_LIKELY(uCount + 1u <= 8u)) {
                        std::array<Value, 8> boundArgs{};
                        boundArgs[0] = hitLeaf.receiverVal;
                        for (std::size_t k = 0; k < uCount; ++k) boundArgs[k + 1u] = userArgsPtr[k];
                        ok = executeFunction(
                            state,
                            *hitLeaf.target,
                            ArgumentSource(boundArgs.data(), uCount + 1u),
                            callResult,
                            hitLeaf.execution,
                            true);
                    } else {
                        std::vector<Value> boundVec;
                        boundVec.reserve(uCount + 1u);
                        boundVec.push_back(hitLeaf.receiverVal);
                        for (std::size_t k = 0; k < uCount; ++k) boundVec.push_back(userArgsPtr[k]);
                        ok = executeFunction(
                            state,
                            *hitLeaf.target,
                            ArgumentSource(boundVec),
                            callResult,
                            hitLeaf.execution,
                            true);
                    }

                    frameBase = state.values.data() + frameOffset;
                    if (RS_FLAT_LIKELY(hitLeaf.execution->isSmallLeafInlineCandidate && !gcBudgetEnabled)) {
                        localExecuted = state.executed;
                        gcAccounted = localExecuted;
                    } else {
                        syncAfterFlatSubcall();
                    }
                    if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                    if (ip->dstSlot != detail::InvalidCompactReg) {
                        frameBase[ip->dstSlot] = std::move(callResult);
                    }
                    RS_FLAT_DISPATCH_NEXT();
                }

                // Multicast fast path (flags == 4): pre-flattened & pre-resolved leaf vector cache!
                bool mcCacheHit = (siteCache.multicastValid && siteCache.multicastRef == delRef);
                if (mcCacheHit) {
                    for (const auto& leaf : siteCache.multicastLeaves) {
                        if (!state.heap->isAlive(leaf.leafRef) ||
                            (leaf.hasBoundReceiver &&
                             leaf.receiverVal.rawTag() == ValueTag::ObjectRef &&
                             !state.heap->isAlive(leaf.receiverRef))) {
                            mcCacheHit = false;
                            break;
                        }
                    }
                }
                std::vector<ResolvedDelegateLeaf> fallbackMulticastLeaves;
                std::vector<ResolvedDelegateLeaf>* activeLeaves = &siteCache.multicastLeaves;
                bool pinnedSiteCache = false;
                if (RS_FLAT_LIKELY(mcCacheHit)) {
                    ++siteCache.multicastPinCount;
                    pinnedSiteCache = true;
                } else if (siteCache.multicastPinCount == 0u) {
                    siteCache.multicastValid = false;
                    if (!flattenAndResolveMulticastFast(
                            state, *state.mirror, siteDesc.delegateTypeId, delRef, uCount, siteCache, siteCache.multicastLeaves)) {
                        goto L_FlatMathFallback;
                    }
                    siteCache.multicastRef = delRef;
                    siteCache.multicastValid = true;
                    ++siteCache.multicastPinCount;
                    pinnedSiteCache = true;
                } else {
                    activeLeaves = &fallbackMulticastLeaves;
                    if (!flattenAndResolveMulticastFast(
                            state, *state.mirror, siteDesc.delegateTypeId, delRef, uCount, siteCache, fallbackMulticastLeaves)) {
                        goto L_FlatMathFallback;
                    }
                }
                struct MulticastPinGuard {
                    DelegateCallSiteCache& cache;
                    bool pinned = false;
                    ~MulticastPinGuard() {
                        if (pinned && cache.multicastPinCount > 0u) {
                            --cache.multicastPinCount;
                        }
                    }
                } pinGuard{siteCache, pinnedSiteCache};

                localExecuted += baseCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;

                Value lastResult{};
                std::array<Value, 8> mcBoundArgs{};
                const std::size_t leafCount = activeLeaves->size();
                for (std::size_t li = 0; li < leafCount; ++li) {
                    const ResolvedDelegateLeaf leaf = (*activeLeaves)[li];
                    if (tryEvalTrivialLeaf(leaf, 1u, lastResult)) {
                        state.executed = localExecuted;
                        continue;
                    }
                    if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) {
                        state.executed = localExecuted - 1u;
                        fail(state, ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                        goto L_FlatError;
                    }
                    if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                    state.executed = localExecuted;

                    bool ok = false;
                    if (!leaf.hasBoundReceiver) {
                        ok = executeFunction(
                            state,
                            *leaf.target,
                            ArgumentSource(userArgsPtr, uCount),
                            lastResult,
                            leaf.execution,
                            true);
                    } else if (RS_FLAT_LIKELY(uCount + 1u <= mcBoundArgs.size())) {
                        mcBoundArgs[0] = leaf.receiverVal;
                        for (std::size_t k = 0; k < uCount; ++k) mcBoundArgs[k + 1u] = userArgsPtr[k];
                        ok = executeFunction(
                            state,
                            *leaf.target,
                            ArgumentSource(mcBoundArgs.data(), uCount + 1u),
                            lastResult,
                            leaf.execution,
                            true);
                    } else {
                        std::vector<Value> boundVec;
                        boundVec.reserve(uCount + 1u);
                        boundVec.push_back(leaf.receiverVal);
                        for (std::size_t k = 0; k < uCount; ++k) boundVec.push_back(userArgsPtr[k]);
                        ok = executeFunction(
                            state,
                            *leaf.target,
                            ArgumentSource(boundVec),
                            lastResult,
                            leaf.execution,
                            true);
                    }
                    if (RS_FLAT_LIKELY(leaf.execution->isSmallLeafInlineCandidate && !gcBudgetEnabled)) {
                        localExecuted = state.executed;
                        gcAccounted = localExecuted;
                    } else {
                        syncAfterFlatSubcall();
                    }
                    if (RS_FLAT_UNLIKELY(!ok)) {
                        frameBase = state.values.data() + frameOffset;
                        goto L_FlatError;
                    }
                }
                frameBase = state.values.data() + frameOffset;
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    storeValueIntoFlatFrameSlot(state, *execution, frameOffset, frameBase, ip->dstSlot, lastResult);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AsyncEnter) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AwaitSuspend) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const Value awaitableVal = frameBase[ip->src0Slot];
                frameBase[ip->storeSlot] = awaitableVal;
                if (RS_FLAT_UNLIKELY(state.awaitScheduler && state.awaitScheduler->handlesValue(awaitableVal))) {
                    if (state.awaitScheduler->isCompleted(awaitableVal)) {
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    state.executed = localExecuted;
                    if (RS_FLAT_UNLIKELY(!suspendInterpreterWithAwaitScheduler(
                            state,
                            function,
                            activeCoroutine,
                            frameBase + execution->paramOffset,
                            argCount,
                            frameBase + execution->localOffset,
                            function.localTypes.size(),
                            frameBase + execution->registerOffset,
                            function.registerTypes.size(),
                            static_cast<std::uint32_t>(ip->integerImmediate),
                            awaitableVal,
                            result))) {
                        goto L_FlatError;
                    }
                    state.valueTop = frameOffset;
                    state.stack.pop_back();
                    return true;
                }
                AwaitableHandle awaitedHandle{};
                if (RS_FLAT_UNLIKELY(!state.asyncRuntime ||
                                     !tryExtractAwaitableHandle(awaitableVal, awaitedHandle) ||
                                     !state.asyncRuntime->awaitables().isValid(awaitedHandle))) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::InvalidProgram, "awaited value does not hold a valid Awaitable handle");
                    goto L_FlatError;
                }
                if (state.asyncRuntime->awaitables().isCompleted(awaitedHandle)) {
                    RS_FLAT_DISPATCH_NEXT();
                }
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!suspendInterpreterCoroutine(
                        state,
                        function,
                        activeCoroutine,
                        frameBase + execution->paramOffset,
                        argCount,
                        frameBase + execution->localOffset,
                        function.localTypes.size(),
                        frameBase + execution->registerOffset,
                        function.registerTypes.size(),
                        static_cast<std::uint32_t>(ip->integerImmediate),
                        awaitedHandle,
                        result))) {
                    goto L_FlatError;
                }
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return true;
            }

            RS_FLAT_CASE(AwaitResume) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const Value awaitableVal = frameBase[ip->src0Slot];
                frameBase[ip->src0Slot] = Value{};
                Value resumedVal{};
                const bool hasRes = (ip->dstSlot != detail::InvalidCompactReg);
                if (RS_FLAT_UNLIKELY(!resumeInterpreterAwaitable(
                        state,
                        awaitableVal,
                        hasRes,
                        static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1)),
                        static_cast<semantic::SymbolId>(ip->integerImmediate),
                        resumedVal))) {
                    frameBase = state.values.data() + frameOffset;
                    syncAfterFlatSubcall();
                    goto L_FlatError;
                }
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (hasRes) {
                    storeValueIntoFlatFrameSlot(state, *execution, frameOffset, frameBase, ip->dstSlot, resumedVal);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AsyncResolve) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const Value* payloadPtr =
                    (ip->inlineArgCount != 0u && ip->src0Slot != detail::InvalidCompactReg)
                        ? &frameBase[ip->src0Slot]
                        : nullptr;
                Value wrapped{};
                if (RS_FLAT_UNLIKELY(!resolveInterpreterCoroutine(
                        state,
                        activeCoroutine,
                        static_cast<semantic::SymbolId>(ip->integerImmediate),
                        payloadPtr,
                        wrapped))) {
                    goto L_FlatError;
                }
                frameBase[ip->dstSlot] = std::move(wrapped);
                RS_FLAT_DISPATCH_NEXT();
            }

            #define RS_TRUSTED_SPAN_CHECK() \
                const std::uint64_t spanCost = ip->span; \
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) { \
                    if (spanCost > 1u) goto L_FlatSuperFallback; \
                    goto L_FlatBudgetExceeded; \
                }

            RS_FLAT_CASE(AddIntTrustedChecked) {
                RS_TRUSTED_SPAN_CHECK();
                const std::int64_t a = frameBase[ip->src0Slot].asInt64Unchecked();
                const std::int64_t b = frameBase[ip->src1Slot].asInt64Unchecked();
                const std::int64_t res = a + b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AddIntTrustedUnchecked) {
                RS_TRUSTED_SPAN_CHECK();
                const std::int64_t a = frameBase[ip->src0Slot].asInt64Unchecked();
                const std::int64_t b = frameBase[ip->src1Slot].asInt64Unchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a + b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubIntTrustedChecked) {
                RS_TRUSTED_SPAN_CHECK();
                const std::int64_t a = frameBase[ip->src0Slot].asInt64Unchecked();
                const std::int64_t b = frameBase[ip->src1Slot].asInt64Unchecked();
                const std::int64_t res = a - b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubIntTrustedUnchecked) {
                RS_TRUSTED_SPAN_CHECK();
                const std::int64_t a = frameBase[ip->src0Slot].asInt64Unchecked();
                const std::int64_t b = frameBase[ip->src1Slot].asInt64Unchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a - b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulIntTrustedChecked) {
                RS_TRUSTED_SPAN_CHECK();
                const std::int64_t a = frameBase[ip->src0Slot].asInt64Unchecked();
                const std::int64_t b = frameBase[ip->src1Slot].asInt64Unchecked();
                const std::int64_t res = a * b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulIntTrustedUnchecked) {
                RS_TRUSTED_SPAN_CHECK();
                const std::int64_t a = frameBase[ip->src0Slot].asInt64Unchecked();
                const std::int64_t b = frameBase[ip->src1Slot].asInt64Unchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a * b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessIntTrusted) {
                RS_TRUSTED_SPAN_CHECK();
                const std::int64_t a = frameBase[ip->src0Slot].asInt64Unchecked();
                const std::int64_t b = frameBase[ip->src1Slot].asInt64Unchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a < b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AddDoubleTrusted) {
                RS_TRUSTED_SPAN_CHECK();
                const double a = frameBase[ip->src0Slot].asDoubleUnchecked();
                const double b = frameBase[ip->src1Slot].asDoubleUnchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(a + b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubDoubleTrusted) {
                RS_TRUSTED_SPAN_CHECK();
                const double a = frameBase[ip->src0Slot].asDoubleUnchecked();
                const double b = frameBase[ip->src1Slot].asDoubleUnchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(a - b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulDoubleTrusted) {
                RS_TRUSTED_SPAN_CHECK();
                const double a = frameBase[ip->src0Slot].asDoubleUnchecked();
                const double b = frameBase[ip->src1Slot].asDoubleUnchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(a * b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(DivDoubleTrusted) {
                RS_TRUSTED_SPAN_CHECK();
                const double a = frameBase[ip->src0Slot].asDoubleUnchecked();
                const double b = frameBase[ip->src1Slot].asDoubleUnchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(a / b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AddFloatTrusted) {
                RS_TRUSTED_SPAN_CHECK();
                const float a = frameBase[ip->src0Slot].asFloatUnchecked();
                const float b = frameBase[ip->src1Slot].asFloatUnchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(a + b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubFloatTrusted) {
                RS_TRUSTED_SPAN_CHECK();
                const float a = frameBase[ip->src0Slot].asFloatUnchecked();
                const float b = frameBase[ip->src1Slot].asFloatUnchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(a - b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulFloatTrusted) {
                RS_TRUSTED_SPAN_CHECK();
                const float a = frameBase[ip->src0Slot].asFloatUnchecked();
                const float b = frameBase[ip->src1Slot].asFloatUnchecked();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(a * b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InitStructSlice) {
                RS_TRUSTED_SPAN_CHECK();
                const std::uint16_t sid = ip->aux0Slot;
                const auto* sc = getFlatStructSchema(sid);
                const std::uint32_t payOff = ip->auxImm32;
                if (RS_FLAT_UNLIKELY(sc == nullptr || state.flatBankId == 0u ||
                                     payOff >= execution->totalFrameSlots ||
                                     frameOffset + payOff > StructStorageHandle::FrameOffsetMask)) {
                    goto L_FlatMathFallback;
                }
                std::memcpy(
                    static_cast<void*>(frameBase + payOff),
                    static_cast<const void*>(sc->prototypeLeaves.data()),
                    static_cast<std::size_t>(ip->aux1Slot) * sizeof(Value));
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                    static_cast<semantic::SymbolId>(ip->integerImmediate),
                    state.flatBankId,
                    static_cast<std::uint32_t>(frameOffset + payOff),
                    sid);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AliasStructSlice) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot] = frameBase[ip->src0Slot];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CopyStructSlice) {
                RS_TRUSTED_SPAN_CHECK();
                const std::uint16_t sid = ip->aux0Slot;
                const std::uint16_t leafCnt = ip->aux1Slot;
                const std::uint32_t dstPayOff = ip->auxImm32;
                if (RS_FLAT_UNLIKELY(state.flatBankId == 0u ||
                                     dstPayOff >= execution->totalFrameSlots ||
                                     frameOffset + dstPayOff > StructStorageHandle::FrameOffsetMask)) {
                    goto L_FlatMathFallback;
                }
                const Value& srcVal = frameBase[ip->src0Slot];
                const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + dstPayOff);
                if (RS_FLAT_LIKELY(srcVal.rawTag() == ValueTag::Struct &&
                                   srcVal.u_.structVal.isFrameSlice() &&
                                   srcVal.u_.structVal.frameSchemaId() == sid &&
                                   srcVal.u_.structVal.storage.frameSliceBank() == state.flatBankId)) {
                    const std::uint32_t srcAbs = srcVal.u_.structVal.storage.frameSliceOffset();
                    if (srcAbs != dstAbs) {
                        std::memmove(
                            static_cast<void*>(state.values.data() + dstAbs),
                            static_cast<const void*>(state.values.data() + srcAbs),
                            static_cast<std::size_t>(leafCnt) * sizeof(Value));
                    }
                } else {
                    const auto* sc = getFlatStructSchema(sid);
                    if (RS_FLAT_UNLIKELY(!sc || !extractStructLeavesToBuffer(srcVal, *sc, frameBase + dstPayOff))) {
                        goto L_FlatMathFallback;
                    }
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                    static_cast<semantic::SymbolId>(ip->integerImmediate),
                    state.flatBankId,
                    dstAbs,
                    sid);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadStructSliceField) {
                if (RS_FLAT_UNLIKELY(localExecuted >= budgetLimit)) goto L_FlatBudgetExceeded;
                const Value& recv = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recv.rawTag() == ValueTag::Struct &&
                                   recv.u_.structVal.isFrameSlice() &&
                                   recv.u_.structVal.storage.frameSliceBank() == state.flatBankId)) {
                    const std::uint32_t baseAbs =
                        recv.u_.structVal.storage.frameSliceOffset() + ip->aux0Slot;
                    const std::uint16_t subSid = ip->aux1Slot;
                    if (subSid == 0u) {
                        ++localExecuted;
                        frameBase[ip->dstSlot] = state.values[baseAbs];
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    const auto subTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                    if (ip->auxImm32 == detail::InvalidStreamOffset) {
                        if (RS_FLAT_UNLIKELY(baseAbs > StructStorageHandle::FrameOffsetMask)) {
                            goto L_FlatGenericFallback;
                        }
                        ++localExecuted;
                        frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                            subTid, state.flatBankId, baseAbs, subSid);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    const auto* subSc = getFlatStructSchema(subSid);
                    if (RS_FLAT_LIKELY(subSc != nullptr && ip->auxImm32 < execution->totalFrameSlots &&
                                       frameOffset + ip->auxImm32 <= StructStorageHandle::FrameOffsetMask)) {
                        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + ip->auxImm32);
                        if (dstAbs != baseAbs) {
                            std::memmove(
                                static_cast<void*>(state.values.data() + dstAbs),
                                static_cast<const void*>(state.values.data() + baseAbs),
                                static_cast<std::size_t>(subSc->flatSlotCount) * sizeof(Value));
                        }
                        ++localExecuted;
                        frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                            subTid, state.flatBankId, dstAbs, subSid);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatGenericFallback;
            }

            RS_FLAT_CASE(StoreStructSliceField) {
                if (RS_FLAT_UNLIKELY(localExecuted >= budgetLimit)) goto L_FlatBudgetExceeded;
                const std::uint16_t sid = ip->aux0Slot;
                const std::uint16_t fIdx = ip->aux1Slot;
                const std::uint32_t dstPayOff = ip->auxImm32;
                const auto* sc = getFlatStructSchema(sid);
                if (RS_FLAT_UNLIKELY(!sc || fIdx >= sc->directFields.size() ||
                                     state.flatBankId == 0u || dstPayOff >= execution->totalFrameSlots ||
                                     frameOffset + dstPayOff > StructStorageHandle::FrameOffsetMask)) {
                    goto L_FlatGenericFallback;
                }
                const Value& recv = frameBase[ip->src0Slot];
                const Value& rhs = frameBase[ip->src1Slot];
                Value* dstLeaves = frameBase + dstPayOff;
                const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + dstPayOff);
                if (RS_FLAT_LIKELY(recv.rawTag() == ValueTag::Struct &&
                                   recv.u_.structVal.isFrameSlice() &&
                                   recv.u_.structVal.frameSchemaId() == sid &&
                                   recv.u_.structVal.storage.frameSliceBank() == state.flatBankId)) {
                    const std::uint32_t srcAbs = recv.u_.structVal.storage.frameSliceOffset();
                    if (srcAbs != dstAbs) {
                        std::memmove(
                            static_cast<void*>(dstLeaves),
                            static_cast<const void*>(state.values.data() + srcAbs),
                            static_cast<std::size_t>(sc->flatSlotCount) * sizeof(Value));
                    }
                } else if (!extractStructLeavesToBuffer(recv, *sc, dstLeaves)) {
                    goto L_FlatGenericFallback;
                }
                const auto& fd = sc->directFields[fIdx];
                if (fd.subSchemaId == 0u) {
                    dstLeaves[fd.leafOffset] = rhs;
                } else {
                    const auto* subSc = getFlatStructSchema(fd.subSchemaId);
                    if (RS_FLAT_UNLIKELY(!subSc ||
                                         !extractStructLeavesToBuffer(rhs, *subSc, dstLeaves + fd.leafOffset))) {
                        goto L_FlatGenericFallback;
                    }
                }
                ++localExecuted;
                frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                    static_cast<semantic::SymbolId>(ip->integerImmediate),
                    state.flatBankId,
                    dstAbs,
                    sid);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreLocalStructFieldInPlace) {
                RS_TRUSTED_SPAN_CHECK();
                const std::uint32_t locPayOff = ip->auxImm32;
                const std::uint16_t leafOff = ip->aux0Slot;
                const std::uint16_t subSid = ip->aux1Slot;
                const Value& rhs = frameBase[ip->src1Slot];
                if (subSid == 0u) {
                    frameBase[locPayOff + leafOff] = rhs;
                    localExecuted += spanCost;
                    RS_FLAT_DISPATCH_NEXT();
                }
                const auto* subSc = getFlatStructSchema(subSid);
                if (RS_FLAT_LIKELY(subSc != nullptr)) {
                    if (RS_FLAT_LIKELY(rhs.rawTag() == ValueTag::Struct &&
                                       rhs.u_.structVal.isFrameSlice() &&
                                       rhs.u_.structVal.frameSchemaId() == subSid &&
                                       rhs.u_.structVal.storage.frameSliceBank() == state.flatBankId)) {
                        const std::uint32_t srcAbs = rhs.u_.structVal.storage.frameSliceOffset();
                        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + locPayOff + leafOff);
                        if (srcAbs != dstAbs) {
                            std::memmove(
                                static_cast<void*>(state.values.data() + dstAbs),
                                static_cast<const void*>(state.values.data() + srcAbs),
                                static_cast<std::size_t>(subSc->flatSlotCount) * sizeof(Value));
                        }
                        localExecuted += spanCost;
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    if (extractStructLeavesToBuffer(rhs, *subSc, frameBase + locPayOff + leafOff)) {
                        localExecuted += spanCost;
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatSuperFallback;
            }

            RS_FLAT_CASE(StoreElementStructFieldInPlace) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatSuperFallback;
                }
                const auto expectedTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (ce.isPacked) {
                    if (RS_FLAT_UNLIKELY(ce.packedView.encoding != PackedElemEncoding::BlittableStruct ||
                                         ce.packedView.elementType != semantic::PrimitiveType::Struct ||
                                         ce.packedView.elementTypeId != expectedTid)) {
                        goto L_FlatSuperFallback;
                    }
                    const std::uint64_t spanCost = ip->span;
                    if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                        goto L_FlatSuperFallback;
                    }
                    std::int64_t idx = 0;
                    if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                         static_cast<std::uint64_t>(idx) >= ce.packedView.length)) {
                        goto L_FlatSuperFallback;
                    }
                    const auto* rootSc = getFlatStructSchema(static_cast<std::uint16_t>(ip->auxImm32));
                    bool needsDemote = false;
                    if (RS_FLAT_UNLIKELY(!rootSc ||
                                         !writePackedStructLeafSlice(
                                             ce.packedView.rawData,
                                             static_cast<std::size_t>(idx),
                                             ce.packedView.blittableSchemaId,
                                             *rootSc,
                                             ip->aux0Slot,
                                             ip->aux1Slot,
                                             frameBase[ip->src2Slot],
                                             needsDemote))) {
                        ce.valid = false;
                        goto L_FlatSuperFallback;
                    }
                    localExecuted += spanCost;
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_UNLIKELY(!ce.isFlatStruct ||
                                     ce.flatStructView.elementTypeId != expectedTid ||
                                     ce.flatStructView.flatSchemaId != static_cast<std::uint16_t>(ip->auxImm32))) {
                    goto L_FlatSuperFallback;
                }
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.flatStructView.length)) {
                    goto L_FlatSuperFallback;
                }
                const auto uIdx = static_cast<std::size_t>(idx);
                const std::size_t stride = ce.flatStructView.flatStride;
                const std::uint16_t leafOff = ip->aux0Slot;
                const std::uint16_t subSid = ip->aux1Slot;
                if (RS_FLAT_UNLIKELY(leafOff >= stride)) {
                    goto L_FlatSuperFallback;
                }
                Value* dstLeaf = ce.flatStructView.data + uIdx * stride + leafOff;
                const Value& rhs = frameBase[ip->src2Slot];
                std::size_t writtenCount = 1;
                if (subSid == 0u) {
                    *dstLeaf = detachFrameSliceValue(rhs);
                } else {
                    const auto* subSc = getFlatStructSchema(subSid);
                    if (RS_FLAT_UNLIKELY(!subSc ||
                                         leafOff + subSc->flatSlotCount > stride ||
                                         !extractStructLeavesToBuffer(rhs, *subSc, dstLeaf))) {
                        goto L_FlatSuperFallback;
                    }
                    writtenCount = subSc->flatSlotCount;
                }
                if (ce.flatStructView.containsPoolValues) {
                    for (std::size_t k = 0; k < writtenCount; ++k) {
                        pinValueInPools(dstLeaf[k]);
                    }
                }
                if (ce.flatStructView.containsManagedReferences) {
                    if (ce.flatStructView.dirtyCards != nullptr) {
                        ce.flatStructView.dirtyCards[uIdx >> ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (RS_FLAT_UNLIKELY(ce.flatStructView.needsRememberedBarrier ||
                                         gcActive ||
                                         state.heap->phase() != GcPhase::Idle)) {
                        state.heap->recordFlatStructArrayWriteBarrier(ce.ref, uIdx);
                        ce.flatStructView.needsRememberedBarrier = false;
                    }
                }
                localExecuted += spanCost;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InitStackRefCell) {
                if (RS_FLAT_UNLIKELY(localExecuted >= budgetLimit)) goto L_FlatBudgetExceeded;
                const auto& mirror = detail::executionMirror(*state.program);
                if (RS_FLAT_UNLIKELY(state.flatBankId == 0u ||
                                     ip->aux0Slot >= mirror.stackRefTemplates.size() ||
                                     ip->auxImm32 >= execution->totalFrameSlots)) {
                    goto L_FlatGenericFallback;
                }
                const auto& srt = mirror.stackRefTemplates[ip->aux0Slot];
                const std::uint32_t cellOff = ip->auxImm32;
                if (srt.flatSchemaId == 0u) {
                    frameBase[cellOff] = srt.scalarDefaultValue;
                } else {
                    const auto* sc = getFlatStructSchema(srt.flatSchemaId);
                    if (RS_FLAT_UNLIKELY(!sc || cellOff + sc->flatSlotCount > execution->totalFrameSlots)) {
                        goto L_FlatGenericFallback;
                    }
                    std::memcpy(
                        static_cast<void*>(frameBase + cellOff),
                        static_cast<const void*>(sc->prototypeLeaves.data()),
                        static_cast<std::size_t>(sc->flatSlotCount) * sizeof(Value));
                }
                ++localExecuted;
                frameBase[ip->dstSlot] = ObjectRef::makeStackSlotRef(
                    static_cast<std::uint32_t>(frameOffset + cellOff),
                    state.flatBankId,
                    srt.flatSchemaId);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(BindLocalStackRef) {
                RS_TRUSTED_SPAN_CHECK();
                if (RS_FLAT_UNLIKELY(state.flatBankId == 0u || ip->auxImm32 >= execution->totalFrameSlots)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = ObjectRef::makeStackSlotRef(
                    static_cast<std::uint32_t>(frameOffset + ip->auxImm32),
                    state.flatBankId,
                    ip->aux0Slot);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(WritebackLocalStackRef) {
                RS_TRUSTED_SPAN_CHECK();
                const Value& wrapVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(wrapVal.rawTag() == ValueTag::ObjectRef &&
                                   wrapVal.u_.objectRef.isStackSlotRef() &&
                                   wrapVal.u_.objectRef.generation == state.flatBankId &&
                                   wrapVal.u_.objectRef.slot ==
                                       static_cast<std::uint32_t>(frameOffset + ip->auxImm32))) {
                    localExecuted += spanCost;
                    RS_FLAT_DISPATCH_NEXT();
                }
                goto L_FlatSuperFallback;
            }

            RS_FLAT_CASE(LoadStackRefCell) {
                if (RS_FLAT_UNLIKELY(localExecuted >= budgetLimit)) goto L_FlatBudgetExceeded;
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef &&
                                   recvVal.u_.objectRef.isStackSlotRef() &&
                                   recvVal.u_.objectRef.generation == state.flatBankId)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    const std::uint16_t sid = ip->aux0Slot;
                    if (sid == 0u) {
                        ++localExecuted;
                        frameBase[ip->dstSlot] = state.values[ref.slot];
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    const auto innerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                    if (ip->auxImm32 == detail::InvalidStreamOffset) {
                        if (RS_FLAT_UNLIKELY(ref.slot > StructStorageHandle::FrameOffsetMask)) {
                            goto L_FlatMathFallback;
                        }
                        ++localExecuted;
                        frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                            innerTid, state.flatBankId, ref.slot, sid);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    const auto* sc = getFlatStructSchema(sid);
                    if (RS_FLAT_LIKELY(sc != nullptr && ip->auxImm32 < execution->totalFrameSlots &&
                                       frameOffset + ip->auxImm32 <= StructStorageHandle::FrameOffsetMask)) {
                        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + ip->auxImm32);
                        if (dstAbs != ref.slot) {
                            std::memmove(
                                static_cast<void*>(state.values.data() + dstAbs),
                                static_cast<const void*>(state.values.data() + ref.slot),
                                static_cast<std::size_t>(sc->flatSlotCount) * sizeof(Value));
                        }
                        ++localExecuted;
                        frameBase[ip->dstSlot] = StructValue::makeFrameSlice(
                            innerTid, state.flatBankId, dstAbs, sid);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(StoreStackRefCell) {
                if (RS_FLAT_UNLIKELY(localExecuted >= budgetLimit)) goto L_FlatBudgetExceeded;
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef &&
                                   recvVal.u_.objectRef.isStackSlotRef() &&
                                   recvVal.u_.objectRef.generation == state.flatBankId)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    const Value& rhs = frameBase[ip->src1Slot];
                    const std::uint16_t sid = ip->aux0Slot;
                    if (sid == 0u) {
                        state.values[ref.slot] = detachFrameSliceValue(rhs);
                        ++localExecuted;
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    const auto* sc = getFlatStructSchema(sid);
                    if (RS_FLAT_LIKELY(sc != nullptr)) {
                        if (RS_FLAT_LIKELY(rhs.rawTag() == ValueTag::Struct &&
                                           rhs.u_.structVal.isFrameSlice() &&
                                           rhs.u_.structVal.frameSchemaId() == sid &&
                                           rhs.u_.structVal.storage.frameSliceBank() == state.flatBankId)) {
                            const std::uint32_t srcAbs = rhs.u_.structVal.storage.frameSliceOffset();
                            if (srcAbs != ref.slot) {
                                std::memmove(
                                    static_cast<void*>(state.values.data() + ref.slot),
                                    static_cast<const void*>(state.values.data() + srcAbs),
                                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(Value));
                            }
                            ++localExecuted;
                            RS_FLAT_DISPATCH_NEXT();
                        }
                        if (extractStructLeavesToBuffer(rhs, *sc, state.values.data() + ref.slot)) {
                            ++localExecuted;
                            RS_FLAT_DISPATCH_NEXT();
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(StoreStackRefStructFieldInPlace) {
                RS_TRUSTED_SPAN_CHECK();
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef &&
                                   recvVal.u_.objectRef.isStackSlotRef() &&
                                   recvVal.u_.objectRef.generation == state.flatBankId &&
                                   recvVal.u_.objectRef.stackRefSchemaId() ==
                                       static_cast<std::uint16_t>(ip->auxImm32))) {
                    const std::uint32_t dstAbs = recvVal.u_.objectRef.slot + ip->aux0Slot;
                    const std::uint16_t subSid = ip->aux1Slot;
                    const Value& rhs = frameBase[ip->src1Slot];
                    if (subSid == 0u) {
                        state.values[dstAbs] = detachFrameSliceValue(rhs);
                        localExecuted += spanCost;
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    const auto* subSc = getFlatStructSchema(subSid);
                    if (RS_FLAT_LIKELY(subSc != nullptr)) {
                        if (RS_FLAT_LIKELY(rhs.rawTag() == ValueTag::Struct &&
                                           rhs.u_.structVal.isFrameSlice() &&
                                           rhs.u_.structVal.frameSchemaId() == subSid &&
                                           rhs.u_.structVal.storage.frameSliceBank() == state.flatBankId)) {
                            const std::uint32_t srcAbs = rhs.u_.structVal.storage.frameSliceOffset();
                            if (srcAbs != dstAbs) {
                                std::memmove(
                                    static_cast<void*>(state.values.data() + dstAbs),
                                    static_cast<const void*>(state.values.data() + srcAbs),
                                    static_cast<std::size_t>(subSc->flatSlotCount) * sizeof(Value));
                            }
                            localExecuted += spanCost;
                            RS_FLAT_DISPATCH_NEXT();
                        }
                        if (extractStructLeavesToBuffer(rhs, *subSc, state.values.data() + dstAbs)) {
                            localExecuted += spanCost;
                            RS_FLAT_DISPATCH_NEXT();
                        }
                    }
                }
                goto L_FlatSuperFallback;
            }

            RS_FLAT_CASE(GenericBytecode) {
            L_FlatMathFallback:
                if (RS_FLAT_UNLIKELY(ip->span > 1)) goto L_FlatSuperFallback;
            L_FlatGenericFallback:
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                if (RS_FLAT_UNLIKELY(cold.inlinedCallee != nullptr)) {
                    const auto& cLoc = *cold.inlinedCallee;
                    const auto& cFunc = *cLoc.function;
                    const auto& cExec = *cold.directExecution;
                    const std::size_t subOff = frameOffset + cold.inlineFrameBias;
                    const ValueWindow argWin{&state.values, subOff + cExec.paramOffset, cFunc.parameterTypes.size()};
                    const ValueWindow locWin{&state.values, subOff + cExec.localOffset, cFunc.localTypes.size()};
                    const ValueWindow regWin{&state.values, subOff + cExec.registerOffset, cFunc.registerTypes.size()};
                    state.stack.push_back(&cLoc.qualifiedName);
                    const bool ok = executeInstruction(
                        state, *cLoc.module, cFunc, *cold.bytecode, argWin, locWin, regWin);
                    frameBase = state.values.data() + frameOffset;
                    syncAfterFlatSubcall();
                    if (RS_FLAT_UNLIKELY(!ok)) {
                        if (!state.pendingException) {
                            refineInterpreterFrameSourceLocation(
                                state,
                                cLoc,
                                cExec.blocks[cold.blockIndex].bytecode->id,
                                cold.instructionIndexInBlock,
                                ip->span,
                                false);
                        }
                        state.stack.pop_back();
                        if (!state.pendingException) {
                            refineInterpreterFrameSourceLocation(
                                state,
                                location,
                                execution->blocks[cold.callerBlockIndex].bytecode->id,
                                cold.callerInstIndex,
                                1,
                                false);
                        }
                        state.valueTop = frameOffset;
                        state.stack.pop_back();
                        return false;
                    }
                    state.stack.pop_back();
                    RS_FLAT_DISPATCH_NEXT();
                }
                const ValueWindow argWin{&state.values, frameOffset + execution->paramOffset, argCount};
                const ValueWindow locWin{&state.values, frameOffset + execution->localOffset, function.localTypes.size()};
                const ValueWindow regWin{&state.values, frameOffset + execution->registerOffset, function.registerTypes.size()};
                const bool ok = executeInstruction(
                    state, *location.module, function, *cold.bytecode, argWin, locWin, regWin);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                if (RS_FLAT_UNLIKELY(execution->hasStructPayloads && cold.bytecode != nullptr &&
                                     cold.bytecode->result < function.registerTypes.size())) {
                    const auto rSlot = static_cast<std::uint16_t>(execution->registerOffset + cold.bytecode->result);
                    if (execution->slotStructSchemaIds[rSlot] != 0u &&
                        execution->slotPayloadOffsets[rSlot] != detail::InvalidCompactReg) {
                        const Value cur = frameBase[rSlot];
                        storeValueIntoFlatFrameSlot(state, *execution, frameOffset, frameBase, rSlot, cur);
                    }
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(TermJumpNoArgs) {
                const std::uint64_t hopCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || hopCost > budgetLimit - localExecuted)) {
                    if (hopCost > 1u) {
                        // Step first hop if 1 budget unit remains
                        if (localExecuted < budgetLimit) {
                            ++localExecuted;
                            ++state.statistics.branchesTaken;
                            ip = streamBase + ip->auxImm32;
                        }
                    }
                    goto L_FlatBudgetExceeded;
                }
                localExecuted += hopCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.statistics.branchesTaken += hopCost;
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(TermBranchBoolNoArgs) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const bool cond = frameBase[ip->src0Slot].asBoolUnchecked();
                ++state.statistics.branchesTaken;
                ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(TermBranchGeneric) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const auto& cold = flatColdBase[ip - streamBase];
                const auto& blk = execution->blocks[cold.blockIndex];
                const auto& terminator = blk.bytecode->terminator;
                const detail::ExecutionEdge* edge = &blk.branch;
                std::uint32_t nextStreamOffset = ip->trueTargetOffset;
                if (terminator.kind == bytecode::TerminatorKind::Branch) {
                    const std::size_t condReg = terminator.condition;
                    if (RS_FLAT_UNLIKELY(condReg >= function.registerTypes.size() ||
                        !expectType(state, frameBase[execution->registerOffset + condReg],
                            semantic::PrimitiveType::Bool, "branch"))) {
                        state.executed = localExecuted;
                        goto L_FlatError;
                    }
                    if (!frameBase[execution->registerOffset + condReg].asBoolUnchecked()) {
                        edge = &blk.falseBranch;
                        nextStreamOffset = ip->falseTargetOffset;
                    }
                } else if (RS_FLAT_UNLIKELY(terminator.kind != bytecode::TerminatorKind::Jump)) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::InvalidProgram, "block has invalid terminator");
                    goto L_FlatError;
                }
                ++state.statistics.branchesTaken;
                if (RS_FLAT_UNLIKELY(edge->target == detail::InvalidBlockIndex ||
                    !edge->sources || !edge->destinations ||
                    edge->sources->size() != edge->destinations->size())) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::InvalidProgram, "branch target or argument count is invalid");
                    goto L_FlatError;
                }
                if (edge->hasArguments) {
                    const auto& sources = *edge->sources;
                    const auto& destinations = *edge->destinations;
                    if (RS_FLAT_UNLIKELY(sources.size() > execution->edgeScratchCount)) {
                        state.executed = localExecuted;
                        fail(state, ErrorCode::InvalidProgram, "branch scratch space is too small");
                        goto L_FlatError;
                    }
                    Value* const regPtr = frameBase + execution->registerOffset;
                    Value* const scratchPtr = frameBase + execution->scratchOffset;
                    const std::size_t regLimit = function.registerTypes.size();
                    if (RS_FLAT_UNLIKELY(execution->hasStructPayloads &&
                                         execution->edgeStructScratchOffset != detail::InvalidCompactReg &&
                                         state.flatBankId != 0u)) {
                        std::size_t edgePayCursor = execution->edgeStructScratchOffset;
                        for (std::size_t idx = 0; idx < sources.size(); ++idx) {
                            if (RS_FLAT_UNLIKELY(sources[idx] >= regLimit || destinations[idx].target >= regLimit)) {
                                state.executed = localExecuted;
                                fail(state, ErrorCode::InvalidProgram, "branch argument register is invalid");
                                goto L_FlatError;
                            }
                            const auto dstSlot = static_cast<std::uint16_t>(execution->registerOffset + destinations[idx].target);
                            const std::uint16_t sid = execution->slotStructSchemaIds[dstSlot];
                            if (sid != 0u) {
                                const auto* sc = getFlatStructSchema(sid);
                                if (sc != nullptr) {
                                    (void)extractStructLeavesToBuffer(regPtr[sources[idx]], *sc, frameBase + edgePayCursor);
                                    scratchPtr[idx] = StructValue::makeFrameSlice(
                                        sc->structTypeId,
                                        state.flatBankId,
                                        static_cast<std::uint32_t>(frameOffset + edgePayCursor),
                                        sid);
                                    edgePayCursor += sc->flatSlotCount;
                                    continue;
                                }
                            }
                            scratchPtr[idx] = regPtr[sources[idx]];
                        }
                        for (std::size_t idx = 0; idx < sources.size(); ++idx) {
                            const auto dstSlot = static_cast<std::uint16_t>(execution->registerOffset + destinations[idx].target);
                            if (execution->slotStructSchemaIds[dstSlot] != 0u) {
                                storeValueIntoFlatFrameSlot(
                                    state, *execution, frameOffset, frameBase, dstSlot, scratchPtr[idx]);
                            } else {
                                regPtr[destinations[idx].target] = std::move(scratchPtr[idx]);
                            }
                        }
                    } else {
                        for (std::size_t idx = 0; idx < sources.size(); ++idx) {
                            if (RS_FLAT_UNLIKELY(sources[idx] >= regLimit || destinations[idx].target >= regLimit)) {
                                state.executed = localExecuted;
                                fail(state, ErrorCode::InvalidProgram, "branch argument register is invalid");
                                goto L_FlatError;
                            }
                            scratchPtr[idx] = regPtr[sources[idx]];
                        }
                        for (std::size_t idx = 0; idx < sources.size(); ++idx) {
                            regPtr[destinations[idx].target] = std::move(scratchPtr[idx]);
                        }
                    }
                }
                ip = streamBase + nextStreamOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(TermReturnVoid) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                result = std::monostate{};
                state.executed = localExecuted;
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return true;
            }

            RS_FLAT_CASE(TermReturnScalarFast) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                result = frameBase[ip->src0Slot];
                state.executed = localExecuted;
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return true;
            }

            RS_FLAT_CASE(TermReturnGeneric) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const auto& cold = flatColdBase[ip - streamBase];
                const auto& terminator = execution->blocks[cold.blockIndex].bytecode->terminator;
                if (RS_FLAT_UNLIKELY(terminator.value >= function.registerTypes.size())) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::InvalidProgram, "return register is invalid");
                    goto L_FlatError;
                }
                const Value& retVal = frameBase[execution->registerOffset + terminator.value];
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!expectSignatureType(state, retVal, function.returnType, function.returnTypeId, "return value"))) {
                    goto L_FlatError;
                }
                if (callerStructReturnSchemaId != 0u &&
                    callerStructReturnAbsOffset != detail::InvalidStreamOffset &&
                    state.flatBankId != 0u) {
                    if (const auto* sc = getFlatStructSchema(callerStructReturnSchemaId);
                        sc != nullptr &&
                        extractStructLeavesToBuffer(retVal, *sc, state.values.data() + callerStructReturnAbsOffset)) {
                        result = StructValue::makeFrameSlice(
                            sc->structTypeId,
                            state.flatBankId,
                            callerStructReturnAbsOffset,
                            callerStructReturnSchemaId);
                        state.valueTop = frameOffset;
                        state.stack.pop_back();
                        return true;
                    }
                }
                result = detachFrameSliceValue(retVal);
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return true;
            }

            RS_FLAT_CASE(TermThrow) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                const auto& terminator = execution->blocks[cold.blockIndex].bytecode->terminator;
                if (RS_FLAT_UNLIKELY(terminator.value >= function.registerTypes.size() ||
                    !std::holds_alternative<ObjectRef>(frameBase[execution->registerOffset + terminator.value]))) {
                    fail(state, ErrorCode::NullReference, "throw requires a non-null script object");
                    goto L_FlatError;
                }
                state.pendingException = frameBase[execution->registerOffset + terminator.value];
                goto L_FlatError;
            }

#if !(defined(__GNUC__) || defined(__clang__))
            default:
                goto L_FlatError;
            }
#endif

L_FlatSuperFallback: {
        // When instruction budget is exhausted mid-superinstruction (or overflow occurs in a folded op),
        // step the underlying single instructions one by one for exact instruction/location parity.
        if ((ip->arithFlags & detail::FlatInstruction::FlagPreJump1) != 0u) {
            if (localExecuted >= budgetLimit) {
                goto L_FlatBudgetExceeded;
            }
            ++localExecuted;
            ++state.statistics.branchesTaken;
            ip = streamBase + ip->auxImm32;
        }
        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
        state.executed = localExecuted;
        const auto& cold = flatColdBase[ip - streamBase];
        const bool isInlined = (cold.inlinedCallee != nullptr);
        const ProgramImage::FunctionLocation& activeLoc = isInlined ? *cold.inlinedCallee : location;
        const bytecode::Function& activeFunc = *activeLoc.function;
        const detail::ExecutionFunction& activeExec = isInlined ? *cold.directExecution : *execution;
        const std::size_t activeFrameOff = isInlined ? (frameOffset + cold.inlineFrameBias) : frameOffset;
        const std::size_t activeArgCount = isInlined ? activeFunc.parameterTypes.size() : argCount;
        const auto& blk = activeExec.blocks[cold.blockIndex];
        const ValueWindow argWin{&state.values, activeFrameOff + activeExec.paramOffset, activeArgCount};
        const ValueWindow locWin{&state.values, activeFrameOff + activeExec.localOffset, activeFunc.localTypes.size()};
        const ValueWindow regWin{&state.values, activeFrameOff + activeExec.registerOffset, activeFunc.registerTypes.size()};
        const std::size_t startIdx = cold.instructionIndexInBlock;
        const bool includesBranch =
            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalRegBranch ||
            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalConstBranch ||
            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalArrayLengthBranch;
        const bool includesJump =
            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump;
        const std::size_t instSpan = (includesBranch || includesJump)
            ? static_cast<std::size_t>(ip->span - 1)
            : static_cast<std::size_t>(ip->span);
        if (isInlined) {
            state.stack.push_back(&activeLoc.qualifiedName);
        }
        std::size_t executedSubK = 0;
        for (std::size_t k = 0; k < instSpan; ++k) {
            executedSubK = k;
            const auto& singleDec = blk.instructions[startIdx + k];
            const bool ok = (singleDec.opcode == detail::QuickOpcode::Generic ||
                             singleDec.opcode >= detail::QuickOpcode::ConstantDouble)
                ? executeInstruction(state, *activeLoc.module, activeFunc, *singleDec.bytecode, argWin, locWin, regWin)
                : executeQuickInstruction(state, *activeLoc.module, singleDec, argWin, locWin, regWin);
            frameBase = state.values.data() + frameOffset;
            if (ok && activeExec.hasStructPayloads && singleDec.bytecode != nullptr) {
                Value* activeFrameBase = state.values.data() + activeFrameOff;
                if (singleDec.bytecode->opcode == bytecode::Opcode::StoreLocal &&
                    singleDec.bytecode->index < activeFunc.localTypes.size()) {
                    const auto lSlot = static_cast<std::uint16_t>(activeExec.localOffset + singleDec.bytecode->index);
                    if (activeExec.slotStructSchemaIds[lSlot] != 0u) {
                        const Value cur = activeFrameBase[lSlot];
                        storeValueIntoFlatFrameSlot(state, activeExec, activeFrameOff, activeFrameBase, lSlot, cur);
                    }
                } else if (singleDec.result < activeFunc.registerTypes.size()) {
                    const auto rSlot = static_cast<std::uint16_t>(activeExec.registerOffset + singleDec.result);
                    if (activeExec.slotStructSchemaIds[rSlot] != 0u &&
                        activeExec.slotPayloadOffsets[rSlot] != detail::InvalidCompactReg) {
                        const Value cur = activeFrameBase[rSlot];
                        storeValueIntoFlatFrameSlot(state, activeExec, activeFrameOff, activeFrameBase, rSlot, cur);
                    }
                }
            }
            if (!ok) {
                syncAfterFlatSubcall();
                if (isInlined) {
                    if (!state.pendingException) {
                        refineInterpreterFrameSourceLocation(
                            state,
                            activeLoc,
                            blk.bytecode->id,
                            static_cast<std::uint32_t>(startIdx + executedSubK),
                            1,
                            false);
                    }
                    state.stack.pop_back();
                    if (!state.pendingException) {
                        refineInterpreterFrameSourceLocation(
                            state,
                            location,
                            execution->blocks[cold.callerBlockIndex].bytecode->id,
                            cold.callerInstIndex,
                            1,
                            false);
                    }
                    if (state.pendingException && execution->hasExceptionHandlers) {
                        const auto protectedBlockId = execution->blocks[cold.callerBlockIndex].bytecode->id;
                        const auto handlerOffset = routeFastException(protectedBlockId);
                        if (handlerOffset != detail::InvalidStreamOffset) {
                            ip = streamBase + handlerOffset;
                            RS_FLAT_DISPATCH_AT_IP();
                        }
                    }
                    state.valueTop = frameOffset;
                    state.stack.pop_back();
                    return false;
                }
                if (!state.pendingException) {
                    refineInterpreterFrameSourceLocation(
                        state,
                        location,
                        blk.bytecode->id,
                        static_cast<std::uint32_t>(startIdx + executedSubK),
                        1,
                        false);
                    state.valueTop = frameOffset;
                    state.stack.pop_back();
                    return false;
                }
                goto L_FlatError;
            }
        }
        if (isInlined) {
            state.stack.pop_back();
        }
        syncAfterFlatSubcall();
        if (includesBranch) {
            if (++localExecuted > budgetLimit) goto L_FlatBudgetExceeded;
            if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
            const bool cond = frameBase[activeFrameOff - frameOffset + activeExec.registerOffset + blk.terminatorReg].asBoolUnchecked();
            ++state.statistics.branchesTaken;
            ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
            RS_FLAT_DISPATCH_AT_IP();
        }
        if (includesJump) {
            if (++localExecuted > budgetLimit) goto L_FlatBudgetExceeded;
            if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
            ++state.statistics.branchesTaken;
            ip = streamBase + ip->trueTargetOffset;
            RS_FLAT_DISPATCH_AT_IP();
        }
        RS_FLAT_DISPATCH_NEXT();
    }

L_FlatBudgetExceeded:
        localExecuted = budgetLimit;
        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
        state.executed = budgetLimit;
        fail(state, ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        goto L_FlatError;

L_FlatTypeMismatch:
        state.executed = localExecuted;
        fail(state, ErrorCode::TypeMismatch, "expected int operand in flat quick instruction");
        goto L_FlatError;

L_FlatError:
        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
        if (ip >= streamBase && ip < streamBase + execution->flatStream.size()) {
            const auto& cold = flatColdBase[ip - streamBase];
            if (cold.inlinedCallee != nullptr) {
                if (state.pendingException && execution->hasExceptionHandlers) {
                    const auto protectedBlockId = execution->blocks[cold.callerBlockIndex].bytecode->id;
                    const auto handlerOffset = routeFastException(protectedBlockId);
                    if (handlerOffset != detail::InvalidStreamOffset) {
                        ip = streamBase + handlerOffset;
                        RS_FLAT_DISPATCH_AT_IP();
                    }
                }
                if (!state.pendingException) {
                    if (cold.callerBlockIndex < execution->blocks.size()) {
                        refineInterpreterFrameSourceLocation(
                            state,
                            location,
                            execution->blocks[cold.callerBlockIndex].bytecode->id,
                            cold.callerInstIndex,
                            1,
                            false);
                    }
                    const auto& cLoc = *cold.inlinedCallee;
                    const auto& cExec = *cold.directExecution;
                    state.stack.push_back(&cLoc.qualifiedName);
                    state.error.stackTrace.insert(state.error.stackTrace.begin(), cLoc.qualifiedName);
                    StackTraceLocation calleeTraceLoc;
                    calleeTraceLoc.function = cLoc.qualifiedName;
                    if (cLoc.function) {
                        calleeTraceLoc.symbolId = cLoc.function->symbolId;
                        const auto& dbg = cLoc.function->debugInfo;
                        if (!dbg.sequencePoints.empty() && cLoc.module) {
                            const auto* activeSp = &dbg.sequencePoints.front();
                            for (const auto& src : cLoc.module->sourceFiles) {
                                if (src.id == activeSp->range.fileId) {
                                    calleeTraceLoc.sourcePath = src.path;
                                    break;
                                }
                            }
                            calleeTraceLoc.sourceLine =
                                static_cast<std::uint32_t>(activeSp->range.start.line + 1);
                            calleeTraceLoc.sourceColumn =
                                static_cast<std::uint32_t>(activeSp->range.start.column + 1);
                        }
                    }
                    state.error.sourceLocations.insert(state.error.sourceLocations.begin(), calleeTraceLoc);
                    state.error.symbolizedStackTrace.insert(
                        state.error.symbolizedStackTrace.begin(), calleeTraceLoc.format());
                    if (cold.blockIndex < cExec.blocks.size()) {
                        const bool isTerm =
                            ip->flatOpcode >= detail::FlatOpcode::TermJumpNoArgs ||
                            (ip->flatOpcode >= detail::FlatOpcode::InlineReturnVoid &&
                             ip->flatOpcode <= detail::FlatOpcode::InlineReturnScalarJump) ||
                            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalRegBranch ||
                            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalConstBranch ||
                            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalArrayLengthBranch ||
                            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
                            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
                            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump;
                        if (!isTerm) {
                            refineInterpreterFrameSourceLocation(
                                state,
                                cLoc,
                                cExec.blocks[cold.blockIndex].bytecode->id,
                                cold.instructionIndexInBlock,
                                ip->span,
                                false);
                        }
                    }
                    state.stack.pop_back();
                }
                if (execution->isAsyncFunction && state.pendingException.has_value()) {
                    const bool resolved = finishUncaughtInterpreterAsyncException(
                        state, function, activeCoroutine, result);
                    state.valueTop = frameOffset;
                    state.stack.pop_back();
                    return resolved;
                }
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return false;
            }
            if (state.pendingException && execution->hasExceptionHandlers) {
                const auto protectedBlockId = execution->blocks[cold.blockIndex].bytecode->id;
                const auto handlerOffset = routeFastException(protectedBlockId);
                if (handlerOffset != detail::InvalidStreamOffset) {
                    ip = streamBase + handlerOffset;
                    RS_FLAT_DISPATCH_AT_IP();
                }
            }
            if (!state.pendingException && cold.blockIndex < execution->blocks.size()) {
                const bool isTerm =
                    ip->flatOpcode >= detail::FlatOpcode::TermJumpNoArgs ||
                    ip->flatOpcode == detail::FlatOpcode::SuperLessLocalRegBranch ||
                    ip->flatOpcode == detail::FlatOpcode::SuperLessLocalConstBranch ||
                    ip->flatOpcode == detail::FlatOpcode::SuperLessLocalArrayLengthBranch ||
                    ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
                    ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
                    ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump;
                if (!isTerm) {
                    refineInterpreterFrameSourceLocation(
                        state,
                        location,
                        execution->blocks[cold.blockIndex].bytecode->id,
                        cold.instructionIndexInBlock,
                        ip->span,
                        false);
                }
            }
        }
        if (execution->isAsyncFunction && state.pendingException.has_value()) {
            const bool resolved = finishUncaughtInterpreterAsyncException(
                state, function, activeCoroutine, result);
            state.valueTop = frameOffset;
            state.stack.pop_back();
            return resolved;
        }
        state.valueTop = frameOffset;
        state.stack.pop_back();
        return false;

#if !(defined(__GNUC__) || defined(__clang__))
        }
#endif

        #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic pop
        #endif
        #undef RS_FLAT_DISPATCH_NEXT
        #undef RS_FLAT_DISPATCH_AT_IP
        #undef RS_FLAT_CASE
        #undef RS_FLAT_LIKELY
        #undef RS_FLAT_UNLIKELY
    }

    return executeBlockVmFunction(
        state,
        location,
        *execution,
        arguments,
        activeCoroutine,
        activeCoroutineFrame,
        result);
}

} // namespace realscript::runtime::interp_detail
