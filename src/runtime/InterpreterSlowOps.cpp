#include "InterpreterInternal.h"

namespace realscript::runtime::interp_detail {

std::vector<std::string> materializeStack(
    const std::vector<const std::string*>& stack) {
    std::vector<std::string> result;
    result.reserve(stack.size());
    for (const auto* frame : stack) {
        result.push_back(frame ? *frame : std::string{});
    }
    return result;
}

void populateInterpreterSourceLocations(State& state) {
    state.error.sourceLocations.clear();
    state.error.symbolizedStackTrace.clear();
    state.error.sourceLocations.reserve(state.error.stackTrace.size());
    state.error.symbolizedStackTrace.reserve(state.error.stackTrace.size());
    for (std::size_t idx = 0; idx < state.error.stackTrace.size(); ++idx) {
        const auto& frameName = state.error.stackTrace[idx];
        StackTraceLocation loc;
        loc.function = frameName;
        if (state.program) {
            if (const auto symbolId = state.program->findFunction(frameName)) {
                loc.symbolId = *symbolId;
                if (const auto* fnLoc = state.program->resolveFunction(*symbolId)) {
                    const auto& dbg = fnLoc->function->debugInfo;
                    const debug::SequencePoint* activeSp = nullptr;
                    if (idx < state.debugFrames.size()) {
                        const auto& dbgFrame =
                            state.debugFrames[state.debugFrames.size() - 1 - idx];
                        if (dbgFrame.function == fnLoc->function && dbgFrame.point) {
                            activeSp = dbgFrame.point;
                        }
                    }
                    if (!activeSp && !dbg.sequencePoints.empty()) {
                        activeSp = &dbg.sequencePoints.front();
                    }
                    if (activeSp && fnLoc->module) {
                        for (const auto& src : fnLoc->module->sourceFiles) {
                            if (src.id == activeSp->range.fileId) {
                                loc.sourcePath = src.path;
                                break;
                            }
                        }
                        loc.sourceLine =
                            static_cast<std::uint32_t>(activeSp->range.start.line + 1);
                        loc.sourceColumn =
                            static_cast<std::uint32_t>(activeSp->range.start.column + 1);
                    }
                }
            }
        }
        state.error.symbolizedStackTrace.push_back(loc.format());
        state.error.sourceLocations.push_back(std::move(loc));
    }
}

void refineInterpreterFrameSourceLocation(
    State& state,
    const ProgramImage::FunctionLocation& location,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    std::uint32_t span,
    bool isTerminator) {
    if (state.stack.empty() ||
        state.error.sourceLocations.size() < state.stack.size() ||
        state.error.symbolizedStackTrace.size() != state.error.sourceLocations.size()) {
        return;
    }
    const std::size_t traceIdx = state.error.sourceLocations.size() - state.stack.size();
    auto& loc = state.error.sourceLocations[traceIdx];
    if (!location.function || !location.module) return;
    const auto& dbg = location.function->debugInfo;
    if (dbg.sequencePoints.empty()) return;

    const std::uint32_t maxInstIdx =
        instructionIndex + (span > 1 ? span - 1 : 0);
    const debug::SequencePoint* bestSp = nullptr;
    for (const auto& sp : dbg.sequencePoints) {
        if (sp.blockId != blockId) continue;
        if (isTerminator && sp.terminator) {
            bestSp = &sp;
            break;
        }
        if (!sp.terminator && sp.instructionIndex <= maxInstIdx) {
            bestSp = &sp;
        }
    }
    if (!bestSp) {
        bestSp = &dbg.sequencePoints.front();
    }
    for (const auto& src : location.module->sourceFiles) {
        if (src.id == bestSp->range.fileId) {
            loc.sourcePath = src.path;
            break;
        }
    }
    loc.sourceLine = static_cast<std::uint32_t>(bestSp->range.start.line + 1);
    loc.sourceColumn = static_cast<std::uint32_t>(bestSp->range.start.column + 1);
    state.error.symbolizedStackTrace[traceIdx] = loc.format();
}

void emitTrace(
    State& state,
    TraceEventKind kind,
    DeterminismOperationId operationId,
    std::string_view operation) {
    if (!state.traceEventsEnabled) return;
    if (state.determinismEventsEnabled) {
        state.determinismEventDigest = accumulateDeterminismEvent(
            state.determinismEventDigest,
            kind,
            state.currentFunctionId,
            operationId,
            state.executed,
            state.stack.size());
        ++state.determinismEventCount;
    }
    if (state.profile) {
        ++state.profileEvents;
        auto& profile = state.profileFunctions[state.currentFunctionId];
        if (profile.function.empty() && !state.stack.empty()) {
            profile.function = *state.stack.back();
        }
        profile.maximumCallDepth = std::max(
            profile.maximumCallDepth, state.stack.size());
        switch (kind) {
        case TraceEventKind::FunctionEnter: ++profile.calls; break;
        case TraceEventKind::FunctionExit: ++profile.returns; break;
        case TraceEventKind::Instruction: ++profile.instructions; break;
        case TraceEventKind::Branch: ++profile.branches; break;
        case TraceEventKind::ExternalCall: ++profile.externalCalls; break;
        case TraceEventKind::GcStep: ++profile.gcSteps; break;
        case TraceEventKind::RuntimeError: ++profile.runtimeErrors; break;
        }
    }
    if (!state.diagnosticEventsEnabled) return;
    TraceEvent event;
    event.kind = kind;
    event.function = state.stack.empty()
        ? std::string{}
        : *state.stack.back();
    event.operation.assign(operation.data(), operation.size());
    event.instructionIndex = state.executed;
    event.callDepth = state.stack.size();
    if (state.trace && *state.trace) (*state.trace)(event);
}

void mergeProfile(State& state) {
    if (!state.profile) return;
    ExecutionProfile profile;
    profile.totalEvents = state.profileEvents;
    profile.functions.reserve(state.profileFunctions.size());
    for (auto& [symbolId, function] : state.profileFunctions) {
        (void)symbolId;
        profile.functions.push_back(std::move(function));
    }
    state.profile->merge(profile);
}

bool notifyDebuggerException(
    State& state,
    ErrorCode code,
    std::string exceptionId,
    std::string description,
    bool isUncaught,
    bool isRuntimeError) {
    if (!state.debugger || state.exceptionNotifiedDebugger ||
        code == ErrorCode::ExecutionTerminated || state.debugFrames.empty()) {
        return true;
    }
    auto& topFrame = state.debugFrames.back();
    if (!topFrame.point && topFrame.function &&
        !topFrame.function->debugInfo.sequencePoints.empty()) {
        topFrame.point = &topFrame.function->debugInfo.sequencePoints.front();
    }
    if (isUncaught || isRuntimeError) {
        state.exceptionNotifiedDebugger = true;
    }
    return state.debugger->onException(
        state.debugFrames,
        state.heap,
        code,
        std::move(exceptionId),
        std::move(description),
        isUncaught,
        isRuntimeError);
}

bool fail(State& state, ErrorCode code, std::string message) {
    state.error.code = code;
    state.error.message = std::move(message);
    state.error.stackTrace = materializeStack(state.stack);
    std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
    populateInterpreterSourceLocations(state);
    emitTrace(state, TraceEventKind::RuntimeError, 0, state.error.message);
    notifyDebuggerException(
        state,
        code,
        std::string(errorCodeName(code)),
        state.error.message,
        true,
        true);
    return false;
}

bool consume(
    State& state,
    DeterminismOperationId operationId,
    std::string_view operation) {
    if (state.executed >= state.limits.instructionBudget) {
        return fail(state, ErrorCode::InstructionBudgetExceeded,
            "instruction budget exceeded");
    }
    ++state.executed;
    if (state.fastAccounting) {
        stepGcIfNeeded(state, 1);
        return true;
    }
    state.statistics.instructionsExecuted = state.executed;
    if (state.traceEventsEnabled && !operation.empty()) {
        if (state.determinismOnly) {
            state.determinismEventDigest = accumulateDeterminismEvent(
                state.determinismEventDigest,
                TraceEventKind::Instruction,
                state.currentFunctionId,
                operationId,
                state.executed,
                state.stack.size());
            ++state.determinismEventCount;
        } else {
            emitTrace(state, TraceEventKind::Instruction,
                operationId, operation);
        }
    }
    stepGcIfNeeded(state, 1);
    return true;
}

bool checkCrossHeapRefAccess(
    State& state,
    const ObjectRef& reference,
    bool isWrite,
    std::string_view context) {
    if (reference.isStackSlotRef() || !state.heap || !reference.valid()) {
        return true;
    }
    if (reference.heapId != 0 &&
        reference.heapId != static_cast<std::uint32_t>(state.heap->heapId()) &&
        (isWrite || state.crossHeapPolicy != CrossHeapAccessPolicy::AllowReadWrite)) {
        return fail(
            state,
            ErrorCode::CrossHeapAccessViolation,
            std::string(context) + (isWrite
                ? " rejected cross-heap mutation by active CrossHeapAccessPolicy"
                : " rejected cross-heap access by active CrossHeapAccessPolicy"));
    }
    return true;
}

bool expectType(
    State& state,
    const Value& value,
    semantic::PrimitiveType type,
    std::string_view context) {
    if (const auto* reference = std::get_if<ObjectRef>(&value)) {
        if (reference->isStackSlotRef()) {
            if (reference->generation != state.flatBankId ||
                reference->slot >= state.values.size()) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    std::string(context) + " contains an expired stack slot reference");
            }
        } else {
            if (!checkCrossHeapRefAccess(state, *reference, false, context)) {
                return false;
            }
            if (!state.heap || !state.heap->isAlive(*reference)) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    std::string(context) + " contains an invalid managed object reference");
            }
        }
    }
    if (valueType(value) != type) {
        return fail(state, ErrorCode::TypeMismatch,
            std::string(context) + " expected '" + semantic::primitiveTypeName(type) +
            "', got '" + semantic::primitiveTypeName(valueType(value)) + "'");
    }
    return true;
}

const semantic::TypeSymbol* typeDescriptorById(
    const bytecode::Module& module,
    semantic::SymbolId id) {
    for (const auto& type : module.types) {
        if (type.id == id) return &type;
    }
    return nullptr;
}

semantic::SymbolId exactTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) {
    return semantic::isExactType(type) && !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

bool runtimeAssignable(
    const State& state,
    semantic::SymbolId actual,
    semantic::SymbolId expected) noexcept {
    if (actual == expected) return true;
    if (actual == 0) return false;
    if (state.mirror) {
        const auto it = state.mirror->typeDispatch.find(actual);
        if (it != state.mirror->typeDispatch.end()) {
            if (expected == 0) return true;
            for (const auto cand : it->second.assignableTargetIds) {
                if (cand == expected) return true;
            }
            return false;
        }
    }
    if (!state.program) return false;
    if (expected == 0) return state.program->resolveType(actual) != nullptr;
    auto current = actual;
    for (std::size_t depth = 0; current != 0 && depth < 256; ++depth) {
        const auto* found = state.program->resolveType(current);
        if (!found) return false;
        for (const auto& implementation :
             found->interfaceDispatchMaps) {
            if (implementation.interfaceTypeId == expected) return true;
        }
        if (found->baseTypeId == expected) return true;
        current = found->baseTypeId;
    }
    return false;
}

const detail::PrelinkedDispatchTarget* resolvePrelinkedPolymorphicTarget(
    const detail::ExecutionMirror& mirror,
    semantic::SymbolId actualTypeId,
    const detail::PolymorphicCallSiteDesc& site) noexcept {
    if (!site.valid || actualTypeId == 0) return nullptr;
    const auto tIt = mirror.typeDispatch.find(actualTypeId);
    if (tIt == mirror.typeDispatch.end()) return nullptr;
    const auto& meta = tIt->second;
    if (!site.isInterface) {
        if (site.slot >= meta.virtualTargets.size()) return nullptr;
        const auto& target = meta.virtualTargets[site.slot];
        return (target.target && target.execution) ? &target : nullptr;
    }
    const auto& imaps = meta.interfaceMaps;
    if (imaps.size() <= 4) {
        for (const auto& m : imaps) {
            if (m.interfaceTypeId == site.interfaceTypeId) {
                if (site.slot >= m.slots.size()) return nullptr;
                const auto& target = m.slots[site.slot];
                return (target.target && target.execution) ? &target : nullptr;
            }
        }
        return nullptr;
    }
    std::size_t lo = 0;
    std::size_t hi = imaps.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1u);
        if (imaps[mid].interfaceTypeId < site.interfaceTypeId) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < imaps.size() && imaps[lo].interfaceTypeId == site.interfaceTypeId) {
        if (site.slot >= imaps[lo].slots.size()) return nullptr;
        const auto& target = imaps[lo].slots[site.slot];
        return (target.target && target.execution) ? &target : nullptr;
    }
    return nullptr;
}

const semantic::InterfaceDispatchMap* runtimeInterfaceMap(
    const State& state,
    semantic::SymbolId actual,
    semantic::SymbolId interfaceTypeId) noexcept {
    if (!state.program) return nullptr;
    auto current = actual;
    for (std::size_t depth = 0; current != 0 && depth < 256; ++depth) {
        const auto* found = state.program->resolveType(current);
        if (!found) return nullptr;
        for (const auto& implementation :
             found->interfaceDispatchMaps) {
            if (implementation.interfaceTypeId == interfaceTypeId) {
                return &implementation;
            }
        }
        current = found->baseTypeId;
    }
    return nullptr;
}

bool expectObject(
    State& state,
    const Value& value,
    const semantic::TypeSymbol& type,
    bool allowNull,
    const std::string& context) {
    if (std::holds_alternative<NullObject>(value)) {
        if (allowNull) return true;
        return fail(state, ErrorCode::NullReference,
            context + " attempted to dereference null");
    }
    const auto* reference = std::get_if<ObjectRef>(&value);
    if (!reference || reference->kind != ObjectKind::Record) {
        return fail(state, ErrorCode::TypeMismatch,
            context + " expected object '" + semantic::canonicalTypeName(type) + "'");
    }
    if (reference->isStackSlotRef()) {
        if (reference->generation == state.flatBankId &&
            reference->slot < state.values.size() &&
            isSynthesizedRefWrapperTypeName(type.name)) {
            return true;
        }
        return fail(state, ErrorCode::InvalidObjectReference,
            context + " contains an invalid stack slot reference");
    }
    if (!checkCrossHeapRefAccess(state, *reference, false, context)) {
        return false;
    }
    if (!state.heap || !state.heap->isAlive(*reference)) {
        return fail(state, ErrorCode::InvalidObjectReference,
            context + " contains an invalid managed object reference");
    }
    const auto actual = state.heap->objectTypeId(*reference);
    if (!actual || !runtimeAssignable(state, *actual, type.id)) {
        return fail(state, ErrorCode::TypeMismatch,
            context + " expected object '" + semantic::canonicalTypeName(type) + "'");
    }
    return true;
}

bool expectSignatureType(
    State& state,
    const Value& value,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::string_view context) {
    if (!expectType(state, value, type, context)) return false;
    if (type == semantic::PrimitiveType::Object) {
        if (std::holds_alternative<NullObject>(value) || typeId == 0) return true;
        const auto* reference = std::get_if<ObjectRef>(&value);
        if (reference && reference->isStackSlotRef()) {
            if (reference->generation == state.flatBankId &&
                reference->slot < state.values.size() &&
                state.program) {
                const auto& mirror = detail::executionMirror(*state.program);
                if (mirror.stackRefTemplateByTypeId.find(typeId) !=
                    mirror.stackRefTemplateByTypeId.end()) {
                    return true;
                }
            }
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has an incompatible stack reference type");
        }
        const auto actual = reference && state.heap
            ? state.heap->objectTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || !runtimeAssignable(state, *actual, typeId)) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong runtime object type");
        }
    } else if (type == semantic::PrimitiveType::Array) {
        if (std::holds_alternative<NullArray>(value) || typeId == 0) return true;
        const auto* reference = std::get_if<ObjectRef>(&value);
        const auto actual = reference && state.heap
            ? state.heap->arrayTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || *actual != typeId) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong runtime array type");
        }
    } else if (type == semantic::PrimitiveType::Handle && typeId != 0) {
        NativeHandle handle{};
        if (!value.tryGetNativeHandle(handle) ||
            (handle.valid() && handle.typeId != typeId) ||
            (!handle.valid() && handle.typeId != 0 && handle.typeId != typeId)) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong native handle type");
        }
    } else if (type == semantic::PrimitiveType::Struct) {
        const auto actualTypeId = structValueTypeId(value);
        const auto* structure = std::get_if<StructValue>(&value);
        if (!actualTypeId || typeId == 0 || *actualTypeId != typeId ||
            (structure && !structure->valid())) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong struct type");
        }
    } else if (type == semantic::PrimitiveType::Enum) {
        EnumValue enumeration{};
        if (!value.tryGetEnum(enumeration) || typeId == 0 || enumeration.typeId != typeId) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong enum type");
        }
    }
    return true;
}

Value defaultValue(
    const bytecode::Module& module,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::size_t depth) {
    switch (type) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return ByteValue{};
    case semantic::PrimitiveType::SByte: return SByteValue{};
    case semantic::PrimitiveType::Short: return ShortValue{};
    case semantic::PrimitiveType::UShort: return UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return UIntValue{};
    case semantic::PrimitiveType::Long: return LongValue{};
    case semantic::PrimitiveType::ULong: return ULongValue{};
    case semantic::PrimitiveType::Float: return FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return CharValue{};
    case semantic::PrimitiveType::String: return NullString{};
    case semantic::PrimitiveType::Object: return NullObject{};
    case semantic::PrimitiveType::Array: return NullArray{};
    case semantic::PrimitiveType::Handle: {
        NativeHandle handle;
        handle.typeId = typeId;
        return handle;
    }
    case semantic::PrimitiveType::Enum: return EnumValue{typeId, 0};
    case semantic::PrimitiveType::Struct: {
        if (const auto inlined = makeDefaultInlineStructValue(typeId)) {
            return *inlined;
        }
        if (depth > 64) return StructValue{};
        const auto* descriptor = typeDescriptorById(module, typeId);
        if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
            return StructValue{};
        }
        if (descriptor->fields.size() <= StructValue::MaxInlineFields) {
            std::array<Value, StructValue::MaxInlineFields> stackFields{};
            for (std::size_t i = 0; i < descriptor->fields.size(); ++i) {
                const auto& field = descriptor->fields[i];
                stackFields[i] = defaultValue(
                    module, field.type, exactTypeId(field.type, field.typeName), depth + 1);
            }
            return makeStructValueFromFields(
                typeId, ValueSpan(stackFields.data(), descriptor->fields.size()));
        }
        std::vector<Value> fields;
        fields.reserve(descriptor->fields.size());
        for (const auto& field : descriptor->fields) {
            fields.push_back(defaultValue(
                module, field.type, exactTypeId(field.type, field.typeName), depth + 1));
        }
        return makeStructValueFromFields(typeId, std::move(fields));
    }
    case semantic::PrimitiveType::Null: return std::monostate{};
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return std::monostate{};
}


bool valuesEqual(State& state, const Value& left, const Value& right, bool& equal) {
    if (valueType(left) != valueType(right)) {
        equal = false;
        return true;
    }
    if (valueType(left) == semantic::PrimitiveType::Object ||
        valueType(left) == semantic::PrimitiveType::Array) {
        const auto checkAccess = [&](const Value& value) {
            const auto* reference = std::get_if<ObjectRef>(&value);
            return !reference || checkCrossHeapRefAccess(state, *reference, false, "reference equality");
        };
        if (!checkAccess(left) || !checkAccess(right)) {
            return false;
        }
        const auto validate = [&](const Value& value) {
            const auto* reference = std::get_if<ObjectRef>(&value);
            return !reference || (state.heap && state.heap->isAlive(*reference));
        };
        if (!validate(left) || !validate(right)) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "reference equality encountered an invalid managed reference");
        }
        equal = left == right;
        return true;
    }
    if (valueType(left) == semantic::PrimitiveType::Struct) {
        if (left == right) {
            equal = true;
            return true;
        }
        if (left.rawTag() == ValueTag::Struct &&
            right.rawTag() == ValueTag::Struct) {
            const auto& ls = left.asStructUnchecked();
            const auto& rs = right.asStructUnchecked();
            if (ls.typeId == rs.typeId && ls.fieldCount() == rs.fieldCount()) {
                const auto count = ls.fieldCount();
                for (std::size_t i = 0; i < count; ++i) {
                    const auto lf = structValueFieldAt(ls, i);
                    const auto rf = structValueFieldAt(rs, i);
                    if (!lf || !rf) {
                        equal = false;
                        return true;
                    }
                    bool fieldEq = false;
                    if (!valuesEqual(state, *lf, *rf, fieldEq)) return false;
                    if (!fieldEq) {
                        equal = false;
                        return true;
                    }
                }
                equal = true;
                return true;
            }
        }
        equal = false;
        return true;
    }
    if (valueType(left) != semantic::PrimitiveType::String) {
        equal = left == right;
        return true;
    }
    if (std::holds_alternative<NullString>(left) ||
        std::holds_alternative<NullString>(right)) {
        equal = std::holds_alternative<NullString>(left) &&
            std::holds_alternative<NullString>(right);
        return true;
    }
    const auto getText = [&](const Value& value) -> std::optional<std::string_view> {
        if (const auto* text = std::get_if<std::string>(&value)) return *text;
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            if (!state.heap) return std::nullopt;
            return state.heap->stringView(*reference);
        }
        return std::nullopt;
    };
    const auto leftText = getText(left);
    const auto rightText = getText(right);
    if (!leftText || !rightText) {
        return fail(state, ErrorCode::InvalidObjectReference,
            "string equality encountered an invalid managed reference");
    }
    equal = *leftText == *rightText;
    return true;
}

bool checkedIntResult(State& state, std::int64_t value, Value& output) {
    constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    if (value < minimum || value > maximum) {
        return fail(state, ErrorCode::IntegerOverflow, "integer arithmetic overflow");
    }
    output = value;
    return true;
}

bool checkedLongAdd(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left + right};
    return true;
}

bool checkedLongSubtract(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left - right};
    return true;
}

bool checkedLongMultiply(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if (left == 0 || right == 0) {
        output = LongValue{};
        return true;
    }
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    bool overflow = false;
    if (left > 0) {
        overflow = right > 0 ? left > maximum / right : right < minimum / left;
    } else {
        overflow = right > 0 ? left < minimum / right :
            (left != 0 && right < maximum / left);
    }
    if (overflow) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left * right};
    return true;
}

const debug::SequencePoint* findSequencePoint(
    const bytecode::Function& function,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator) {
    for (const auto& point : function.debugInfo.sequencePoints) {
        if (point.blockId == blockId &&
            point.instructionIndex == instructionIndex &&
            point.terminator == terminator) {
            return &point;
        }
    }
    return nullptr;
}

bool debugSequencePoint(
    State& state,
    const bytecode::Module& /*module*/,
    const bytecode::Function& function,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator) {
    if (!state.debugger) return true;
    const auto* point = findSequencePoint(
        function, blockId, instructionIndex, terminator);
    if (!point && !state.debugger->hasPendingInterruption()) return true;
    if (point) {
        state.debugFrames.back().point = point;
    }
    if (!state.debugger->onSequencePoint(state.debugFrames, state.heap)) {
        return fail(state, ErrorCode::ExecutionTerminated,
            "execution terminated by debugger");
    }
    return true;
}

bool executeCall(
    State& state,
    const bytecode::Module& module,
    const bytecode::Instruction& instruction,
    const ValueWindow& registers,
    Value& result) {
    if (instruction.index >= module.functionReferences.size()) {
        return fail(state, ErrorCode::InvalidProgram, "call reference index is invalid");
    }
    const auto& reference = module.functionReferences[instruction.index];
    const ArgumentSource arguments(registers, instruction.operands);
    for (const auto operand : instruction.operands) {
        if (operand >= registers.size()) {
            return fail(state, ErrorCode::InvalidProgram, "call operand register is invalid");
        }
    }
    const auto cacheIndex = [&] {
        const auto callSite = reinterpret_cast<std::uintptr_t>(&instruction);
        return static_cast<std::size_t>((callSite >> 4U) & 63U);
    };
    const auto invokeCached = [&](const InlineCallCacheEntry& cached)
            -> std::optional<bool> {
        if (cached.callSite != &instruction ||
            !cached.target || !cached.execution) {
            return std::nullopt;
        }
        return executeFunction(
            state, *cached.target, arguments, result, cached.execution, true);
    };
    const auto tryCachedReceiver = [&](ObjectRef receiver)
            -> std::optional<bool> {
        if (!state.heap || !state.heap->isAlive(receiver)) return std::nullopt;
        const auto& cached = (*state.inlineCallCache)[cacheIndex()];
        return cached.receiver == receiver
            ? invokeCached(cached)
            : std::nullopt;
    };
    const auto tryCachedType = [&](semantic::SymbolId receiverTypeId)
            -> std::optional<bool> {
        const auto& cached = (*state.inlineCallCache)[cacheIndex()];
        return cached.receiverTypeId == receiverTypeId
            ? invokeCached(cached)
            : std::nullopt;
    };

    if (arguments.size() != reference.parameterTypes.size()) {
        return fail(state, ErrorCode::InvalidProgram,
            "call argument count does not match its reference");
    }
    auto targetSymbolId = reference.symbolId;
    if (reference.virtualDispatch && reference.interfaceDispatch) {
        return fail(state, ErrorCode::InvalidProgram,
            "call cannot use virtual and interface dispatch together");
    }
    if (reference.virtualDispatch) {
        if (reference.virtualSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            arguments.empty() ||
            reference.parameterTypes.empty() ||
            reference.parameterTypes.front() !=
                semantic::PrimitiveType::Object) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call has an invalid receiver contract");
        }
        if (std::holds_alternative<NullObject>(arguments.front())) {
            return fail(state, ErrorCode::NullReference,
                "virtual call attempted to dereference null");
        }
        const auto* receiver = std::get_if<ObjectRef>(&arguments.front());
        if (receiver) {
            if (!checkCrossHeapRefAccess(state, *receiver, false, "virtual call")) {
                return false;
            }
            if (const auto cached = tryCachedReceiver(*receiver)) {
                return *cached;
            }
        }
        const auto actualTypeId = receiver && state.heap
            ? state.heap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        if (!actualTypeId || !state.program) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "virtual call receiver has no runtime type descriptor");
        }
        if (const auto cached = tryCachedType(*actualTypeId)) {
            return *cached;
        }
        if (state.mirror) {
            detail::PolymorphicCallSiteDesc desc{};
            desc.valid = true;
            desc.isInterface = false;
            desc.slot = reference.virtualSlot;
            if (const auto* pre = resolvePrelinkedPolymorphicTarget(*state.mirror, *actualTypeId, desc)) {
                (*state.inlineCallCache)[cacheIndex()] = {
                    &instruction, *receiver, *actualTypeId, pre->target, pre->execution};
                return executeFunction(state, *pre->target, arguments, result, pre->execution, true);
            }
        }
        const auto* actualType = state.program->resolveType(*actualTypeId);
        if (!actualType ||
            reference.virtualSlot >=
                actualType->virtualDispatchTable.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call slot is outside the runtime dispatch table");
        }
        targetSymbolId =
            actualType->virtualDispatchTable[reference.virtualSlot];
        if (targetSymbolId == 0) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call reached an abstract runtime slot");
        }
    } else if (reference.interfaceDispatch) {
        if (reference.interfaceTypeId == 0 ||
            reference.interfaceSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            arguments.empty() ||
            reference.parameterTypes.empty() ||
            reference.parameterTypes.front() !=
                semantic::PrimitiveType::Object ||
            typeIdAt(reference.parameterTypeIds, 0) !=
                reference.interfaceTypeId) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call has an invalid receiver contract");
        }
        if (std::holds_alternative<NullObject>(arguments.front())) {
            return fail(state, ErrorCode::NullReference,
                "interface call attempted to dereference null");
        }
        const auto* receiver = std::get_if<ObjectRef>(&arguments.front());
        if (receiver) {
            if (!checkCrossHeapRefAccess(state, *receiver, false, "virtual call")) {
                return false;
            }
            if (const auto cached = tryCachedReceiver(*receiver)) {
                return *cached;
            }
        }
        const auto actualTypeId = receiver && state.heap
            ? state.heap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        if (!actualTypeId) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "interface call receiver has no runtime type descriptor");
        }
        if (const auto cached = tryCachedType(*actualTypeId)) {
            return *cached;
        }
        if (state.mirror) {
            detail::PolymorphicCallSiteDesc desc{};
            desc.valid = true;
            desc.isInterface = true;
            desc.interfaceTypeId = reference.interfaceTypeId;
            desc.slot = reference.interfaceSlot;
            if (const auto* pre = resolvePrelinkedPolymorphicTarget(*state.mirror, *actualTypeId, desc)) {
                (*state.inlineCallCache)[cacheIndex()] = {
                    &instruction, *receiver, *actualTypeId, pre->target, pre->execution};
                return executeFunction(state, *pre->target, arguments, result, pre->execution, true);
            }
        }
        const auto* implementation = runtimeInterfaceMap(
            state, *actualTypeId, reference.interfaceTypeId);
        if (!implementation ||
            reference.interfaceSlot >= implementation->slots.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call slot is outside the runtime dispatch map");
        }
        targetSymbolId = implementation->slots[reference.interfaceSlot];
        if (targetSymbolId == 0) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call reached an unimplemented runtime slot");
        }
    }

    const auto* found = state.program
        ? state.program->resolveFunction(targetSymbolId)
        : nullptr;
    if (found) {
        const auto* execution = detail::findExecutionFunction(
            detail::executionMirror(*state.program), *found->function);
        if ((reference.virtualDispatch || reference.interfaceDispatch) &&
            execution) {
            const auto* receiver = std::get_if<ObjectRef>(&arguments.front());
            const auto receiverTypeId = receiver && state.heap
                ? state.heap->objectTypeId(*receiver)
                : std::optional<semantic::SymbolId>{};
            if (receiverTypeId) {
                (*state.inlineCallCache)[cacheIndex()] = {
                    &instruction, *receiver, *receiverTypeId, found, execution};
            }
        }
        return executeFunction(
            state, *found, arguments, result, execution);
    }
    if (reference.virtualDispatch || reference.interfaceDispatch) {
        return fail(state, ErrorCode::InvalidProgram,
            "runtime dispatch target function is unavailable");
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!expectSignatureType(
                state,
                arguments[index],
                reference.parameterTypes[index],
                typeIdAt(reference.parameterTypeIds, index),
                "call argument")) {
            return false;
        }
    }
    std::array<Value, 8> stackArguments{};
    std::vector<Value> heapArguments;
    ValueSpan argSpan;
    if (arguments.empty()) {
        argSpan = ValueSpan(nullptr, 0);
    } else if (!arguments.indices) {
        argSpan = ValueSpan(
            arguments.values->data() + arguments.offset,
            arguments.count);
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
    if (reference.name.rfind("$intrinsic::Async.__", 0) == 0) {
        std::string intrinsicError;
        if (!tryExecuteAsyncIntrinsic(
                reference.name.substr(12u),
                argSpan,
                state.asyncRuntime,
                state.heap,
                result,
                intrinsicError) ||
            !intrinsicError.empty()) {
            const auto code = intrinsicError.find("null") != std::string::npos
                ? ErrorCode::NullReference
                : ErrorCode::InvalidProgram;
            return fail(state, code, intrinsicError);
        }
        return true;
    }
    if (reference.name.rfind("$intrinsic::", 0) == 0) {
        std::string intrinsicError;
        if (!tryExecuteRuntimeIntrinsic(
                reference.name, argSpan, state.heap, result, intrinsicError)) {
            const auto code = intrinsicError.find("null") != std::string::npos
                ? ErrorCode::NullReference
                : ErrorCode::InvalidProgram;
            return fail(state, code, intrinsicError);
        }
        return true;
    }
    RuntimeError externalError;
    ++state.statistics.externalCalls;
    emitTrace(state, TraceEventKind::ExternalCall,
        reference.symbolId, reference.name);
    std::optional<Value> externalResult;
    const ExternalFunction* fallback =
        state.externalResolver && *state.externalResolver
        ? state.externalResolver
        : nullptr;
    struct ExternalStackRefBridge {
        std::size_t argIdx = 0;
        ObjectRef stackRef{};
        ObjectRef tempHeapRef{};
    };
    std::vector<ExternalStackRefBridge> extRefBridges;
    std::vector<Value> detachedExtArgs;
    bool needsExtArgRewrite = false;
    for (std::size_t idx = 0; idx < argSpan.size(); ++idx) {
        if ((argSpan[idx].rawTag() == ValueTag::Struct &&
             argSpan[idx].asStructUnchecked().isFrameSlice()) ||
            (argSpan[idx].rawTag() == ValueTag::ObjectRef &&
             argSpan[idx].u_.objectRef.isStackSlotRef())) {
            needsExtArgRewrite = true;
            break;
        }
    }
    if (needsExtArgRewrite) {
        detachedExtArgs.resize(argSpan.size());
        for (std::size_t k = 0; k < argSpan.size(); ++k) {
            if (argSpan[k].rawTag() == ValueTag::ObjectRef &&
                argSpan[k].u_.objectRef.isStackSlotRef() &&
                state.heap &&
                k < reference.parameterTypeIds.size()) {
                const ObjectRef sRef = argSpan[k].u_.objectRef;
                const semantic::SymbolId wTid = reference.parameterTypeIds[k];
                Value boxedField{};
                if (sRef.stackRefSchemaId() == 0u) {
                    boxedField = detachFrameSliceValue(state.values[sRef.slot]);
                } else if (const auto* sc = getFlatStructSchema(sRef.stackRefSchemaId())) {
                    boxedField = reconstructDetachedStructFromLeaves(*sc, state.values.data() + sRef.slot);
                }
                std::vector<std::size_t> refFlds;
                if (boxedField.isTraceable()) refFlds.push_back(0);
                RuntimeError allocErr;
                const auto tempObj = state.heap->allocateObject(
                    wTid, std::vector<Value>{boxedField}, std::move(refFlds), &allocErr);
                if (tempObj) {
                    detachedExtArgs[k] = *tempObj;
                    extRefBridges.push_back(ExternalStackRefBridge{k, sRef, *tempObj});
                    continue;
                }
            }
            detachedExtArgs[k] = detachFrameSliceValue(argSpan[k]);
        }
        argSpan = ValueSpan(detachedExtArgs);
    }
    const bool extOk = state.determinism.invokeExternal(
        state.bindings,
        fallback,
        reference,
        argSpan,
        externalResult,
        externalError);
    if (extOk && !extRefBridges.empty() && state.heap) {
        for (const auto& br : extRefBridges) {
            if (const auto updated = state.heap->fieldGet(br.tempHeapRef, 0)) {
                if (br.stackRef.stackRefSchemaId() == 0u) {
                    state.values[br.stackRef.slot] = *updated;
                } else if (const auto* sc = getFlatStructSchema(br.stackRef.stackRefSchemaId())) {
                    (void)extractStructLeavesToBuffer(*updated, *sc, state.values.data() + br.stackRef.slot);
                }
            }
        }
    }
    if (!extOk) {
        state.error = std::move(externalError);
        if (state.error.code == ErrorCode::None) {
            state.error.code = ErrorCode::ExternalFunctionUnresolved;
            state.error.message = "external function '" + reference.name + "' failed";
        }
        state.error.stackTrace = materializeStack(state.stack);
        std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
        populateInterpreterSourceLocations(state);
        notifyDebuggerException(
            state,
            state.error.code,
            std::string(errorCodeName(state.error.code)),
            state.error.message,
            true,
            true);
        return false;
    }
    if (!expectSignatureType(
            state,
            *externalResult,
            reference.returnType,
            reference.returnTypeId,
            "external return value")) {
        return false;
    }
    result = std::move(*externalResult);
    return true;
}

const std::shared_ptr<const std::vector<std::size_t>>& delegateReferenceFields() {
    static const auto refs = std::make_shared<const std::vector<std::size_t>>(
        std::initializer_list<std::size_t>{0u, 7u});
    return refs;
}

std::int64_t delegateWord(std::uint64_t value, bool high) noexcept {
    return static_cast<std::int64_t>(
        static_cast<std::uint32_t>(high ? value >> 32u : value));
}

bool readDelegateIdFromFields(
    const Value* fields,
    std::size_t lowIndex,
    semantic::SymbolId& outId) noexcept {
    std::int64_t lo = 0, hi = 0;
    if (!fields[lowIndex].tryGetInt64(lo) ||
        !fields[lowIndex + 1u].tryGetInt64(hi) ||
        lo < 0 || hi < 0 ||
        lo > std::numeric_limits<std::uint32_t>::max() ||
        hi > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    outId = static_cast<std::uint64_t>(static_cast<std::uint32_t>(lo)) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32u);
    return true;
}

bool readDelegateId(
    State& state,
    ObjectRef reference,
    std::size_t lowIndex,
    semantic::SymbolId& value,
    const char* context) {
    ObjectFastView view{};
    if (state.heap && state.heap->tryBindObjectFastView(reference, view) &&
        lowIndex + 1u < view.fieldCount) {
        if (!readDelegateIdFromFields(view.fields, lowIndex, value)) {
            return fail(state, ErrorCode::InvalidProgram, context);
        }
        return true;
    }
    const auto low = state.heap->fieldGet(reference, lowIndex);
    const auto high = state.heap->fieldGet(reference, lowIndex + 1u);
    const auto* lowValue = low ? std::get_if<std::int64_t>(&*low) : nullptr;
    const auto* highValue = high ? std::get_if<std::int64_t>(&*high) : nullptr;
    if (!lowValue || !highValue || *lowValue < 0 || *highValue < 0 ||
        *lowValue > std::numeric_limits<std::uint32_t>::max() ||
        *highValue > std::numeric_limits<std::uint32_t>::max()) {
        return fail(state, ErrorCode::InvalidProgram, context);
    }
    value = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(*lowValue)) |
        (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(*highValue)) << 32u);
    return true;
}

bool flattenDelegate(
    State& state,
    semantic::SymbolId delegateTypeId,
    const Value& value,
    std::vector<Value>& leaves) {
    if (std::holds_alternative<NullObject>(value)) return true;
    if (!state.heap) {
        return fail(state, ErrorCode::InvalidProgram,
            "delegate invocation has no heap");
    }
    std::array<Value, 64> inlineStack{};
    std::size_t inlineTop = 0;
    std::vector<Value> heapStack;
    inlineStack[inlineTop++] = value;
    std::size_t visitedNodes = 0;
    while (inlineTop > 0 || !heapStack.empty()) {
        Value cur;
        if (!heapStack.empty()) {
            cur = heapStack.back();
            heapStack.pop_back();
        } else {
            cur = inlineStack[--inlineTop];
        }
        if (std::holds_alternative<NullObject>(cur)) continue;
        if (++visitedNodes > 65536u) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation list is too deep");
        }
        const auto* reference = std::get_if<ObjectRef>(&cur);
        ObjectFastView view{};
        if (!reference ||
            !state.heap->tryBindObjectFastView(*reference, view) ||
            view.typeId != delegateTypeId ||
            view.fieldCount != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate invocation list contains an incompatible value");
        }
        std::int64_t flags = -1;
        if (!view.fields[3].tryGetInt64(flags) || flags < 0 || flags > 4) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation-list metadata is corrupt");
        }
        if (flags != 4) {
            leaves.push_back(cur);
            continue;
        }
        const Value left = view.fields[0];
        const Value right = view.fields[7];
        // Push right first so left is popped first (left-to-right order)
        if (!heapStack.empty() || inlineTop + 2u > inlineStack.size()) {
            heapStack.push_back(right);
            heapStack.push_back(left);
        } else {
            inlineStack[inlineTop++] = right;
            inlineStack[inlineTop++] = left;
        }
    }
    return true;
}

bool sameDelegateLeaf(
    State& state,
    const Value& left,
    const Value& right) {
    const auto* leftRef = std::get_if<ObjectRef>(&left);
    const auto* rightRef = std::get_if<ObjectRef>(&right);
    if (!leftRef || !rightRef || !state.heap) return false;
    if (*leftRef == *rightRef && state.heap->isAlive(*leftRef)) return true;
    ObjectFastView lv{}, rv{};
    if (state.heap->tryBindObjectFastView(*leftRef, lv) &&
        state.heap->tryBindObjectFastView(*rightRef, rv) &&
        lv.fieldCount >= 7u && rv.fieldCount >= 7u) {
        for (std::size_t index = 0; index < 7u; ++index) {
            if (!(lv.fields[index] == rv.fields[index])) return false;
        }
        return true;
    }
    for (std::size_t index = 0; index < 7u; ++index) {
        const auto a = state.heap->fieldGet(*leftRef, index);
        const auto b = state.heap->fieldGet(*rightRef, index);
        if (!a || !b || !(*a == *b)) return false;
    }
    return true;
}

bool allocateCombineDelegateNode(
    State& state,
    semantic::SymbolId delegateTypeId,
    const Value& left,
    const Value& right,
    Value& outNode,
    bool* outSlotsMoved = nullptr) {
    const std::array<Value, 8> fields{{
        left,
        std::int64_t{0},
        std::int64_t{0},
        std::int64_t{4},
        std::int64_t{0},
        std::int64_t{0},
        std::int64_t{0},
        right,
    }};
    RuntimeError error;
    bool localMoved = false;
    const auto allocated = state.heap->allocateObjectFromTemplate(
        delegateTypeId,
        fields.data(),
        fields.size(),
        delegateReferenceFields(),
        &error,
        &localMoved);
    if (localMoved && outSlotsMoved) *outSlotsMoved = true;
    if (!allocated) {
        return fail(state,
            error.code == ErrorCode::None
                ? ErrorCode::OutOfMemory
                : error.code,
            error.message.empty()
                ? "delegate-list allocation failed"
                : error.message);
    }
    outNode = *allocated;
    return true;
}

bool buildDelegateList(
    State& state,
    semantic::SymbolId delegateTypeId,
    const std::vector<Value>& leaves,
    Value& result,
    bool* outSlotsMoved = nullptr) {
    if (leaves.empty()) {
        result = NullObject{};
        return true;
    }
    result = leaves.front();
    for (std::size_t index = 1; index < leaves.size(); ++index) {
        Value nextNode;
        if (!allocateCombineDelegateNode(
                state, delegateTypeId, result, leaves[index], nextNode, outSlotsMoved)) {
            return false;
        }
        result = nextNode;
    }
    return true;
}

bool combineOrRemoveDelegateFastImpl(
    State& state,
    semantic::SymbolId delegateTypeId,
    const Value& left,
    const Value& right,
    bool isRemove,
    Value& result,
    bool* outSlotsMoved) {
    if (!state.heap) {
        return fail(state, ErrorCode::InvalidProgram, "delegate combination has no heap");
    }
    const bool leftNull = (left.rawTag() == ValueTag::NullObject);
    const bool rightNull = (right.rawTag() == ValueTag::NullObject);
    ObjectFastView leftView{}, rightView{};
    std::int64_t leftFlags = -1, rightFlags = -1;

    if (!leftNull) {
        if (left.rawTag() != ValueTag::ObjectRef ||
            !state.heap->tryBindObjectFastView(left.u_.objectRef, leftView) ||
            leftView.typeId != delegateTypeId ||
            leftView.fieldCount != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate combination left operand has incompatible type");
        }
        if (!leftView.fields[3].tryGetInt64(leftFlags) || leftFlags < 0 || leftFlags > 4) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation-list metadata is corrupt");
        }
    }
    if (!rightNull) {
        if (right.rawTag() != ValueTag::ObjectRef ||
            !state.heap->tryBindObjectFastView(right.u_.objectRef, rightView) ||
            rightView.typeId != delegateTypeId ||
            rightView.fieldCount != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate combination right operand has incompatible type");
        }
        if (!rightView.fields[3].tryGetInt64(rightFlags) || rightFlags < 0 || rightFlags > 4) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation-list metadata is corrupt");
        }
    }

    if (!isRemove) {
        if (leftNull && rightNull) {
            result = NullObject{};
            return true;
        }
        if (leftNull) {
            if (rightFlags != 4) {
                result = right;
                return true;
            }
            std::vector<Value> rLeaves;
            if (!flattenDelegate(state, delegateTypeId, right, rLeaves)) return false;
            result = right;
            return true;
        }
        if (rightNull) {
            if (leftFlags != 4) {
                result = left;
                return true;
            }
            std::vector<Value> lLeaves;
            if (!flattenDelegate(state, delegateTypeId, left, lLeaves)) return false;
            result = left;
            return true;
        }
        if (leftFlags == 4) {
            const auto checkChild = [&](const Value& child) -> bool {
                if (child.rawTag() == ValueTag::NullObject) return true;
                ObjectFastView cv{};
                std::int64_t cf = -1;
                if (child.rawTag() != ValueTag::ObjectRef ||
                    !state.heap->tryBindObjectFastView(child.u_.objectRef, cv) ||
                    cv.typeId != delegateTypeId ||
                    cv.fieldCount != 8u) {
                    return fail(state, ErrorCode::TypeMismatch,
                        "delegate invocation list contains an incompatible value");
                }
                if (!cv.fields[3].tryGetInt64(cf) || cf < 0 || cf > 4) {
                    return fail(state, ErrorCode::InvalidProgram,
                        "delegate invocation-list metadata is corrupt");
                }
                return true;
            };
            if (!checkChild(leftView.fields[0]) || !checkChild(leftView.fields[7])) {
                return false;
            }
        }
        // O(1) single-node append when appending a single leaf delegate (the 99.9% `+=` path!)
        if (rightFlags != 4) {
            return allocateCombineDelegateNode(
                state, delegateTypeId, left, right, result, outSlotsMoved);
        }
        // Appending a multicast right subtree onto left in O(|right|) allocations
        std::vector<Value> rightLeaves;
        if (!flattenDelegate(state, delegateTypeId, right, rightLeaves)) return false;
        Value cur = left;
        for (const auto& rLeaf : rightLeaves) {
            Value nextNode;
            if (!allocateCombineDelegateNode(
                    state, delegateTypeId, cur, rLeaf, nextNode, outSlotsMoved)) {
                return false;
            }
            cur = nextNode;
        }
        result = cur;
        return true;
    }

    // RemoveDelegate (`-=`)
    if (leftNull) {
        result = Value{NullObject{}};
        return true;
    }
    if (rightNull) {
        if (leftFlags == 4) {
            std::vector<Value> lLeaves;
            if (!flattenDelegate(state, delegateTypeId, left, lLeaves)) return false;
        }
        result = left;
        return true;
    }
    if (leftFlags != 4 && rightFlags != 4) {
        if (sameDelegateLeaf(state, left, right)) {
            result = NullObject{};
        } else {
            result = left;
        }
        return true;
    }
    std::vector<Value> leftLeaves;
    std::vector<Value> rightLeaves;
    if (!flattenDelegate(state, delegateTypeId, left, leftLeaves) ||
        !flattenDelegate(state, delegateTypeId, right, rightLeaves)) {
        return false;
    }
    if (!rightLeaves.empty() && rightLeaves.size() <= leftLeaves.size()) {
        std::optional<std::size_t> removeAt;
        for (std::size_t start = 0;
             start + rightLeaves.size() <= leftLeaves.size();
             ++start) {
            bool matches = true;
            for (std::size_t index = 0; index < rightLeaves.size(); ++index) {
                if (!sameDelegateLeaf(state, leftLeaves[start + index], rightLeaves[index])) {
                    matches = false;
                    break;
                }
            }
            if (matches) removeAt = start;
        }
        if (!removeAt) {
            result = left;
            return true;
        }
        leftLeaves.erase(
            leftLeaves.begin() + static_cast<std::ptrdiff_t>(*removeAt),
            leftLeaves.begin() + static_cast<std::ptrdiff_t>(*removeAt + rightLeaves.size()));
    } else {
        result = left;
        return true;
    }
    return buildDelegateList(state, delegateTypeId, leftLeaves, result, outSlotsMoved);
}

bool resolveDelegateLeafFast(
    State& state,
    const detail::ExecutionMirror& mirror,
    ObjectRef leafRef,
    const ObjectFastView& view,
    std::size_t expectedUserArgs,
    DelegateCallSiteCache& siteCache,
    ResolvedDelegateLeaf& outLeaf) noexcept {
    if (view.fieldCount != 8u || !state.program || !state.heap) return false;
    std::int64_t flags = -1;
    std::int64_t slot = -1;
    if (!view.fields[3].tryGetInt64(flags) ||
        !view.fields[4].tryGetInt64(slot) ||
        flags < 0 || flags > 2 ||
        slot < 0 || slot > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    semantic::SymbolId targetSymId = 0;
    if (!readDelegateIdFromFields(view.fields, 1u, targetSymId)) {
        return false;
    }
    const Value& targetVal = view.fields[0];
    const bool hasRecv = (targetVal.rawTag() != ValueTag::NullObject);
    ObjectRef recvRef{};
    if (hasRecv) {
        if (targetVal.rawTag() == ValueTag::ObjectRef) {
            recvRef = targetVal.u_.objectRef;
            if (!state.heap->isAlive(recvRef)) return false;
        }
    }

    const ProgramImage::FunctionLocation* targetLoc = nullptr;
    const detail::ExecutionFunction* calleeExec = nullptr;

    if (flags != 0) {
        if (!hasRecv || targetVal.rawTag() != ValueTag::ObjectRef) return false;
        const semantic::SymbolId recvTid = state.heap->objectTypeIdFast(recvRef);
        if (recvTid == 0) return false;
        detail::PolymorphicCallSiteDesc polyDesc{};
        polyDesc.slot = static_cast<std::uint32_t>(slot);
        polyDesc.isInterface = (flags == 2);
        if (polyDesc.isInterface) {
            if (!readDelegateIdFromFields(view.fields, 5u, polyDesc.interfaceTypeId)) {
                return false;
            }
        }
        polyDesc.valid = true;
        if (const auto* pre = resolvePrelinkedPolymorphicTarget(mirror, recvTid, polyDesc)) {
            targetLoc = pre->target;
            calleeExec = pre->execution;
            targetSymId = pre->symbolId;
        } else {
            return false;
        }
    } else {
        if (siteCache.lastSymId == targetSymId &&
            siteCache.lastSymTarget != nullptr &&
            siteCache.lastSymExec != nullptr) {
            targetLoc = siteCache.lastSymTarget;
            calleeExec = siteCache.lastSymExec;
        } else {
            targetLoc = state.program->resolveFunction(targetSymId);
            if (!targetLoc || !targetLoc->function) return false;
            calleeExec = detail::findExecutionFunction(mirror, *targetLoc->function);
            if (!calleeExec) return false;
            siteCache.lastSymId = targetSymId;
            siteCache.lastSymTarget = targetLoc;
            siteCache.lastSymExec = calleeExec;
        }
    }

    if (!targetLoc || !targetLoc->function || !calleeExec) return false;
    const std::size_t expectedTotalParams = expectedUserArgs + (hasRecv ? 1u : 0u);
    if (targetLoc->function->parameterTypes.size() != expectedTotalParams) {
        return false;
    }

    outLeaf.leafRef = leafRef;
    outLeaf.receiverRef = recvRef;
    outLeaf.receiverVal = targetVal;
    outLeaf.target = targetLoc;
    outLeaf.execution = calleeExec;
    outLeaf.hasBoundReceiver = hasRecv;
    outLeaf.valid = true;
    return true;
}

bool flattenAndResolveMulticastFast(
    State& state,
    const detail::ExecutionMirror& mirror,
    semantic::SymbolId delegateTypeId,
    ObjectRef rootRef,
    std::size_t expectedUserArgs,
    DelegateCallSiteCache& siteCache,
    std::vector<ResolvedDelegateLeaf>& outLeaves) {
    outLeaves.clear();
    if (!state.heap) return false;

    std::array<ObjectRef, 64> inlineStack{};
    std::size_t inlineTop = 0;
    std::vector<ObjectRef> heapStack;
    inlineStack[inlineTop++] = rootRef;
    std::size_t visitedNodes = 0;

    while (inlineTop > 0 || !heapStack.empty()) {
        ObjectRef curRef{};
        if (!heapStack.empty()) {
            curRef = heapStack.back();
            heapStack.pop_back();
        } else {
            curRef = inlineStack[--inlineTop];
        }
        if (++visitedNodes > 65536u) return false;

        ObjectFastView view{};
        if (!state.heap->tryBindObjectFastView(curRef, view) ||
            view.typeId != delegateTypeId ||
            view.fieldCount != 8u) {
            return false;
        }
        std::int64_t flags = -1;
        if (!view.fields[3].tryGetInt64(flags) || flags < 0 || flags > 4) {
            return false;
        }
        if (flags != 4) {
            ResolvedDelegateLeaf leaf{};
            if (!resolveDelegateLeafFast(
                    state, mirror, curRef, view, expectedUserArgs, siteCache, leaf)) {
                return false;
            }
            outLeaves.push_back(std::move(leaf));
            continue;
        }
        const Value& leftVal = view.fields[0];
        const Value& rightVal = view.fields[7];
        const bool rightObj = (rightVal.rawTag() == ValueTag::ObjectRef);
        const bool leftObj = (leftVal.rawTag() == ValueTag::ObjectRef);
        if ((rightVal.rawTag() != ValueTag::NullObject && !rightObj) ||
            (leftVal.rawTag() != ValueTag::NullObject && !leftObj)) {
            return false;
        }
        if (rightObj) {
            if (!heapStack.empty() || inlineTop + 2u > inlineStack.size()) {
                heapStack.push_back(rightVal.u_.objectRef);
            } else {
                inlineStack[inlineTop++] = rightVal.u_.objectRef;
            }
        }
        if (leftObj) {
            if (!heapStack.empty() || inlineTop + 1u > inlineStack.size()) {
                heapStack.push_back(leftVal.u_.objectRef);
            } else {
                inlineStack[inlineTop++] = leftVal.u_.objectRef;
            }
        }
    }
    return true;
}



bool evaluateQuickInt(
    State& state,
    detail::QuickOpcode opcode,
    bool checkedArithmetic,
    std::int64_t left,
    std::int64_t right,
    Value& output) {
    return evaluateQuickIntInPlace(state, opcode, checkedArithmetic, left, right, output, nullptr);
}

bool executeQuickInstruction(
    State& state,
    const bytecode::Module& module,
    const detail::ExecutionInstruction& decoded,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers) {
    if (!consume(
            state,
            decoded.operationId,
            decoded.opcodeName ? std::string_view(decoded.opcodeName) : std::string_view{})) {
        return false;
    }
    switch (decoded.opcode) {
    case detail::QuickOpcode::LoadParameter:
        registers[decoded.result] = arguments[decoded.auxReg1];
        return true;
    case detail::QuickOpcode::ConstantInt:
        registers[decoded.result] = decoded.integerImmediate;
        return true;
    case detail::QuickOpcode::LoadLocal:
        registers[decoded.result] = locals[decoded.auxReg1];
        return true;
    case detail::QuickOpcode::StoreLocal:
        locals[decoded.auxReg1] = registers[decoded.op0];
        return true;
    case detail::QuickOpcode::CheckNotNullFast: {
        const auto& val = registers[decoded.op0];
        if (const auto* ref = std::get_if<ObjectRef>(&val)) {
            if (!checkCrossHeapRefAccess(state, *ref, false, "field access")) {
                return false;
            }
            if (state.heap && state.heap->isAlive(*ref)) {
                registers[decoded.result] = val;
                return true;
            }
        }
        if (std::holds_alternative<NullObject>(val)) {
            return fail(state, ErrorCode::NullReference,
                "field access attempted to dereference null");
        }
        return fail(state, ErrorCode::InvalidObjectReference,
            "field access contains an invalid managed object reference");
    }
    case detail::QuickOpcode::LoadElementIntFast: {
        const auto& arrVal = registers[decoded.op0];
        const auto& idxVal = registers[decoded.op1];
        if (const auto* ref = std::get_if<ObjectRef>(&arrVal)) {
            if (!checkCrossHeapRefAccess(state, *ref, false, "array element load")) {
                return false;
            }
            const auto idx = std::get<std::int64_t>(idxVal);
            if (idx >= 0 && state.heap) {
                if (const auto* elem = state.heap->arrayGetFast(
                        *ref, static_cast<std::size_t>(idx))) {
                    registers[decoded.result] = *elem;
                    return true;
                }
                if (auto packedElem = state.heap->arrayGet(
                        *ref, static_cast<std::size_t>(idx))) {
                    registers[decoded.result] = std::move(*packedElem);
                    return true;
                }
            }
            if (!state.heap || !state.heap->arrayLength(*ref)) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    "array element load references an invalid managed array");
            }
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        if (std::holds_alternative<NullArray>(arrVal)) {
            return fail(state, ErrorCode::NullReference,
                "array element load attempted to dereference null");
        }
        return fail(state, ErrorCode::InvalidObjectReference,
            "array element load contains an invalid managed object reference");
    }
    case detail::QuickOpcode::StoreElementIntFast: {
        const auto& arrVal = registers[decoded.op0];
        const auto& idxVal = registers[decoded.op1];
        const auto& storeVal = registers[decoded.op2];
        if (const auto* ref = std::get_if<ObjectRef>(&arrVal)) {
            if (!checkCrossHeapRefAccess(state, *ref, true, "array element store")) {
                return false;
            }
            const auto idx = std::get<std::int64_t>(idxVal);
            if (idx >= 0 && state.heap &&
                state.heap->arraySetPrevalidated(
                    *ref, static_cast<std::size_t>(idx), storeVal, false)) {
                return true;
            }
            if (!state.heap || !state.heap->arrayLength(*ref)) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    "array element store references an invalid managed array");
            }
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        if (std::holds_alternative<NullArray>(arrVal)) {
            return fail(state, ErrorCode::NullReference,
                "array element store attempted to dereference null");
        }
        return fail(state, ErrorCode::InvalidObjectReference,
            "array element store contains an invalid managed object reference");
    }
    case detail::QuickOpcode::CallDirect: {
        if (!decoded.directTarget || !decoded.directExecution) {
            return fail(state, ErrorCode::InvalidProgram,
                "direct call execution cache is missing");
        }
        Value callResult;
        const bool ok = (decoded.op0 == detail::InvalidCompactReg)
            ? executeFunction(
                state, *decoded.directTarget, ArgumentSource(registers),
                callResult, decoded.directExecution, true)
            : (decoded.op1 == detail::InvalidCompactReg)
                ? executeFunction(
                    state, *decoded.directTarget,
                    ArgumentSource(registers, static_cast<bytecode::Register>(decoded.op0)),
                    callResult, decoded.directExecution, true)
                : (decoded.op2 == detail::InvalidCompactReg)
                    ? executeFunction(
                        state, *decoded.directTarget,
                        ArgumentSource(
                            registers,
                            static_cast<bytecode::Register>(decoded.op0),
                            static_cast<bytecode::Register>(decoded.op1)),
                        callResult, decoded.directExecution, true)
                    : executeFunction(
                        state, *decoded.directTarget,
                        ArgumentSource(registers, decoded.bytecode->operands),
                        callResult, decoded.directExecution, true);
        if (!ok) {
            return false;
        }
        if (decoded.result == detail::InvalidCompactReg) return true;
        registers[decoded.result] = std::move(callResult);
        return true;
    }
    case detail::QuickOpcode::CallPolled: {
        const auto& instruction = *decoded.bytecode;
        const auto* receiver = std::get_if<ObjectRef>(&registers[decoded.op0]);
        if (receiver && state.heap && state.heap->isAlive(*receiver)) {
            const auto callSite = reinterpret_cast<std::uintptr_t>(&instruction);
            const auto idx = static_cast<std::size_t>((callSite >> 4U) & 63U);
            const auto& cached = (*state.inlineCallCache)[idx];
            if (cached.callSite == &instruction &&
                cached.receiver == *receiver &&
                cached.target && cached.execution) {
                Value callResult;
                if (!executeFunction(
                        state,
                        *cached.target,
                        (decoded.op1 == detail::InvalidCompactReg
                            ? ArgumentSource(registers, static_cast<bytecode::Register>(decoded.op0))
                            : ArgumentSource(registers, instruction.operands)),
                        callResult,
                        cached.execution,
                        true)) {
                    return false;
                }
                if (decoded.result == detail::InvalidCompactReg) return true;
                registers[decoded.result] = std::move(callResult);
                return true;
            }
        }
        Value callResult;
        if (!executeCall(state, module, instruction, registers, callResult)) {
            return false;
        }
        if (decoded.result == detail::InvalidCompactReg) return true;
        registers[decoded.result] = std::move(callResult);
        return true;
    }
    default:
        break;
    }

    const auto* leftPtr = std::get_if<std::int64_t>(&registers[decoded.op0]);
    const auto* rightPtr = std::get_if<std::int64_t>(&registers[decoded.op1]);
    if (!leftPtr || !rightPtr) {
        return fail(state, ErrorCode::TypeMismatch, "expected int operand in quick instruction");
    }
    Value output;
    if (!evaluateQuickInt(
            state, decoded.opcode, decoded.checkedArithmetic1, *leftPtr, *rightPtr, output)) {
        return false;
    }
    registers[decoded.result] = std::move(output);
    return true;
}

bool executeSuperInstruction(
    State& state,
    const detail::ExecutionInstruction& first,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers) {
    switch (first.superOpcode) {
    case detail::SuperOpcode::LoadParameterStoreLocal: {
        state.executed += 2;
        const auto& val = arguments[first.auxReg1];
        registers[first.result] = val;
        locals[first.storeSlot] = val;
        return true;
    }
    case detail::SuperOpcode::BinaryLocalIntConstant:
    case detail::SuperOpcode::BinaryLocalIntLocal:
    case detail::SuperOpcode::BinaryLocalIntConstantStoreLocal:
    case detail::SuperOpcode::BinaryLocalIntLocalStoreLocal: {
        const auto& leftVal = locals[first.auxReg1];
        registers[first.result] = leftVal;
        const auto* leftPtr = std::get_if<std::int64_t>(&leftVal);
        if (!leftPtr) return fail(state, ErrorCode::TypeMismatch, "expected int local");
        const auto left = *leftPtr;
        std::int64_t right = 0;
        if (first.superOpcode == detail::SuperOpcode::BinaryLocalIntConstant ||
            first.superOpcode == detail::SuperOpcode::BinaryLocalIntConstantStoreLocal) {
            right = first.integerImmediate;
            registers[first.auxReg2] = right;
        } else {
            const auto& rightVal = locals[first.op1];
            registers[first.auxReg2] = rightVal;
            const auto* rightPtr = std::get_if<std::int64_t>(&rightVal);
            if (!rightPtr) return fail(state, ErrorCode::TypeMismatch, "expected int local");
            right = *rightPtr;
        }
        state.executed += 3;
        Value output;
        if (!evaluateQuickInt(
                state, first.binaryOpcode1, first.checkedArithmetic1,
                left, right, output)) {
            return false;
        }
        registers[first.superResult] = output;
        if (first.span == 4) {
            ++state.executed;
            locals[first.storeSlot] = std::move(output);
        }
        return true;
    }
    case detail::SuperOpcode::BinaryLocalIntReg:
    case detail::SuperOpcode::BinaryLocalIntRegStoreLocal: {
        const auto& leftVal = locals[first.auxReg1];
        registers[first.result] = leftVal;
        const auto* leftPtr = std::get_if<std::int64_t>(&leftVal);
        const auto* rightPtr = std::get_if<std::int64_t>(&registers[first.op1]);
        if (!leftPtr || !rightPtr) return fail(state, ErrorCode::TypeMismatch, "expected int operand");
        const auto left = *leftPtr;
        const auto right = *rightPtr;
        state.executed += 2;
        Value output;
        if (!evaluateQuickInt(
                state, first.binaryOpcode1, first.checkedArithmetic1,
                left, right, output)) {
            return false;
        }
        registers[first.superResult] = output;
        if (first.span == 3) {
            ++state.executed;
            locals[first.storeSlot] = std::move(output);
        }
        return true;
    }
    case detail::SuperOpcode::BinaryLocalIntRegChainStoreLocal: {
        const auto& leftVal = locals[first.auxReg1];
        registers[first.result] = leftVal;
        const auto* leftPtr = std::get_if<std::int64_t>(&leftVal);
        const auto* right1Ptr = std::get_if<std::int64_t>(&registers[first.op1]);
        if (!leftPtr || !right1Ptr) return fail(state, ErrorCode::TypeMismatch, "expected int operand");
        const auto left = *leftPtr;
        const auto right1 = *right1Ptr;
        state.executed += 2;
        Value midOutput;
        if (!evaluateQuickInt(
                state, first.binaryOpcode1, first.checkedArithmetic1,
                left, right1, midOutput)) {
            return false;
        }
        registers[first.auxReg2] = midOutput;
        const auto midInt = std::get<std::int64_t>(midOutput);
        const auto* right2Ptr = std::get_if<std::int64_t>(&registers[first.op2]);
        if (!right2Ptr) return fail(state, ErrorCode::TypeMismatch, "expected int operand");
        const auto right2 = *right2Ptr;
        ++state.executed;
        Value finalOutput;
        if (!evaluateQuickInt(
                state, first.binaryOpcode2, first.checkedArithmetic2,
                midInt, right2, finalOutput)) {
            return false;
        }
        registers[first.superResult] = finalOutput;
        ++state.executed;
        locals[first.storeSlot] = std::move(finalOutput);
        return true;
    }
    default:
        return false;
    }
}


std::string extractScriptExceptionMessage(const State& state, const Value& excVal) {
    if (!state.heap || !std::holds_alternative<ObjectRef>(excVal)) {
        return "unhandled script exception in async function";
    }
    const ObjectRef ref = std::get<ObjectRef>(excVal);
    const auto tid = state.heap->objectTypeId(ref);
    const auto* typeDesc = (tid && state.program) ? state.program->resolveType(*tid) : nullptr;
    if (typeDesc) {
        for (std::size_t f = 0; f < typeDesc->fields.size(); ++f) {
            if (typeDesc->fields[f].type == semantic::PrimitiveType::String) {
                if (const auto fv = state.heap->fieldGet(ref, f)) {
                    if (!std::holds_alternative<NullString>(*fv) &&
                        !std::holds_alternative<std::monostate>(*fv)) {
                        const auto s = valueToString(*fv, state.heap);
                        if (!s.empty()) return s;
                    }
                }
            }
        }
        if (!typeDesc->name.empty()) return typeDesc->name;
    }
    return "unhandled script exception in async function";
}

void resumeInterpreterCoroutineCallback(
    void* runnerContext,
    CoroutineFrameHandle frameHandle,
    CoroutineFrame& frame,
    AsyncRuntimeContext& asyncContext);

namespace {

[[nodiscard]] Value makePendingAsyncReturnCarrier(
    State& state,
    const bytecode::Function& function,
    AwaitableHandle completionHandle) {
    if (function.returnType == semantic::PrimitiveType::Object &&
        state.heap != nullptr && state.program != nullptr &&
        function.returnTypeId != 0) {
        if (const auto* ts = state.program->resolveType(function.returnTypeId)) {
            std::vector<Value> fields;
            std::vector<std::size_t> refFields;
            fields.reserve(ts->fields.size());
            for (std::size_t idx = 0; idx < ts->fields.size(); ++idx) {
                const auto& f = ts->fields[idx];
                Value fv{};
                switch (f.type) {
                case semantic::PrimitiveType::Bool: fv = Value(false); break;
                case semantic::PrimitiveType::Byte:
                case semantic::PrimitiveType::SByte:
                case semantic::PrimitiveType::Short:
                case semantic::PrimitiveType::UShort:
                case semantic::PrimitiveType::Int:
                case semantic::PrimitiveType::UInt: fv = Value(std::int64_t{0}); break;
                case semantic::PrimitiveType::Long:
                case semantic::PrimitiveType::ULong:
                    fv = idx == 0
                        ? Value(LongValue{static_cast<std::int64_t>(completionHandle.pack64())})
                        : Value(LongValue{0});
                    break;
                case semantic::PrimitiveType::Char: fv = Value(CharValue{0}); break;
                case semantic::PrimitiveType::Float:
                case semantic::PrimitiveType::Double: fv = Value(0.0); break;
                case semantic::PrimitiveType::String:
                    fv = Value(NullString{});
                    refFields.push_back(idx);
                    break;
                case semantic::PrimitiveType::Object:
                    fv = Value(NullObject{});
                    refFields.push_back(idx);
                    break;
                case semantic::PrimitiveType::Array:
                    fv = Value(NullArray{});
                    refFields.push_back(idx);
                    break;
                default:
                    fv = Value{};
                    break;
                }
                fields.push_back(std::move(fv));
            }
            RuntimeError allocErr;
            if (const auto objRef = state.heap->allocateObject(
                    function.returnTypeId,
                    std::move(fields),
                    std::move(refFields),
                    &allocErr)) {
                return Value(*objRef);
            }
        }
        return Value(NullObject{});
    }
    return makeAwaitableStructValue(function.returnTypeId, completionHandle);
}

} // namespace

bool suspendInterpreterCoroutine(
    State& state,
    const bytecode::Function& function,
    CoroutineFrameHandle& ioActiveCoroutine,
    const Value* argsPtr,
    std::size_t argCount,
    const Value* localsPtr,
    std::size_t localCount,
    const Value* regsPtr,
    std::size_t regCount,
    std::uint32_t resumeBlockId,
    AwaitableHandle awaitedHandle,
    Value& outAwaitableStruct,
    bool runInlineOnComplete) {
    if (!state.asyncRuntime) {
        return fail(state, ErrorCode::InvalidProgram, "async suspension requires an AsyncRuntimeContext");
    }
    AwaitableHandle completionHandle{};
    if (!ioActiveCoroutine.valid()) {
        completionHandle = state.asyncRuntime->awaitables().allocatePending(ResumeQueue::Update);
        ioActiveCoroutine = state.asyncRuntime->coroutines().allocateFrame(
            function.symbolId,
            function.returnTypeId,
            completionHandle,
            ResumeQueue::Update);
    } else {
        const auto* existing = state.asyncRuntime->coroutines().resolve(ioActiveCoroutine);
        if (!existing) {
            return fail(state, ErrorCode::InvalidProgram, "active coroutine frame handle is stale");
        }
        if (existing->invalidated) {
            if (awaitedHandle.valid() && !awaitedHandle.isCompletedVoidSentinel()) {
                (void)state.asyncRuntime->awaitables().discard(awaitedHandle);
            }
            return fail(state, ErrorCode::ExecutionTerminated, "coroutine invalidated by hot reload");
        }
        completionHandle = existing->completionHandle;
    }

    std::shared_ptr<const void> ownerCopy{};
    if (state.resumeEnvOwner && *state.resumeEnvOwner) {
        const auto* cur = state.asyncRuntime->coroutines().resolve(ioActiveCoroutine);
        if (!cur || !cur->runnerOwner) {
            ownerCopy = *state.resumeEnvOwner;
        }
    }

    const auto detachSliceArrayIfNeeded = [](const Value* src, std::size_t cnt, std::vector<Value>& buf) -> const Value* {
        if (!src || cnt == 0) return src;
        for (std::size_t i = 0; i < cnt; ++i) {
            if (src[i].rawTag() == ValueTag::Struct && src[i].asStructUnchecked().isFrameSlice()) {
                buf.resize(cnt);
                for (std::size_t k = 0; k < cnt; ++k) {
                    buf[k] = detachFrameSliceValue(src[k]);
                }
                return buf.data();
            }
        }
        return src;
    };
    std::vector<Value> detachedArgs, detachedLocals, detachedRegs;
    const Value* safeArgsPtr = detachSliceArrayIfNeeded(argsPtr, argCount, detachedArgs);
    const Value* safeLocalsPtr = detachSliceArrayIfNeeded(localsPtr, localCount, detachedLocals);
    const Value* safeRegsPtr = detachSliceArrayIfNeeded(regsPtr, regCount, detachedRegs);

    if (!state.asyncRuntime->coroutines().suspendFrame(
            ioActiveCoroutine,
            safeArgsPtr,
            argCount,
            safeLocalsPtr,
            localCount,
            safeRegsPtr,
            regCount,
            resumeBlockId,
            awaitedHandle,
            &resumeInterpreterCoroutineCallback,
            state.resumeEnvRaw,
            std::move(ownerCopy),
            runInlineOnComplete)) {
        return fail(state, ErrorCode::InvalidProgram, "failed to suspend coroutine onto awaited handle");
    }
    outAwaitableStruct = makePendingAsyncReturnCarrier(state, function, completionHandle);
    return true;
}

bool suspendInterpreterWithAwaitScheduler(
    State& state,
    const bytecode::Function& function,
    CoroutineFrameHandle& ioActiveCoroutine,
    const Value* argsPtr,
    std::size_t argCount,
    const Value* localsPtr,
    std::size_t localCount,
    const Value* regsPtr,
    std::size_t regCount,
    std::uint32_t resumeBlockId,
    const Value& awaitableVal,
    Value& outReturnCarrier) {
    if (!state.asyncRuntime || !state.awaitScheduler) {
        return fail(state, ErrorCode::InvalidProgram, "custom await suspension requires AsyncRuntimeContext and IAwaitScheduler");
    }
    const bool hadActiveCoro = ioActiveCoroutine.valid();
    AwaitableHandle bridgeHandle =
        state.asyncRuntime->awaitables().allocatePending(ResumeQueue::Update);
    if (!suspendInterpreterCoroutine(
            state,
            function,
            ioActiveCoroutine,
            argsPtr,
            argCount,
            localsPtr,
            localCount,
            regsPtr,
            regCount,
            resumeBlockId,
            bridgeHandle,
            outReturnCarrier,
            /*runInlineOnComplete=*/true)) {
        (void)state.asyncRuntime->awaitables().discard(bridgeHandle);
        return false;
    }

    std::weak_ptr<AsyncRuntimeContext> weakAsync;
    bool hasWeakAsync = false;
    std::shared_ptr<const void> envOwner;
    if (state.resumeEnvOwner && *state.resumeEnvOwner) {
        envOwner = *state.resumeEnvOwner;
    }
    if (state.resumeEnvRaw) {
        weakAsync = static_cast<const InterpreterResumeEnv*>(state.resumeEnvRaw)->asyncRuntimeWeak;
        hasWeakAsync = !weakAsync.expired();
    }

    auto fired = std::make_shared<std::atomic<bool>>(false);
    AsyncRuntimeContext* const rawAsyncCtx = state.asyncRuntime;
    const CoroutineFrameHandle coroHandle = ioActiveCoroutine;
    std::function<void()> resumeCallback =
        [fired = std::move(fired),
         weakAsync = std::move(weakAsync),
         hasWeakAsync,
         envOwner = std::move(envOwner),
         rawAsyncCtx,
         coroHandle,
         bridgeHandle]() {
            if (fired->exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            std::shared_ptr<AsyncRuntimeContext> lockedAsync;
            AsyncRuntimeContext* ctx = nullptr;
            if (hasWeakAsync) {
                lockedAsync = weakAsync.lock();
                ctx = lockedAsync.get();
            } else {
                ctx = rawAsyncCtx;
            }
            if (!ctx) {
                return;
            }
            const auto* frame = ctx->coroutines().resolve(coroHandle);
            if (!frame || frame->invalidated) {
                (void)ctx->awaitables().discard(bridgeHandle);
                return;
            }
            (void)ctx->awaitables().trySetResult(bridgeHandle);
            (void)ctx->awaitables().discard(bridgeHandle);
        };

    RuntimeError schedErr;
    if (!state.awaitScheduler->onSuspend(awaitableVal, std::move(resumeCallback), schedErr)) {
        (void)state.asyncRuntime->awaitables().discard(bridgeHandle);
        if (!hadActiveCoro && ioActiveCoroutine.valid()) {
            AwaitableHandle allocatedCompletion{};
            if (const auto* f = state.asyncRuntime->coroutines().resolve(ioActiveCoroutine)) {
                allocatedCompletion = f->completionHandle;
            }
            state.asyncRuntime->coroutines().recycleFrame(ioActiveCoroutine);
            ioActiveCoroutine = CoroutineFrameHandle{};
            if (allocatedCompletion.valid() && !allocatedCompletion.isCompletedVoidSentinel()) {
                (void)state.asyncRuntime->awaitables().discard(allocatedCompletion);
            }
        } else if (ioActiveCoroutine.valid()) {
            if (auto* f = state.asyncRuntime->coroutines().resolve(ioActiveCoroutine)) {
                f->suspended = false;
            }
        }
        return fail(state, schedErr.code == ErrorCode::None ? ErrorCode::InvalidProgram : schedErr.code,
            schedErr.message.empty() ? "custom await scheduler rejected suspension" : schedErr.message);
    }
    return true;
}

bool resumeInterpreterAwaitable(
    State& state,
    const Value& awaitableVal,
    bool hasResult,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    Value& outResumedVal) {
    if (state.awaitScheduler && state.awaitScheduler->handlesValue(awaitableVal)) {
        RuntimeError schedErr;
        Value payload{};
        if (!state.awaitScheduler->tryGetResult(awaitableVal, hasResult, payload, schedErr)) {
            return fail(
                state,
                schedErr.code == ErrorCode::None ? ErrorCode::ScriptException : schedErr.code,
                schedErr.message.empty() ? "custom awaiter GetResult failed" : schedErr.message);
        }
        if (hasResult) {
            if (!expectSignatureType(state, payload, expectedType, expectedTypeId, "await result")) {
                return false;
            }
            outResumedVal = std::move(payload);
        }
        return true;
    }
    AwaitableHandle awaitedHandle{};
    if (!state.asyncRuntime || !tryExtractAwaitableHandle(awaitableVal, awaitedHandle)) {
        return fail(state, ErrorCode::InvalidProgram, "await.resume local does not hold a valid Awaitable");
    }
    Value payload{};
    RuntimeError faultErr{};
    Value excObj{};
    const AwaitStatus st = state.asyncRuntime->awaitables().getResult(
        awaitedHandle, payload, faultErr, &excObj);
    if (st == AwaitStatus::Succeeded) {
        if (hasResult) {
            if (!expectSignatureType(state, payload, expectedType, expectedTypeId, "await result")) {
                return false;
            }
            outResumedVal = std::move(payload);
        }
        return true;
    }
    if (st == AwaitStatus::Canceled || st == AwaitStatus::Faulted) {
        const auto matRef = materializeAsyncResumeException(
            state.heap, st, faultErr, excObj);
        if (!matRef) {
            return fail(state, ErrorCode::OutOfMemory, "failed to allocate async resume exception");
        }
        state.pendingException = Value(*matRef);
        return false;
    }
    return fail(state, ErrorCode::InvalidProgram, "await.resume encountered non-terminal Awaitable");
}

bool resolveInterpreterCoroutine(
    State& state,
    CoroutineFrameHandle activeCoroutine,
    semantic::SymbolId returnStructTypeId,
    const Value* optionalPayload,
    Value& outWrappedStruct) {
    if (activeCoroutine.valid() && state.asyncRuntime) {
        auto* frame = state.asyncRuntime->coroutines().resolve(activeCoroutine);
        if (!frame) {
            return fail(state, ErrorCode::InvalidProgram, "async.resolve coroutine frame is invalid");
        }
        frame->completed = true;
        const Value val = optionalPayload ? detachFrameSliceValue(*optionalPayload) : Value{};
        (void)state.asyncRuntime->awaitables().trySetResult(frame->completionHandle, val);
        if (state.awaitScheduler) {
            RuntimeError schedErr;
            if (state.awaitScheduler->tryResolveAsyncReturn(
                    returnStructTypeId,
                    optionalPayload ? &val : nullptr,
                    outWrappedStruct,
                    schedErr)) {
                return true;
            }
            if (schedErr.code != ErrorCode::None) {
                return fail(state, schedErr.code, schedErr.message);
            }
        }
        outWrappedStruct = makeAwaitableStructValue(returnStructTypeId, frame->completionHandle);
        return true;
    }
    if (state.awaitScheduler) {
        RuntimeError schedErr;
        const Value detached = optionalPayload ? detachFrameSliceValue(*optionalPayload) : Value{};
        if (state.awaitScheduler->tryResolveAsyncReturn(
                returnStructTypeId,
                optionalPayload ? &detached : nullptr,
                outWrappedStruct,
                schedErr)) {
            return true;
        }
        if (schedErr.code != ErrorCode::None) {
            return fail(state, schedErr.code, schedErr.message);
        }
    }
    if (!state.asyncRuntime) {
        return fail(state, ErrorCode::InvalidProgram, "async.resolve requires an AsyncRuntimeContext");
    }
    AwaitableHandle h{};
    if (!optionalPayload) {
        h = AwaitableHandle::completedVoid();
    } else {
        h = state.asyncRuntime->awaitables().fromResult(detachFrameSliceValue(*optionalPayload));
    }
    outWrappedStruct = makeAwaitableStructValue(returnStructTypeId, h);
    return true;
}

bool finishUncaughtInterpreterAsyncException(
    State& state,
    const bytecode::Function& function,
    CoroutineFrameHandle activeCoroutine,
    Value& outResult) {
    if (!state.asyncRuntime || !state.pendingException.has_value()) {
        return false;
    }
    const Value excVal = *state.pendingException;
    state.pendingException.reset();
    state.error = RuntimeError{};

    const bool isCanceled = isAsyncCanceledExceptionObject(state.heap, excVal);
    if (activeCoroutine.valid()) {
        auto* frame = state.asyncRuntime->coroutines().resolve(activeCoroutine);
        if (!frame) return false;
        frame->completed = true;
        if (isCanceled) {
            (void)state.asyncRuntime->awaitables().trySetCanceled(frame->completionHandle);
        } else {
            std::string msg = extractScriptExceptionMessage(state, excVal);
            (void)state.asyncRuntime->awaitables().trySetFaulted(
                frame->completionHandle,
                ErrorCode::ScriptException,
                std::move(msg),
                excVal);
        }
        outResult = makeAwaitableStructValue(function.returnTypeId, frame->completionHandle);
        return true;
    }
    AwaitableHandle h = isCanceled
        ? state.asyncRuntime->awaitables().fromCanceled()
        : state.asyncRuntime->awaitables().fromFaulted(
              ErrorCode::ScriptException,
              extractScriptExceptionMessage(state, excVal),
              excVal);
    outResult = makeAwaitableStructValue(function.returnTypeId, h);
    return true;
}

bool executeInstruction(
    State& state,
    const bytecode::Module& module,
    const bytecode::Function& function,
    const bytecode::Instruction& instruction,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers) {
    if (!consume(
            state,
            instructionOperationId(instruction.opcode),
            bytecode::opcodeName(instruction.opcode))) {
        return false;
    }
    auto operand = [&](std::size_t index) -> const Value* {
        return &registers[instruction.operands[index]];
    };
    auto storeResult = [&](Value value) {
        registers[instruction.result] = std::move(value);
        return true;
    };

    switch (instruction.opcode) {
    case bytecode::Opcode::LoadParameter:
        return storeResult(arguments[instruction.index]);
    case bytecode::Opcode::ConstantInt: {
        const auto resultType = function.registerTypes[instruction.result];
        if (resultType == semantic::PrimitiveType::Long) {
            return storeResult(LongValue{instruction.integerImmediate});
        }
        if (resultType == semantic::PrimitiveType::Enum) {
            return storeResult(EnumValue{
                typeIdAt(function.registerTypeIds, instruction.result),
                instruction.integerImmediate});
        }
        return storeResult(instruction.integerImmediate);
    }
    case bytecode::Opcode::ConstantDouble:
        return storeResult(instruction.doubleImmediate);
    case bytecode::Opcode::ConstantBool:
        return storeResult(instruction.boolImmediate);
    case bytecode::Opcode::ConstantString: {
        if (!state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "string allocation requires a managed heap");
        }
        RuntimeError allocationError;
        const auto reference = state.heap->internString(
            instruction.stringImmediate,
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed string allocation failed"
                    : allocationError.message);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::ConstantNull:
        return storeResult(std::monostate{});
    case bytecode::Opcode::LoadLocal:
        return storeResult(locals[instruction.index]);
    case bytecode::Opcode::StoreLocal: {
        const auto* value = operand(0);
        if (!value) return false;
        locals[instruction.index] = *value;
        return true;
    }
    case bytecode::Opcode::ConvertNullToString: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullString{});
    }
    case bytecode::Opcode::ConvertNullToObject: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullObject{});
    }
    case bytecode::Opcode::ConvertNullToArray: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullArray{});
    }
    case bytecode::Opcode::ConvertIntToLong: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int,
                "numeric conversion")) return false;
        return storeResult(LongValue{std::get<std::int64_t>(*value)});
    }
    case bytecode::Opcode::ConvertIntToDouble: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int,
                "numeric conversion")) return false;
        return storeResult(static_cast<double>(std::get<std::int64_t>(*value)));
    }
    case bytecode::Opcode::ConvertLongToDouble: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Long,
                "numeric conversion")) return false;
        return storeResult(static_cast<double>(std::get<LongValue>(*value).value));
    }
    case bytecode::Opcode::ConvertNumeric: {
        const auto* value = operand(0);
        if (!value || instruction.result == bytecode::InvalidRegister ||
            instruction.result >= function.registerTypes.size()) {
            return fail(
                state, ErrorCode::InvalidProgram,
                "numeric conversion metadata is invalid");
        }
        Value converted;
        if (!tryConvertNumeric(
                *value, function.registerTypes[instruction.result], converted,
                instruction.checkedArithmetic)) {
            return fail(
                state, ErrorCode::IntegerOverflow,
                "numeric conversion overflow");
        }
        return storeResult(std::move(converted));
    }
    case bytecode::Opcode::ConstantTypeId:
        return storeResult(ULongValue{
            static_cast<std::uint64_t>(instruction.integerImmediate)});
    case bytecode::Opcode::IsType:
    case bytecode::Opcode::AsType: {
        const auto* value = operand(0);
        if (!value || instruction.operands.front() >=
                function.registerTypes.size()) {
            return fail(
                state, ErrorCode::InvalidProgram,
                "runtime type operation metadata is invalid");
        }
        const auto sourceType = function.registerTypes[
            instruction.operands.front()];
        const bool nullValue =
            std::holds_alternative<std::monostate>(*value) ||
            std::holds_alternative<NullString>(*value) ||
            std::holds_alternative<NullObject>(*value) ||
            std::holds_alternative<NullArray>(*value);
        bool matches = false;
        if (!nullValue) {
            if (instruction.elementType == semantic::PrimitiveType::Object) {
                const auto* reference = std::get_if<ObjectRef>(value);
                if (reference && !checkCrossHeapRefAccess(state, *reference, false, "runtime type check")) {
                    return false;
                }
                const auto actual = reference && state.heap
                    ? state.heap->objectTypeId(*reference)
                    : std::optional<semantic::SymbolId>{};
                matches = actual && runtimeAssignable(
                    state, *actual, instruction.elementTypeId);
            } else if (instruction.elementType ==
                       semantic::PrimitiveType::Array) {
                const auto* reference = std::get_if<ObjectRef>(value);
                if (reference && !checkCrossHeapRefAccess(state, *reference, false, "runtime type check")) {
                    return false;
                }
                const auto actual = reference && state.heap
                    ? state.heap->arrayTypeId(*reference)
                    : std::optional<semantic::SymbolId>{};
                matches = actual && *actual == instruction.elementTypeId;
            } else {
                matches = sourceType == instruction.elementType;
            }
        }
        if (instruction.opcode == bytecode::Opcode::IsType) {
            return storeResult(matches);
        }
        if (matches) return storeResult(*value);
        if (instruction.elementType == semantic::PrimitiveType::String) {
            return storeResult(NullString{});
        }
        if (instruction.elementType == semantic::PrimitiveType::Array) {
            return storeResult(NullArray{});
        }
        return storeResult(NullObject{});
    }
    case bytecode::Opcode::NewArray: {
        const auto* lengthValue = operand(0);
        if (!lengthValue ||
            !expectType(state, *lengthValue, semantic::PrimitiveType::Int,
                "array length") ||
            !state.heap) {
            return false;
        }
        const auto length = std::get<std::int64_t>(*lengthValue);
        if (length < 0) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array length cannot be negative");
        }
        if (static_cast<std::uint64_t>(length) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return fail(state, ErrorCode::OutOfMemory, "array length is too large");
        }
        if (instruction.result >= function.registerTypeIds.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "array allocation result type metadata is missing");
        }
        RuntimeError allocationError;
        const auto reference = state.heap->allocateTypedArray(
            function.registerTypeIds[instruction.result],
            instruction.elementType,
            instruction.elementTypeId,
            static_cast<std::size_t>(length),
            (instruction.elementType == semantic::PrimitiveType::Struct &&
             instruction.elementTypeId != 0 &&
             findFlatStructSchemaByTypeId(instruction.elementTypeId) != nullptr)
                ? Value{}
                : defaultValue(module, instruction.elementType, instruction.elementTypeId),
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed array allocation failed"
                    : allocationError.message);
        }
        if (state.telemetry != nullptr && state.heap->isPackedArray(*reference)) {
            state.telemetry->noteCompactHeapObject(*reference);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::NewObject: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || type->interfaceType || type->delegateType ||
            !state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "object allocation references an invalid class or heap");
        }
        std::vector<Value> fields;
        std::vector<std::size_t> referenceFields;
        fields.reserve(type->fields.size());
        for (std::size_t index = 0; index < type->fields.size(); ++index) {
            const auto fieldType = type->fields[index].type;
            const auto fieldTypeId =
                exactTypeId(fieldType, type->fields[index].typeName);
            fields.push_back(defaultValue(
                module, fieldType, fieldTypeId));
            if (fieldType == semantic::PrimitiveType::String ||
                fieldType == semantic::PrimitiveType::Object ||
                fieldType == semantic::PrimitiveType::Array ||
                (fieldType == semantic::PrimitiveType::Struct &&
                 !makeDefaultInlineStructValue(fieldTypeId).has_value())) {
                referenceFields.push_back(index);
            }
        }
        RuntimeError allocationError;
        const auto reference = state.heap->allocateObject(
            type->id,
            std::move(fields),
            std::move(referenceFields),
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed object allocation failed"
                    : allocationError.message);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::NewStruct: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || type->kind != semantic::TypeKind::Struct) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct allocation references an invalid type descriptor");
        }
        if (const auto inlined = makeDefaultInlineStructValue(type->id)) {
            return storeResult(*inlined);
        }
        if (type->fields.size() <= StructValue::MaxInlineFields) {
            std::array<Value, StructValue::MaxInlineFields> stackFields{};
            for (std::size_t i = 0; i < type->fields.size(); ++i) {
                const auto& field = type->fields[i];
                stackFields[i] = defaultValue(
                    module, field.type, exactTypeId(field.type, field.typeName));
            }
            return storeResult(makeStructValueFromFields(
                type->id, ValueSpan(stackFields.data(), type->fields.size())));
        }
        std::vector<Value> fields;
        fields.reserve(type->fields.size());
        for (const auto& field : type->fields) {
            fields.push_back(defaultValue(
                module, field.type, exactTypeId(field.type, field.typeName)));
        }
        return storeResult(makeStructValueFromFields(type->id, std::move(fields)));
    }
    case bytecode::Opcode::ArrayLength: {
        const auto* receiver = operand(0);
        if (!receiver) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array length attempted to dereference null");
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array length")) {
            return false;
        }
        const auto* reference = std::get_if<ObjectRef>(receiver);
        const auto length = reference && state.heap
            ? state.heap->arrayLength(*reference)
            : std::optional<std::size_t>{};
        if (!length || *length >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array length references an invalid managed array");
        }
        return storeResult(static_cast<std::int64_t>(*length));
    }
    case bytecode::Opcode::LoadElement: {
        const auto* receiver = operand(0);
        const auto* indexValue = operand(1);
        if (!receiver || !indexValue) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array element load attempted to dereference null");
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array element load") ||
            !expectType(state, *indexValue, semantic::PrimitiveType::Int,
                "array index")) {
            return false;
        }
        const auto index = std::get<std::int64_t>(*indexValue);
        const auto reference = std::get<ObjectRef>(*receiver);
        const auto length = state.heap->arrayLength(reference);
        if (!length) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array element load references an invalid managed array");
        }
        if (index < 0 || static_cast<std::uint64_t>(index) >= *length) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        const auto elementType = state.heap->arrayElementType(reference);
        const auto elementTypeId = state.heap->arrayElementTypeId(reference);
        if (!elementType || !elementTypeId ||
            *elementType != instruction.elementType ||
            *elementTypeId != instruction.elementTypeId) {
            return fail(state, ErrorCode::TypeMismatch,
                "array element metadata does not match the bytecode operation");
        }
        const auto value = state.heap->arrayGet(
            reference, static_cast<std::size_t>(index));
        if (!value || !expectSignatureType(
                state, *value, instruction.elementType,
                instruction.elementTypeId, "array element value")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::StoreElement: {
        const auto* receiver = operand(0);
        const auto* indexValue = operand(1);
        const auto* value = operand(2);
        if (!receiver || !indexValue || !value) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array element store attempted to dereference null");
        }
        if (const auto* recvRef = std::get_if<ObjectRef>(receiver)) {
            if (!checkCrossHeapRefAccess(state, *recvRef, true, "array element store")) {
                return false;
            }
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array element store") ||
            !expectType(state, *indexValue, semantic::PrimitiveType::Int,
                "array index") ||
            !expectSignatureType(state, *value, instruction.elementType,
                instruction.elementTypeId, "array element store value")) {
            return false;
        }
        const auto index = std::get<std::int64_t>(*indexValue);
        const auto reference = std::get<ObjectRef>(*receiver);
        const auto length = state.heap->arrayLength(reference);
        if (!length) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array element store references an invalid managed array");
        }
        if (index < 0 || static_cast<std::uint64_t>(index) >= *length) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        const auto elementType = state.heap->arrayElementType(reference);
        const auto elementTypeId = state.heap->arrayElementTypeId(reference);
        if (!elementType || !elementTypeId ||
            *elementType != instruction.elementType ||
            *elementTypeId != instruction.elementTypeId) {
            return fail(state, ErrorCode::TypeMismatch,
                "array element metadata does not match the bytecode operation");
        }
        RuntimeError arrayError;
        if (!state.heap->arraySet(
                reference,
                static_cast<std::size_t>(index),
                *value,
                &arrayError,
                instruction.elementType == semantic::PrimitiveType::Object
                    ? ArrayStoreTypePolicy::PrevalidatedAssignableObject
                    : ArrayStoreTypePolicy::Exact)) {
            return fail(state,
                arrayError.code == ErrorCode::None
                    ? ErrorCode::InvalidProgram
                    : arrayError.code,
                arrayError.message.empty()
                    ? "array element store failed"
                    : arrayError.message);
        }
        return true;
    }
    case bytecode::Opcode::CheckNotNull: {
        const auto* value = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!value || !type || !expectObject(state, *value, *type, false, "field access")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::LoadField: {
        const auto* receiver = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !type ||
            instruction.index >= type->fields.size() ||
            !expectObject(state, *receiver, *type, false, "field load")) {
            return false;
        }
        const auto reference = std::get<ObjectRef>(*receiver);
        if (reference.isStackSlotRef()) {
            const auto& field = type->fields[instruction.index];
            Value stackVal{};
            if (reference.stackRefSchemaId() == 0u) {
                stackVal = state.values[reference.slot];
            } else {
                stackVal = StructValue::makeFrameSlice(
                    exactTypeId(field.type, field.typeName),
                    state.flatBankId,
                    reference.slot,
                    reference.stackRefSchemaId());
            }
            if (!expectSignatureType(
                    state,
                    stackVal,
                    field.type,
                    exactTypeId(field.type, field.typeName),
                    "field load value")) {
                return false;
            }
            return storeResult(stackVal);
        }
        const auto value = state.heap->fieldGet(reference, instruction.index);
        if (!value) {
            return fail(state, ErrorCode::InvalidProgram,
                "field load references an invalid field slot");
        }
        const auto& field = type->fields[instruction.index];
        if (!expectSignatureType(
                state,
                *value,
                field.type,
                exactTypeId(field.type, field.typeName),
                "field load value")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::StoreField: {
        const auto* receiver = operand(0);
        const auto* value = operand(1);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !value || !type ||
            instruction.index >= type->fields.size()) {
            return false;
        }
        if (const auto* recvRef = std::get_if<ObjectRef>(receiver)) {
            if (!checkCrossHeapRefAccess(state, *recvRef, true, "field store")) {
                return false;
            }
        }
        if (!expectObject(state, *receiver, *type, false, "field store") ||
            !expectSignatureType(
                state,
                *value,
                type->fields[instruction.index].type,
                exactTypeId(
                    type->fields[instruction.index].type,
                    type->fields[instruction.index].typeName),
                "field store value")) {
            return false;
        }
        const auto recvRef = std::get<ObjectRef>(*receiver);
        if (!recvRef.isStackSlotRef() &&
            state.crossHeapPolicy != CrossHeapAccessPolicy::AllowReadWrite &&
            state.heap && recvRef.valid() &&
            recvRef.heapId != static_cast<std::uint32_t>(state.heap->heapId())) {
            return fail(
                state,
                ErrorCode::CrossHeapAccessViolation,
                "cross-heap field write rejected by active CrossHeapAccessPolicy");
        }
        if (recvRef.isStackSlotRef()) {
            if (recvRef.stackRefSchemaId() == 0u) {
                state.values[recvRef.slot] = detachFrameSliceValue(*value);
                return true;
            }
            const auto* sc = getFlatStructSchema(recvRef.stackRefSchemaId());
            if (!sc || !extractStructLeavesToBuffer(*value, *sc, state.values.data() + recvRef.slot)) {
                return fail(state, ErrorCode::InvalidProgram, "stack ref struct store failed");
            }
            return true;
        }
        RuntimeError fieldError;
        if (!state.heap->fieldSet(
                recvRef,
                instruction.index,
                *value,
                &fieldError)) {
            return fail(state,
                fieldError.code == ErrorCode::None
                    ? ErrorCode::InvalidProgram
                    : fieldError.code,
                fieldError.message.empty()
                    ? "field store failed"
                    : fieldError.message);
        }
        return true;
    }
    case bytecode::Opcode::LoadStructField: {
        const auto* receiver = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !type || type->kind != semantic::TypeKind::Struct ||
            instruction.index >= type->fields.size() ||
            !expectSignatureType(state, *receiver, semantic::PrimitiveType::Struct,
                type->id, "struct field load")) {
            return false;
        }
        const auto& field = type->fields[instruction.index];
        if (const auto inlinedField = loadInlineStructField(*receiver, instruction.index)) {
            if (!expectSignatureType(state, *inlinedField, field.type,
                    exactTypeId(field.type, field.typeName), "struct field load value")) {
                return false;
            }
            return storeResult(*inlinedField);
        }
        const auto& structure = std::get<StructValue>(*receiver);
        const auto fieldValOpt = structValueFieldAt(structure, instruction.index);
        if (!fieldValOpt) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct field load references an invalid field slot");
        }
        if (!expectSignatureType(state, *fieldValOpt, field.type,
                exactTypeId(field.type, field.typeName), "struct field load value")) {
            return false;
        }
        return storeResult(*fieldValOpt);
    }
    case bytecode::Opcode::StoreStructField: {
        const auto* receiver = operand(0);
        const auto* value = operand(1);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !value || !type || type->kind != semantic::TypeKind::Struct ||
            instruction.index >= type->fields.size() ||
            !expectSignatureType(state, *receiver, semantic::PrimitiveType::Struct,
                type->id, "struct field store")) {
            return false;
        }
        const auto& field = type->fields[instruction.index];
        if (!expectSignatureType(state, *value, field.type,
                exactTypeId(field.type, field.typeName), "struct field store value")) {
            return false;
        }
        const Value cleanVal = detachFrameSliceValue(*value);
        if (const auto updatedInline = storeInlineStructField(*receiver, instruction.index, cleanVal)) {
            return storeResult(*updatedInline);
        }
        const auto& structure = std::get<StructValue>(*receiver);
        if (structure.isFrameSlice()) {
            const std::size_t fCount = structure.fieldCount();
            if (instruction.index >= fCount) {
                return fail(state, ErrorCode::InvalidProgram,
                    "struct field store references an invalid field slot");
            }
            std::vector<Value> fields(fCount);
            for (std::size_t fIdx = 0; fIdx < fCount; ++fIdx) {
                fields[fIdx] = fIdx == instruction.index
                    ? cleanVal
                    : detachFrameSliceValue(structValueFieldAt(structure, fIdx).value_or(Value{}));
            }
            return storeResult(makeStructValueFromFields(type->id, std::move(fields)));
        }
        if (!structure.storage || instruction.index >= structure.storage->fields.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct field store references an invalid field slot");
        }
        auto storage = std::make_shared<StructStorage>(*structure.storage);
        storage->fields[instruction.index] = cleanVal;
        return storeResult(StructValue{type->id, std::move(storage)});
    }
    case bytecode::Opcode::NewDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || !type->delegateType || !state.heap ||
            instruction.index >= module.functionReferences.size() ||
            instruction.operands.size() > 1) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate creation has invalid metadata");
        }
        const auto& reference = module.functionReferences[instruction.index];
        Value target = NullObject{};
        if (!instruction.operands.empty()) {
            const auto* value = operand(0);
            if (!value || reference.parameterTypes.empty() ||
                !expectSignatureType(
                    state, *value, reference.parameterTypes.front(),
                    typeIdAt(reference.parameterTypeIds, 0),
                    "delegate target")) {
                return false;
            }
            target = *value;
        } else if (!reference.parameterTypes.empty() &&
                   (reference.virtualDispatch ||
                    reference.interfaceDispatch)) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch target has no receiver");
        }
        std::uint64_t flags = 0;
        if (reference.virtualDispatch) flags |= 1u;
        if (reference.interfaceDispatch) flags |= 2u;
        const std::array<Value, 8> fields{{
            std::move(target),
            delegateWord(reference.symbolId, false),
            delegateWord(reference.symbolId, true),
            static_cast<std::int64_t>(flags),
            static_cast<std::int64_t>(reference.virtualDispatch
                ? reference.virtualSlot
                : reference.interfaceDispatch
                    ? reference.interfaceSlot
                    : 0u),
            delegateWord(reference.interfaceTypeId, false),
            delegateWord(reference.interfaceTypeId, true),
            NullObject{},
        }};
        RuntimeError error;
        const auto allocated = state.heap->allocateObjectFromTemplate(
            type->id, fields.data(), fields.size(), delegateReferenceFields(), &error);
        if (!allocated) {
            return fail(state,
                error.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : error.code,
                error.message.empty()
                    ? "delegate allocation failed"
                    : error.message);
        }
        return storeResult(*allocated);
    }
    case bytecode::Opcode::CombineDelegate:
    case bytecode::Opcode::RemoveDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        const auto* left = operand(0);
        const auto* right = operand(1);
        if (!type || !type->delegateType || !left || !right ||
            !state.heap ||
            !expectSignatureType(
                state, *left, semantic::PrimitiveType::Object,
                type->id, "delegate combination left operand") ||
            !expectSignatureType(
                state, *right, semantic::PrimitiveType::Object,
                type->id, "delegate combination right operand")) {
            return false;
        }
        Value combined;
        if (!combineOrRemoveDelegateFastImpl(
                state,
                type->id,
                *left,
                *right,
                instruction.opcode == bytecode::Opcode::RemoveDelegate,
                combined)) {
            return false;
        }
        return storeResult(std::move(combined));
    }
    case bytecode::Opcode::InvokeDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        const auto* delegateValue = operand(0);
        if (!type || !type->delegateType || !delegateValue ||
            !state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation has invalid metadata");
        }
        if (std::holds_alternative<NullObject>(*delegateValue)) {
            return fail(state, ErrorCode::NullReference,
                "delegate invocation attempted to dereference null");
        }
        const auto* delegateRef = std::get_if<ObjectRef>(delegateValue);
        if (delegateRef && !checkCrossHeapRefAccess(state, *delegateRef, false, "delegate invocation")) {
            return false;
        }
        const auto actualType = delegateRef
            ? state.heap->objectTypeId(*delegateRef)
            : std::optional<semantic::SymbolId>{};
        if (!delegateRef || !actualType || *actualType != type->id ||
            state.heap->fieldCount(*delegateRef) != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate invocation received an incompatible value");
        }
        const auto rootFlagsValue = state.heap->fieldGet(
            *delegateRef, 3u);
        const auto* rootFlags = rootFlagsValue
            ? std::get_if<std::int64_t>(&*rootFlagsValue)
            : nullptr;
        if (rootFlags && *rootFlags == 4) {
            std::vector<Value> leaves;
            if (!flattenDelegate(
                    state, type->id, *delegateValue, leaves)) {
                return false;
            }
            Value lastResult;
            for (const auto& leaf : leaves) {
                std::vector<Value> nestedRegisterValues;
                nestedRegisterValues.reserve(registers.size());
                for (std::size_t index = 0; index < registers.size(); ++index) {
                    nestedRegisterValues.push_back(registers[index]);
                }
                ValueWindow nestedRegisters{
                    &nestedRegisterValues, 0, nestedRegisterValues.size()};
                nestedRegisters[instruction.operands.front()] = leaf;
                if (!executeInstruction(
                        state, module, function, instruction,
                        arguments, locals, nestedRegisters)) {
                    return false;
                }
                if (instruction.result !=
                    bytecode::InvalidRegister) {
                    lastResult = nestedRegisters[instruction.result];
                }
            }
            if (instruction.result ==
                bytecode::InvalidRegister) {
                return true;
            }
            return storeResult(std::move(lastResult));
        }
        const semantic::FunctionSymbol* invoke = nullptr;
        for (const auto& method : type->methods) {
            if (method.name == "Invoke") {
                invoke = &method;
                break;
            }
        }
        if (!invoke || invoke->parameters.empty() ||
            instruction.operands.size() != invoke->parameters.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation signature is invalid");
        }
        std::vector<Value> delegateArguments;
        delegateArguments.reserve(instruction.operands.size());
        for (std::size_t index = 1;
             index < instruction.operands.size(); ++index) {
            const auto* value = operand(index);
            const auto& parameter = invoke->parameters[index];
            const auto parameterType =
                semantic::storageTypeOf(parameter);
            if (!value || !expectSignatureType(
                    state, *value, parameterType,
                    exactTypeId(
                        parameterType,
                        semantic::storageTypeNameOf(parameter)),
                    "delegate argument")) {
                return false;
            }
            delegateArguments.push_back(*value);
        }
        semantic::SymbolId targetSymbolId = 0;
        if (!readDelegateId(
                state, *delegateRef, 1u, targetSymbolId,
                "delegate callable identity is corrupt")) {
            return false;
        }
        const auto target = state.heap->fieldGet(*delegateRef, 0u);
        const auto flagsValue = state.heap->fieldGet(*delegateRef, 3u);
        const auto slotValue = state.heap->fieldGet(*delegateRef, 4u);
        const auto* flags = flagsValue
            ? std::get_if<std::int64_t>(&*flagsValue)
            : nullptr;
        const auto* slot = slotValue
            ? std::get_if<std::int64_t>(&*slotValue)
            : nullptr;
        if (!target || !flags || !slot || *flags < 0 || *flags > 3 ||
            *slot < 0 ||
            *slot > std::numeric_limits<std::uint32_t>::max()) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch metadata is corrupt");
        }
        const bool virtualDispatch = (*flags & 1) != 0;
        const bool interfaceDispatch = (*flags & 2) != 0;
        if (virtualDispatch && interfaceDispatch) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch modes conflict");
        }
        if (!std::holds_alternative<NullObject>(*target)) {
            delegateArguments.insert(delegateArguments.begin(), *target);
        }
        if (virtualDispatch || interfaceDispatch) {
            const auto* receiver = delegateArguments.empty()
                ? nullptr
                : std::get_if<ObjectRef>(&delegateArguments.front());
            if (receiver && !checkCrossHeapRefAccess(state, *receiver, false, "delegate invocation")) {
                return false;
            }
            const auto runtimeType = receiver
                ? state.heap->objectTypeId(*receiver)
                : std::optional<semantic::SymbolId>{};
            if (!runtimeType || !state.program) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    "delegate receiver has no runtime type");
            }
            const auto* foundType = state.program->resolveType(*runtimeType);
            if (!foundType) {
                return fail(state, ErrorCode::InvalidProgram,
                    "delegate receiver descriptor is unavailable");
            }
            const auto dispatchSlot = static_cast<std::uint32_t>(*slot);
            if (virtualDispatch) {
                if (dispatchSlot >=
                    foundType->virtualDispatchTable.size()) {
                    return fail(state, ErrorCode::InvalidProgram,
                        "delegate virtual slot is invalid");
                }
                targetSymbolId =
                    foundType->virtualDispatchTable[dispatchSlot];
            } else {
                semantic::SymbolId interfaceTypeId = 0;
                if (!readDelegateId(
                        state, *delegateRef, 5u, interfaceTypeId,
                        "delegate interface identity is corrupt")) {
                    return false;
                }
                const auto* map = runtimeInterfaceMap(
                    state, *runtimeType, interfaceTypeId);
                if (!map || dispatchSlot >= map->slots.size()) {
                    return fail(state, ErrorCode::InvalidProgram,
                        "delegate interface slot is invalid");
                }
                targetSymbolId = map->slots[dispatchSlot];
            }
        }
        const auto* found = state.program
            ? state.program->resolveFunction(targetSymbolId)
            : nullptr;
        if (!found) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate target function is unavailable");
        }
        Value delegateResult;
        if (!executeFunction(
                state, *found, delegateArguments, delegateResult)) {
            return false;
        }
        if (instruction.result == bytecode::InvalidRegister) return true;
        return storeResult(std::move(delegateResult));
    }
    case bytecode::Opcode::Call: {
        Value callResult;
        if (!executeCall(state, module, instruction, registers, callResult)) return false;
        if (instruction.result == bytecode::InvalidRegister) return true;
        return storeResult(std::move(callResult));
    }
    case bytecode::Opcode::AsyncEnter:
        return true;
    case bytecode::Opcode::AwaitSuspend: {
        const auto* val = operand(0);
        if (!val || instruction.index >= locals.size()) {
            return fail(state, ErrorCode::InvalidProgram, "invalid await.suspend operand or local");
        }
        locals[instruction.index] = *val;
        if (state.awaitScheduler && state.awaitScheduler->handlesValue(*val)) {
            if (state.awaitScheduler->isCompleted(*val)) {
                return true;
            }
            if (!state.currentBlockVmCoroutinePtr) {
                return fail(state, ErrorCode::InvalidProgram, "missing coroutine context in block VM");
            }
            Value pendingRet{};
            const Value* argsRaw = arguments.size() == 0 ? nullptr : &arguments.values->at(arguments.offset);
            const Value* locsRaw = locals.size() == 0 ? nullptr : &locals.values->at(locals.offset);
            const Value* regsRaw = registers.size() == 0 ? nullptr : &registers.values->at(registers.offset);
            if (!suspendInterpreterWithAwaitScheduler(
                    state,
                    function,
                    *state.currentBlockVmCoroutinePtr,
                    argsRaw,
                    arguments.size(),
                    locsRaw,
                    locals.size(),
                    regsRaw,
                    registers.size(),
                    static_cast<std::uint32_t>(instruction.activeIntegerImmediate()),
                    *val,
                    pendingRet)) {
                return false;
            }
            state.suspendedReturnValue = std::move(pendingRet);
            state.coroutineSuspendedNow = true;
            return true;
        }
        if (!state.asyncRuntime) {
            return fail(state, ErrorCode::InvalidProgram, "invalid await.suspend operand or local");
        }
        AwaitableHandle awaitedHandle{};
        if (!tryExtractAwaitableHandle(*val, awaitedHandle) ||
            !state.asyncRuntime->awaitables().isValid(awaitedHandle)) {
            return fail(state, ErrorCode::InvalidProgram, "awaited value does not hold a valid Awaitable handle");
        }
        if (state.asyncRuntime->awaitables().isCompleted(awaitedHandle)) {
            return true;
        }
        if (!state.currentBlockVmCoroutinePtr) {
            return fail(state, ErrorCode::InvalidProgram, "missing coroutine context in block VM");
        }
        Value pendingRet{};
        const Value* argsRaw = arguments.size() == 0 ? nullptr : &arguments.values->at(arguments.offset);
        const Value* locsRaw = locals.size() == 0 ? nullptr : &locals.values->at(locals.offset);
        const Value* regsRaw = registers.size() == 0 ? nullptr : &registers.values->at(registers.offset);
        if (!suspendInterpreterCoroutine(
                state,
                function,
                *state.currentBlockVmCoroutinePtr,
                argsRaw,
                arguments.size(),
                locsRaw,
                locals.size(),
                regsRaw,
                registers.size(),
                static_cast<std::uint32_t>(instruction.activeIntegerImmediate()),
                awaitedHandle,
                pendingRet)) {
            return false;
        }
        state.suspendedReturnValue = std::move(pendingRet);
        state.coroutineSuspendedNow = true;
        return true;
    }
    case bytecode::Opcode::AwaitResume: {
        if (instruction.index >= locals.size()) {
            return fail(state, ErrorCode::InvalidProgram, "invalid await.resume local");
        }
        const Value awaitableVal = locals[instruction.index];
        locals[instruction.index] = Value{};
        Value resumedVal{};
        const bool hasRes = (instruction.result != bytecode::InvalidRegister);
        if (!resumeInterpreterAwaitable(
                state,
                awaitableVal,
                hasRes,
                instruction.elementType,
                instruction.activeElementTypeId(),
                resumedVal)) {
            return false;
        }
        if (!hasRes) return true;
        return storeResult(std::move(resumedVal));
    }
    case bytecode::Opcode::AsyncResolve: {
        const auto* retTypeDesc = typeDescriptor(module, instruction.typeIndex);
        if (!retTypeDesc) {
            return fail(state, ErrorCode::InvalidProgram, "invalid async.resolve metadata");
        }
        if (!state.awaitScheduler && !state.currentBlockVmCoroutinePtr) {
            return fail(state, ErrorCode::InvalidProgram, "invalid async.resolve metadata");
        }
        const Value* payloadPtr = instruction.operands.empty() ? nullptr : operand(0);
        Value wrapped{};
        const CoroutineFrameHandle activeCoro = state.currentBlockVmCoroutinePtr
            ? *state.currentBlockVmCoroutinePtr
            : CoroutineFrameHandle{};
        if (!resolveInterpreterCoroutine(
                state,
                activeCoro,
                retTypeDesc->id,
                payloadPtr,
                wrapped)) {
            return false;
        }
        return storeResult(std::move(wrapped));
    }
    case bytecode::Opcode::NegateInt: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int, "negation")) return false;
        Value output;
        const auto number = std::get<std::int64_t>(*value);
        if (!instruction.checkedArithmetic &&
            number == std::numeric_limits<std::int32_t>::min()) {
            return storeResult(number);
        }
        if (!checkedIntResult(state, -number, output)) return false;
        return storeResult(std::move(output));
    }
    case bytecode::Opcode::NegateLong: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Long,
                "negation")) return false;
        const auto number = std::get<LongValue>(*value).value;
        if (number == std::numeric_limits<std::int64_t>::min()) {
            if (!instruction.checkedArithmetic) return storeResult(LongValue{number});
            return fail(state, ErrorCode::IntegerOverflow, "long negation overflow");
        }
        return storeResult(LongValue{-number});
    }
    case bytecode::Opcode::NegateDouble: {
        const auto* value = operand(0);
        if (value && valueType(*value) == semantic::PrimitiveType::Float) {
            return storeResult(FloatValue{-std::get<FloatValue>(*value).value});
        }
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Double,
                "negation")) return false;
        return storeResult(-std::get<double>(*value));
    }
    case bytecode::Opcode::LogicalNot: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Bool, "logical not")) return false;
        return storeResult(!std::get<bool>(*value));
    }
    default:
        break;
    }

    const auto* left = operand(0);
    const auto* right = operand(1);
    if (!left || !right) return false;

    if (instruction.opcode == bytecode::Opcode::Equal || instruction.opcode == bytecode::Opcode::NotEqual) {
        bool equal = false;
        if (!valuesEqual(state, *left, *right, equal)) return false;
        return storeResult(instruction.opcode == bytecode::Opcode::Equal ? equal : !equal);
    }
    const auto isIntOpcode =
        instruction.opcode >= bytecode::Opcode::AddInt &&
        instruction.opcode <= bytecode::Opcode::GreaterOrEqualInt;
    if (isIntOpcode && instruction.operands.size() >= 2 &&
        instruction.operands[0] < function.registerTypes.size() &&
        instruction.operands[1] < function.registerTypes.size() &&
        function.registerTypes[instruction.operands[0]] ==
            semantic::PrimitiveType::Int &&
        function.registerTypes[instruction.operands[1]] ==
            semantic::PrimitiveType::Int) {
        const auto* typedLeft = std::get_if<std::int64_t>(left);
        const auto* typedRight = std::get_if<std::int64_t>(right);
        if (!typedLeft || !typedRight) {
            return fail(state, ErrorCode::TypeMismatch,
                "binary operation expected int operands");
        }
        const auto a = *typedLeft;
        const auto b = *typedRight;
        Value intOutput;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddInt:
            if (!instruction.checkedArithmetic) {
                return storeResult(wrapInt32(a + b));
            }
            return checkedIntResult(state, a + b, intOutput) &&
                storeResult(std::move(intOutput));
        case bytecode::Opcode::SubtractInt:
            if (!instruction.checkedArithmetic) {
                return storeResult(wrapInt32(a - b));
            }
            return checkedIntResult(state, a - b, intOutput) &&
                storeResult(std::move(intOutput));
        case bytecode::Opcode::MultiplyInt:
            if (!instruction.checkedArithmetic) {
                return storeResult(wrapInt32(a * b));
            }
            return checkedIntResult(state, a * b, intOutput) &&
                storeResult(std::move(intOutput));
        case bytecode::Opcode::DivideInt:
            if (b == 0) {
                return fail(state, ErrorCode::DivisionByZero,
                    "division by zero");
            }
            if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
                return fail(state, ErrorCode::IntegerOverflow,
                    "integer division overflow");
            }
            return storeResult(a / b);
        case bytecode::Opcode::RemainderInt:
            if (b == 0) {
                return fail(state, ErrorCode::DivisionByZero,
                    "remainder by zero");
            }
            if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
                return storeResult(std::int64_t{0});
            }
            return storeResult(a % b);
        case bytecode::Opcode::LessInt: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualInt: return storeResult(a <= b);
        case bytecode::Opcode::GreaterInt: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualInt: return storeResult(a >= b);
        default: break;
        }
    }
    const auto isLongOpcode = instruction.opcode >= bytecode::Opcode::AddLong &&
        instruction.opcode <= bytecode::Opcode::GreaterOrEqualLong;
    const auto isDoubleOpcode = instruction.opcode >= bytecode::Opcode::AddDouble &&
        instruction.opcode <= bytecode::Opcode::GreaterOrEqualDouble;
    Value output;
    if (isLongOpcode) {
        if (valueType(*left) == semantic::PrimitiveType::ULong) {
            if (!expectType(state, *right, semantic::PrimitiveType::ULong,
                    "binary operation")) return false;
            const auto a = std::get<ULongValue>(*left).value;
            const auto b = std::get<ULongValue>(*right).value;
            switch (instruction.opcode) {
            case bytecode::Opcode::AddLong:
                if (instruction.checkedArithmetic &&
                    b > std::numeric_limits<std::uint64_t>::max() - a)
                    return fail(state, ErrorCode::IntegerOverflow, "ulong addition overflow");
                return storeResult(ULongValue{a + b});
            case bytecode::Opcode::SubtractLong:
                if (instruction.checkedArithmetic && a < b) return fail(state, ErrorCode::IntegerOverflow, "ulong subtraction overflow");
                return storeResult(ULongValue{a - b});
            case bytecode::Opcode::MultiplyLong:
                if (instruction.checkedArithmetic && b != 0 &&
                    a > std::numeric_limits<std::uint64_t>::max() / b)
                    return fail(state, ErrorCode::IntegerOverflow, "ulong multiplication overflow");
                return storeResult(ULongValue{a * b});
            case bytecode::Opcode::DivideLong:
                if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
                return storeResult(ULongValue{a / b});
            case bytecode::Opcode::RemainderLong:
                if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
                return storeResult(ULongValue{a % b});
            case bytecode::Opcode::LessLong: return storeResult(a < b);
            case bytecode::Opcode::LessOrEqualLong: return storeResult(a <= b);
            case bytecode::Opcode::GreaterLong: return storeResult(a > b);
            case bytecode::Opcode::GreaterOrEqualLong: return storeResult(a >= b);
            default: break;
            }
        }
        if (!expectType(state, *left, semantic::PrimitiveType::Long, "binary operation") ||
            !expectType(state, *right, semantic::PrimitiveType::Long, "binary operation")) {
            return false;
        }
        const auto a = std::get<LongValue>(*left).value;
        const auto b = std::get<LongValue>(*right).value;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) +
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongAdd(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::SubtractLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) -
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongSubtract(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::MultiplyLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) *
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongMultiply(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::DivideLong:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return fail(state, ErrorCode::IntegerOverflow, "long division overflow");
            }
            return storeResult(LongValue{a / b});
        case bytecode::Opcode::RemainderLong:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return storeResult(LongValue{});
            }
            return storeResult(LongValue{a % b});
        case bytecode::Opcode::LessLong: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualLong: return storeResult(a <= b);
        case bytecode::Opcode::GreaterLong: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualLong: return storeResult(a >= b);
        default: break;
        }
    }
    if (isDoubleOpcode) {
        if (valueType(*left) == semantic::PrimitiveType::Float) {
            if (!expectType(state, *right, semantic::PrimitiveType::Float,
                    "binary operation")) return false;
            const auto a = std::get<FloatValue>(*left).value;
            const auto b = std::get<FloatValue>(*right).value;
            switch (instruction.opcode) {
            case bytecode::Opcode::AddDouble: return storeResult(FloatValue{a + b});
            case bytecode::Opcode::SubtractDouble: return storeResult(FloatValue{a - b});
            case bytecode::Opcode::MultiplyDouble: return storeResult(FloatValue{a * b});
            case bytecode::Opcode::DivideDouble: return storeResult(FloatValue{a / b});
            case bytecode::Opcode::LessDouble: return storeResult(a < b);
            case bytecode::Opcode::LessOrEqualDouble: return storeResult(a <= b);
            case bytecode::Opcode::GreaterDouble: return storeResult(a > b);
            case bytecode::Opcode::GreaterOrEqualDouble: return storeResult(a >= b);
            default: break;
            }
        }
        if (!expectType(state, *left, semantic::PrimitiveType::Double, "binary operation") ||
            !expectType(state, *right, semantic::PrimitiveType::Double, "binary operation")) {
            return false;
        }
        const auto a = std::get<double>(*left);
        const auto b = std::get<double>(*right);
        switch (instruction.opcode) {
        case bytecode::Opcode::AddDouble: return storeResult(a + b);
        case bytecode::Opcode::SubtractDouble: return storeResult(a - b);
        case bytecode::Opcode::MultiplyDouble: return storeResult(a * b);
        case bytecode::Opcode::DivideDouble:
            // Floating-point division follows IEEE 754. Zero divisors produce
            // infinities or NaN rather than an integer-style runtime trap.
            return storeResult(a / b);
        case bytecode::Opcode::LessDouble: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualDouble: return storeResult(a <= b);
        case bytecode::Opcode::GreaterDouble: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualDouble: return storeResult(a >= b);
        default: break;
        }
    }
    if (valueType(*left) == semantic::PrimitiveType::UInt) {
        if (!expectType(state, *right, semantic::PrimitiveType::UInt,
                "binary operation")) return false;
        const auto a = std::get<UIntValue>(*left).value;
        const auto b = std::get<UIntValue>(*right).value;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddInt:
            if (instruction.checkedArithmetic &&
                b > std::numeric_limits<std::uint32_t>::max() - a)
                return fail(state, ErrorCode::IntegerOverflow, "uint addition overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a + b)});
        case bytecode::Opcode::SubtractInt:
            if (instruction.checkedArithmetic && a < b) return fail(state, ErrorCode::IntegerOverflow, "uint subtraction overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a - b)});
        case bytecode::Opcode::MultiplyInt:
            if (instruction.checkedArithmetic && b != 0 &&
                a > std::numeric_limits<std::uint32_t>::max() / b)
                return fail(state, ErrorCode::IntegerOverflow, "uint multiplication overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a * b)});
        case bytecode::Opcode::DivideInt:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a / b)});
        case bytecode::Opcode::RemainderInt:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a % b)});
        case bytecode::Opcode::LessInt: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualInt: return storeResult(a <= b);
        case bytecode::Opcode::GreaterInt: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualInt: return storeResult(a >= b);
        default: break;
        }
    }
    if (!expectType(state, *left, semantic::PrimitiveType::Int, "binary operation") ||
        !expectType(state, *right, semantic::PrimitiveType::Int, "binary operation")) return false;
    const auto a = std::get<std::int64_t>(*left);
    const auto b = std::get<std::int64_t>(*right);
    switch (instruction.opcode) {
    case bytecode::Opcode::AddInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a + b));
        return checkedIntResult(state, a + b, output) && storeResult(output);
    case bytecode::Opcode::SubtractInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a - b));
        return checkedIntResult(state, a - b, output) && storeResult(output);
    case bytecode::Opcode::MultiplyInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a * b));
        return checkedIntResult(state, a * b, output) && storeResult(output);
    case bytecode::Opcode::DivideInt:
        if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
        if (a == std::numeric_limits<std::int32_t>::min() && b == -1)
            return fail(state, ErrorCode::IntegerOverflow, "integer division overflow");
        return storeResult(a / b);
    case bytecode::Opcode::RemainderInt:
        if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
        if (a == std::numeric_limits<std::int32_t>::min() && b == -1) return storeResult(std::int64_t{0});
        return storeResult(a % b);
    case bytecode::Opcode::LessInt: return storeResult(a < b);
    case bytecode::Opcode::LessOrEqualInt: return storeResult(a <= b);
    case bytecode::Opcode::GreaterInt: return storeResult(a > b);
    case bytecode::Opcode::GreaterOrEqualInt: return storeResult(a >= b);
    default:
        return fail(state, ErrorCode::InvalidProgram, "unsupported bytecode opcode");
    }
}

bool executeBlockVmFunction(
    State& state,
    const ProgramImage::FunctionLocation& location,
    const detail::ExecutionFunction& executionObj,
    const ArgumentSource& arguments,
    CoroutineFrameHandle& activeCoroutine,
    CoroutineFrame* activeCoroutineFrame,
    Value& result) {
    const auto& function = *location.function;
    const detail::ExecutionFunction* const execution = &executionObj;
    const auto previousFunctionId = state.currentFunctionId;
    state.currentFunctionId = function.symbolId;
    struct RestoreFunctionId {
        State& state;
        semantic::SymbolId previous;
        ~RestoreFunctionId() { state.currentFunctionId = previous; }
    } restoreFunctionId{state, previousFunctionId};
    state.stack.push_back(&location.qualifiedName);
    ++state.statistics.functionCalls;
    state.statistics.maximumCallDepth = std::max(state.statistics.maximumCallDepth, state.stack.size());
    emitTrace(state, TraceEventKind::FunctionEnter);
    ArenaFrameScope frame(state);
    const std::size_t blockArgCount = activeCoroutineFrame
        ? activeCoroutineFrame->savedArguments.size()
        : arguments.size();
    const auto argumentOffset = frame.base();
    const auto localOffset = argumentOffset + blockArgCount;
    const auto registerOffset = localOffset + function.localTypes.size();
    const auto scratchOffset = registerOffset + function.registerTypes.size();
    frame.allocate(
        blockArgCount + function.localTypes.size() +
        function.registerTypes.size() + execution->edgeScratchCount);
    if (activeCoroutineFrame != nullptr) {
        for (std::size_t index = 0; index < blockArgCount; ++index) {
            state.values[argumentOffset + index] = activeCoroutineFrame->savedArguments[index];
        }
        const std::size_t nLocs = std::min(
            activeCoroutineFrame->savedLocals.size(), function.localTypes.size());
        for (std::size_t index = 0; index < nLocs; ++index) {
            state.values[localOffset + index] = activeCoroutineFrame->savedLocals[index];
        }
        const std::size_t nRegs = std::min(
            activeCoroutineFrame->savedRegisters.size(), function.registerTypes.size());
        for (std::size_t index = 0; index < nRegs; ++index) {
            state.values[registerOffset + index] = activeCoroutineFrame->savedRegisters[index];
        }
    } else {
        for (std::size_t index = 0; index < blockArgCount; ++index) {
            state.values[argumentOffset + index] = arguments[index];
        }
    }
    ValueWindow argumentWindow{&state.values, argumentOffset, blockArgCount};
    ValueWindow locals{&state.values, localOffset, function.localTypes.size()};
    ValueWindow registers{
        &state.values, registerOffset, function.registerTypes.size()};
    ValueWindow edgeScratch{
        &state.values, scratchOffset, execution->edgeScratchCount};
    DebugFrameScope debugFrame(state, state.debugger
        ? debug::DebugFrameView{
            location.module,
            &function,
            nullptr,
            argumentWindow.debugView(),
            locals.debugView(),
            registers.debugView(),
        }
        : debug::DebugFrameView{});
    CoroutineFrameHandle* const prevBlockCoroPtr = state.currentBlockVmCoroutinePtr;
    state.currentBlockVmCoroutinePtr = &activeCoroutine;
    struct RestoreBlockCoroPtr {
        State& st;
        CoroutineFrameHandle* prev;
        ~RestoreBlockCoroPtr() { st.currentBlockVmCoroutinePtr = prev; }
    } restoreBlockCoroPtr{state, prevBlockCoroPtr};
    const detail::ExecutionBlock* block = activeCoroutineFrame
        ? detail::findExecutionBlock(*execution, activeCoroutineFrame->resumeBlockId)
        : &execution->blocks[execution->entry];
    if (!block) {
        state.stack.pop_back();
        return fail(state, ErrorCode::InvalidProgram, "invalid coroutine resume block in block VM");
    }

    const auto routePendingException = [&](bytecode::BlockId protectedBlock) {
        if (!state.pendingException ||
            !std::holds_alternative<ObjectRef>(*state.pendingException)) {
            return static_cast<const detail::ExecutionBlock*>(nullptr);
        }
        const auto reference = std::get<ObjectRef>(*state.pendingException);
        const auto actual = state.heap->objectTypeId(reference);
        if (!actual) return static_cast<const detail::ExecutionBlock*>(nullptr);
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
            if (handler.exceptionLocal >= locals.size()) {
                return static_cast<const detail::ExecutionBlock*>(nullptr);
            }
            locals[handler.exceptionLocal] = *state.pendingException;
            state.pendingException.reset();
            return detail::findExecutionBlock(*execution, handler.handlerBlock);
        }
        return static_cast<const detail::ExecutionBlock*>(nullptr);
    };

    while (true) {
        const auto& bytecodeBlock = *block->bytecode;
        if (state.debugger && !state.debugFrames.empty()) {
            state.debugFrames.back().activeBlockId = bytecodeBlock.id;
        }
        bool exceptionTransferred = false;
        for (std::size_t instructionIndex = 0;
             instructionIndex < block->instructions.size();
             ++instructionIndex) {
            const auto& decodedInstruction = block->instructions[instructionIndex];
            if (state.debugger && execution->hasDebugSequencePoints &&
                !debugSequencePoint(
                    state, *location.module, function, bytecodeBlock.id,
                    static_cast<std::uint32_t>(instructionIndex), false)) {
                state.stack.pop_back();
                return false;
            }
            std::size_t executionSpan = 1;
            bool executed = false;
            const auto canExecuteSuperInstruction =
                state.fastAccounting &&
                decodedInstruction.superOpcode != detail::SuperOpcode::None &&
                state.executed <= state.limits.instructionBudget &&
                decodedInstruction.span <=
                    state.limits.instructionBudget - state.executed;
            if (canExecuteSuperInstruction) {
                executionSpan = decodedInstruction.span;
                executed = executeSuperInstruction(
                    state, decodedInstruction,
                    argumentWindow, locals, registers);
                if (executed) {
                    stepGcIfNeeded(state, executionSpan);
                }
            } else if (decodedInstruction.opcode ==
                    detail::QuickOpcode::Generic ||
                    decodedInstruction.opcode >= detail::QuickOpcode::ConstantDouble) {
                executed = executeInstruction(
                    state, *location.module, function, *decodedInstruction.bytecode,
                    argumentWindow, locals, registers);
            } else {
                executed = executeQuickInstruction(
                    state, *location.module, decodedInstruction,
                    argumentWindow, locals, registers);
            }
            if (!executed) {
                if (state.pendingException) {
                    if (const auto* handler = routePendingException(bytecodeBlock.id)) {
                        block = handler;
                        exceptionTransferred = true;
                        break;
                    }
                } else {
                    refineInterpreterFrameSourceLocation(
                        state,
                        location,
                        bytecodeBlock.id,
                        static_cast<std::uint32_t>(instructionIndex),
                        static_cast<std::uint32_t>(executionSpan),
                        false);
                }
                if (execution->isAsyncFunction && state.pendingException.has_value()) {
                    const bool resolved = finishUncaughtInterpreterAsyncException(
                        state, function, activeCoroutine, result);
                    state.stack.pop_back();
                    return resolved;
                }
                state.stack.pop_back();
                return false;
            }
            if (state.coroutineSuspendedNow) {
                state.coroutineSuspendedNow = false;
                result = std::move(state.suspendedReturnValue);
                if (state.traceEventsEnabled) {
                    emitTrace(state, TraceEventKind::FunctionExit);
                }
                state.stack.pop_back();
                return true;
            }
            instructionIndex += executionSpan - 1;
        }
        if (exceptionTransferred) continue;
        if ((state.debugger && !debugSequencePoint(
                state, *location.module, function, bytecodeBlock.id,
                static_cast<std::uint32_t>(bytecodeBlock.instructions.size()), true)) ||
            !consume(
                state,
                block->terminatorOpId,
                block->terminatorOpName ? std::string_view(block->terminatorOpName) : std::string_view{})) {
            state.stack.pop_back();
            return false;
        }
        if (state.fastAccounting) {
            if (block->terminatorKind == bytecode::TerminatorKind::Branch &&
                block->conditionPrevalidatedBool) {
                const auto cond = std::get<bool>(registers[block->terminatorReg]);
                const detail::ExecutionEdge* fastEdge = cond ? &block->branch : &block->falseBranch;
                if (!fastEdge->hasArguments && fastEdge->target != detail::InvalidBlockIndex) {
                    ++state.statistics.branchesTaken;
                    block = &execution->blocks[fastEdge->target];
                    continue;
                }
            } else if (block->terminatorKind == bytecode::TerminatorKind::Jump) {
                const detail::ExecutionEdge* fastEdge = &block->branch;
                if (!fastEdge->hasArguments && fastEdge->target != detail::InvalidBlockIndex) {
                    ++state.statistics.branchesTaken;
                    block = &execution->blocks[fastEdge->target];
                    continue;
                }
            } else if (block->terminatorKind == bytecode::TerminatorKind::ReturnValue &&
                       block->returnPrevalidatedScalar) {
                result = registers[block->terminatorReg];
                state.stack.pop_back();
                return true;
            } else if (block->terminatorKind == bytecode::TerminatorKind::ReturnVoid) {
                result = std::monostate{};
                state.stack.pop_back();
                return true;
            }
        }
        const auto& terminator = bytecodeBlock.terminator;
        if (terminator.kind == bytecode::TerminatorKind::Throw) {
            if (terminator.value >= registers.size() ||
                !std::holds_alternative<ObjectRef>(registers[terminator.value])) {
                const bool failed = fail(state, ErrorCode::NullReference,
                    "throw requires a non-null script object");
                state.stack.pop_back();
                return failed;
            }
            if (state.debugger) {
                const auto excRef = std::get<ObjectRef>(registers[terminator.value]);
                semantic::SymbolId actualTypeId = 0;
                if (state.heap) {
                    if (const auto tid = state.heap->objectTypeId(excRef)) {
                        actualTypeId = *tid;
                    }
                }
                const auto* typeDesc = (actualTypeId != 0 && state.program)
                    ? state.program->resolveType(actualTypeId)
                    : nullptr;
                std::string excTypeName = typeDesc && !typeDesc->name.empty()
                    ? typeDesc->name
                    : "Exception";
                std::string excDescription = excTypeName;
                if (typeDesc && state.heap) {
                    for (std::size_t fieldIdx = 0; fieldIdx < typeDesc->fields.size(); ++fieldIdx) {
                        if (typeDesc->fields[fieldIdx].type == semantic::PrimitiveType::String) {
                            const auto fieldVal = state.heap->fieldGet(excRef, fieldIdx);
                            if (fieldVal && !std::holds_alternative<NullString>(*fieldVal) &&
                                !std::holds_alternative<std::monostate>(*fieldVal)) {
                                const auto msg = valueToString(*fieldVal, state.heap);
                                if (!msg.empty()) {
                                    excDescription = excTypeName + ": " + msg;
                                }
                                break;
                            }
                        }
                    }
                }
                bool isUncaught = true;
                if (actualTypeId != 0) {
                    for (auto frameIt = state.debugFrames.rbegin();
                         frameIt != state.debugFrames.rend();
                         ++frameIt) {
                        if (!frameIt->function) continue;
                        bool frameCatches = false;
                        for (const auto& handler : frameIt->function->exceptionHandlers) {
                            if (std::find(
                                    handler.protectedBlocks.begin(),
                                    handler.protectedBlocks.end(),
                                    frameIt->activeBlockId) ==
                                handler.protectedBlocks.end()) {
                                continue;
                            }
                            if (handler.catchTypeId != 0 &&
                                !runtimeAssignable(state, actualTypeId, handler.catchTypeId)) {
                                continue;
                            }
                            frameCatches = true;
                            break;
                        }
                        if (frameCatches) {
                            isUncaught = false;
                            break;
                        }
                    }
                }
                if (!notifyDebuggerException(
                        state,
                        ErrorCode::ScriptException,
                        std::move(excTypeName),
                        std::move(excDescription),
                        isUncaught,
                        false)) {
                    state.stack.pop_back();
                    return fail(state, ErrorCode::ExecutionTerminated,
                        "execution terminated by debugger");
                }
            }
            state.pendingException = registers[terminator.value];
            if (const auto* handler = routePendingException(bytecodeBlock.id)) {
                block = handler;
                continue;
            }
            if (execution->isAsyncFunction && state.pendingException.has_value()) {
                const bool resolved = finishUncaughtInterpreterAsyncException(
                    state, function, activeCoroutine, result);
                state.stack.pop_back();
                return resolved;
            }
            state.stack.pop_back();
            return false;
        }
        if (terminator.kind == bytecode::TerminatorKind::ReturnVoid) {
            result = std::monostate{};
            if (state.traceEventsEnabled) {
                if (state.diagnosticEventsEnabled) {
                    const auto operation = valueToString(result, state.heap);
                    emitTrace(state, TraceEventKind::FunctionExit, 0, operation);
                } else {
                    emitTrace(state, TraceEventKind::FunctionExit);
                }
            }
            state.stack.pop_back();
            return true;
        }
        if (terminator.kind == bytecode::TerminatorKind::ReturnValue) {
            if (terminator.value >= registers.size()) {
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram, "return register is invalid");
            }
            if (!expectSignatureType(
                    state,
                    registers[terminator.value],
                    function.returnType,
                    function.returnTypeId,
                    "return value")) {
                state.stack.pop_back();
                return false;
            }
            result = registers[terminator.value];
            if (state.traceEventsEnabled) {
                if (state.diagnosticEventsEnabled) {
                    const auto operation = valueToString(result, state.heap);
                    emitTrace(state, TraceEventKind::FunctionExit, 0, operation);
                } else {
                    emitTrace(state, TraceEventKind::FunctionExit);
                }
            }
            state.stack.pop_back();
            return true;
        }

        bytecode::BlockId target = terminator.target;
        const detail::ExecutionEdge* edge = &block->branch;
        if (terminator.kind == bytecode::TerminatorKind::Branch) {
            if (terminator.condition >= registers.size() ||
                !expectType(state, registers[terminator.condition],
                    semantic::PrimitiveType::Bool, "branch")) {
                state.stack.pop_back();
                return false;
            }
            if (!std::get<bool>(registers[terminator.condition])) {
                target = terminator.falseTarget;
                edge = &block->falseBranch;
            }
        } else if (terminator.kind != bytecode::TerminatorKind::Jump) {
            state.stack.pop_back();
            return fail(state, ErrorCode::InvalidProgram, "block has invalid terminator");
        }

        ++state.statistics.branchesTaken;
        if (state.traceEventsEnabled) {
            if (state.determinismOnly) {
                state.determinismEventDigest = accumulateDeterminismEvent(
                    state.determinismEventDigest,
                    TraceEventKind::Branch,
                    state.currentFunctionId,
                    target,
                    state.executed,
                    state.stack.size());
                ++state.determinismEventCount;
            } else if (state.diagnosticEventsEnabled) {
                const auto operation = "bb" + std::to_string(target);
                emitTrace(state, TraceEventKind::Branch, target, operation);
            } else {
                emitTrace(state, TraceEventKind::Branch, target);
            }
        }
        if (edge->target == detail::InvalidBlockIndex ||
            !edge->sources || !edge->destinations ||
            edge->sources->size() != edge->destinations->size()) {
            state.stack.pop_back();
            return fail(state, ErrorCode::InvalidProgram, "branch target or argument count is invalid");
        }
        const auto& sources = *edge->sources;
        const auto& destinations = *edge->destinations;
        if (sources.size() > edgeScratch.size()) {
            state.stack.pop_back();
            return fail(state, ErrorCode::InvalidProgram,
                "branch scratch space is too small");
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            if (sources[index] >= registers.size() ||
                destinations[index].target >= registers.size()) {
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram,
                    "branch argument register is invalid");
            }
            edgeScratch[index] = registers[sources[index]];
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            registers[destinations[index].target] = std::move(edgeScratch[index]);
        }
        block = &execution->blocks[edge->target];
    }
}

} // namespace realscript::runtime::interp_detail
