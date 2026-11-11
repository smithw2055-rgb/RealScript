#include "realscript/aot_cpp/AotRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace realscript::aot {
namespace {

runtime::DeterminismOperationId operationIdFromText(
    std::string_view operation) noexcept {
    return runtime::determinismOperationId(operation);
}

semantic::SymbolId typeIdAt(
    const semantic::SymbolId* ids,
    std::size_t count,
    std::size_t index) noexcept {
    return ids && index < count ? ids[index] : 0;
}

bool checkedIntResult(
    ExecutionContext& context,
    std::int64_t value,
    runtime::Value& output) {
    constexpr auto minimum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    constexpr auto maximum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    if (value < minimum || value > maximum) {
        return context.fail(
            runtime::ErrorCode::IntegerOverflow,
            "integer arithmetic overflow");
    }
    output = value;
    return true;
}

bool checkedLongAdd(
    ExecutionContext& context,
    std::int64_t left,
    std::int64_t right,
    runtime::Value& output) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return context.fail(
            runtime::ErrorCode::IntegerOverflow,
            "long arithmetic overflow");
    }
    output = runtime::LongValue{left + right};
    return true;
}

bool checkedLongSubtract(
    ExecutionContext& context,
    std::int64_t left,
    std::int64_t right,
    runtime::Value& output) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
        return context.fail(
            runtime::ErrorCode::IntegerOverflow,
            "long arithmetic overflow");
    }
    output = runtime::LongValue{left - right};
    return true;
}

bool requiresDescriptor(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Object ||
        type == semantic::PrimitiveType::Struct ||
        type == semantic::PrimitiveType::Enum;
}

bool requiresExactTypeId(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Object ||
        type == semantic::PrimitiveType::Struct ||
        type == semantic::PrimitiveType::Enum ||
        type == semantic::PrimitiveType::Array;
}

bool validSignatureIdentity(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept {
    return requiresExactTypeId(type) ? typeId != 0 : typeId == 0;
}

bool validStorageType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Bool ||
        semantic::isNumericType(type) ||
        type == semantic::PrimitiveType::String ||
        type == semantic::PrimitiveType::Object ||
        type == semantic::PrimitiveType::Struct ||
        type == semantic::PrimitiveType::Enum ||
        type == semantic::PrimitiveType::Array ||
        type == semantic::PrimitiveType::Handle;
}

bool validReturnType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Void || validStorageType(type);
}

bool checkedLongMultiply(
    ExecutionContext& context,
    std::int64_t left,
    std::int64_t right,
    runtime::Value& output) {
    if (left == 0 || right == 0) {
        output = runtime::LongValue{};
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
        return context.fail(
            runtime::ErrorCode::IntegerOverflow,
            "long arithmetic overflow");
    }
    output = runtime::LongValue{left * right};
    return true;
}

std::int64_t wrapInt32(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(value);
    return bits <= static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max())
        ? static_cast<std::int64_t>(bits)
        : static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32);
}

std::int64_t signedFromBits(std::uint64_t bits) noexcept {
    return bits <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(bits)
        : -1 - static_cast<std::int64_t>(
            std::numeric_limits<std::uint64_t>::max() - bits);
}

} // namespace

void registerAotProgramObservableTypes(
    runtime::ManagedHeap& heap,
    const ProgramDescriptor& program) {
    if (!program.types || program.typeCount == 0) return;
    for (std::uint32_t index = 0; index < program.typeCount; ++index) {
        const auto& type = program.types[index];
        if (type.kind != semantic::TypeKind::Class) {
            continue;
        }
        if (type.fieldCount == 1u && type.name != nullptr) {
            std::string_view typeName(type.name);
            if (const auto sep = typeName.rfind("::"); sep != std::string_view::npos) {
                typeName.remove_prefix(sep + 2u);
            }
            constexpr std::string_view kWrapperPrefix = "ObservableList__";
            constexpr std::string_view kEnumSuffix = "__Enumerator";
            if (typeName.rfind(kWrapperPrefix, 0) == 0 &&
                (typeName.size() <= kEnumSuffix.size() ||
                 typeName.substr(typeName.size() - kEnumSuffix.size()) != kEnumSuffix)) {
                heap.registerObservableListWrapperType(type.id);
            }
        }
        if (type.fieldCount == 0 || !type.fields) {
            continue;
        }
        bool hasAnyObservable = type.observable;
        for (std::uint32_t f = 0; f < type.fieldCount; ++f) {
            if (type.fields[f].observable || type.fields[f].observableBitIndex < 64u) {
                hasAnyObservable = true;
                break;
            }
        }
        if (!hasAnyObservable) continue;
        std::vector<std::uint8_t> bits(type.fieldCount, 0xFFu);
        for (std::uint32_t f = 0; f < type.fieldCount; ++f) {
            if (type.fields[f].observableBitIndex < 64u) {
                bits[f] = static_cast<std::uint8_t>(type.fields[f].observableBitIndex);
            }
        }
        heap.registerTypeObservableBits(type.id, std::move(bits));
    }
}

FrameScope::FrameScope(
    ExecutionContext& context,
    const std::vector<runtime::Value>& arguments,
    const std::vector<runtime::Value>& locals,
    const std::vector<runtime::Value>& registers)
    : context_(&context) {
    context_->shadowStack().pushFrame(&arguments, &locals, &registers);
}

FrameScope::FrameScope(
    ExecutionContext& context,
    runtime::ValueSpan arguments,
    runtime::ValueSpan locals,
    runtime::ValueSpan registers)
    : context_(&context) {
    context_->shadowStack().pushSpanFrame(arguments, locals, registers);
}

FrameScope::~FrameScope() {
    if (context_) context_->shadowStack().popFrame();
}

ExecutionContext::ExecutionContext(
    const ProgramDescriptor& program,
    std::shared_ptr<runtime::ManagedHeap> heap,
    std::shared_ptr<const runtime::BindingRegistry> bindings,
    runtime::ExecutionOptions options,
    const std::unordered_map<semantic::SymbolId, const TypeDescriptor*>* prebuiltTypes,
    const std::unordered_map<semantic::SymbolId, const FunctionDescriptor*>* prebuiltFunctions)
    : program_(&program),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      bindings_(std::move(bindings)),
      options_([&]() {
          if (options.enforceCapabilities) {
              options.determinism.enforceCapabilities = true;
              if (options.determinism.grantedCapabilities.empty()) {
                  options.determinism.grantedCapabilities = options.grantedCapabilities;
              } else {
                  options.determinism.grantedCapabilities.insert(
                      options.grantedCapabilities.begin(),
                      options.grantedCapabilities.end());
              }
          }
          return std::move(options);
      }()),
      determinism_(options_.determinism) {
    if (heap_) {
        registerAotProgramObservableTypes(*heap_, program);
    }
    traceEventsEnabled_ =
        options_.determinism.mode != runtime::DeterminismMode::Off ||
        options_.profile != nullptr || static_cast<bool>(options_.trace);
    determinismEventsEnabled_ =
        options_.determinism.mode != runtime::DeterminismMode::Off;
    diagnosticEventsEnabled_ =
        static_cast<bool>(options_.trace);
    determinismOnly_ = determinismEventsEnabled_ &&
        options_.profile == nullptr && !diagnosticEventsEnabled_;
    fastAccounting_ = !determinismEventsEnabled_ &&
        options_.profile == nullptr && !diagnosticEventsEnabled_ &&
        (options_.limits.gcWorkBudget == 0 || options_.trustedRelease);
    if (prebuiltTypes) {
        typesPtr_ = prebuiltTypes;
    } else {
        ownedTypes_.reserve(program.typeCount);
        for (std::uint32_t index = 0; index < program.typeCount; ++index) {
            const auto& type = program.types[index];
            ownedTypes_.emplace(type.id, &type);
        }
        typesPtr_ = &ownedTypes_;
    }
    stack_.reserve(64);
    stackSites_.reserve(64);
    if (prebuiltFunctions) {
        functionsPtr_ = prebuiltFunctions;
    } else {
        ownedFunctions_.reserve(program.functionCount);
        for (std::uint32_t index = 0; index < program.functionCount; ++index) {
            const auto& function = program.functions[index];
            ownedFunctions_.emplace(function.symbolId, &function);
        }
        functionsPtr_ = &ownedFunctions_;
    }
}

namespace {

std::optional<runtime::StackTraceLocation> lookupDescriptorSourceLocation(
    const ProgramDescriptor& descriptor,
    semantic::SymbolId symbolId,
    const char* functionName,
    std::uint32_t generatedLine) {
    if (symbolId == 0 && (!functionName || functionName[0] == '\0')) {
        return std::nullopt;
    }
    std::string resolvedName = functionName ? functionName : "";
    if (symbolId == 0 && descriptor.functions) {
        for (std::uint32_t i = 0; i < descriptor.functionCount; ++i) {
            if (descriptor.functions[i].name &&
                resolvedName == descriptor.functions[i].name) {
                symbolId = descriptor.functions[i].symbolId;
                break;
            }
        }
    } else if (resolvedName.empty() && descriptor.functions) {
        for (std::uint32_t i = 0; i < descriptor.functionCount; ++i) {
            if (descriptor.functions[i].symbolId == symbolId &&
                descriptor.functions[i].name) {
                resolvedName = descriptor.functions[i].name;
                break;
            }
        }
    }
    if (symbolId == 0) return std::nullopt;

    const SourceMapEntry* best = nullptr;
    if (descriptor.sourceMap && descriptor.sourceMapCount > 0) {
        for (std::uint32_t i = 0; i < descriptor.sourceMapCount; ++i) {
            const auto& entry = descriptor.sourceMap[i];
            if (entry.symbolId != symbolId) continue;
            if (!best) {
                best = &entry;
            } else if (generatedLine != 0 && entry.generatedLine <= generatedLine &&
                       entry.generatedLine >= best->generatedLine) {
                best = &entry;
            }
        }
    }

    runtime::StackTraceLocation loc;
    loc.symbolId = symbolId;
    loc.function = std::move(resolvedName);
    if (best) {
        loc.sourcePath = best->sourcePath ? best->sourcePath : "";
        loc.sourceLine = best->sourceLine;
        loc.sourceColumn = best->sourceColumn;
        loc.generatedLine = generatedLine != 0 ? generatedLine : best->generatedLine;
        return loc;
    }
    return loc;
}

} // namespace

std::vector<std::string> ExecutionContext::materializeStackTrace() const {
    std::vector<std::string> result;
    result.reserve(stack_.size());
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        result.emplace_back(*it ? *it : "<aot-function>");
    }
    return result;
}

std::vector<runtime::StackTraceLocation>
ExecutionContext::materializeSourceLocations() const {
    std::vector<runtime::StackTraceLocation> result;
    result.reserve(stack_.size());
    for (std::size_t idx = stack_.size(); idx > 0; --idx) {
        const std::size_t i = idx - 1;
        const char* fnName = stack_[i] ? stack_[i] : "<aot-function>";
        const semantic::SymbolId symId =
            i < stackSites_.size() ? stackSites_[i].symbolId : 0;
        const std::uint32_t genLine =
            i < stackSites_.size() ? stackSites_[i].generatedLine : 0;
        if (program_) {
            if (auto resolved = lookupDescriptorSourceLocation(
                    *program_, symId, fnName, genLine)) {
                result.push_back(std::move(*resolved));
                continue;
            }
        }
        runtime::StackTraceLocation fallback;
        fallback.symbolId = symId;
        fallback.function = fnName;
        fallback.generatedLine = genLine;
        result.push_back(std::move(fallback));
    }
    return result;
}

std::vector<std::string>
ExecutionContext::materializeSymbolizedStackTrace() const {
    const auto locations = materializeSourceLocations();
    std::vector<std::string> result;
    result.reserve(locations.size());
    for (const auto& loc : locations) {
        result.push_back(loc.format());
    }
    return result;
}

void ExecutionContext::populateErrorStackTrace(std::uint32_t activeGeneratedLine) {
    if (activeGeneratedLine != 0 && !stackSites_.empty()) {
        stackSites_.back().generatedLine = activeGeneratedLine;
    }
    error_.stackTrace = materializeStackTrace();
    error_.sourceLocations = materializeSourceLocations();
    error_.symbolizedStackTrace = materializeSymbolizedStackTrace();
}

const TypeDescriptor* ExecutionContext::findType(
    semantic::SymbolId typeId) const {
    if (!typesPtr_) return nullptr;
    const auto found = typesPtr_->find(typeId);
    return found == typesPtr_->end() ? nullptr : found->second;
}

bool ExecutionContext::isAssignable(
    semantic::SymbolId actual,
    semantic::SymbolId expected) const noexcept {
    if (actual == expected) return true;
    std::unordered_set<semantic::SymbolId> visited;
    auto current = actual;
    while (current != 0 && visited.insert(current).second) {
        const auto* type = findType(current);
        if (!type) return false;
        for (std::uint32_t index = 0;
             index < type->interfaceDispatchMapCount;
             ++index) {
            if (type->interfaceDispatchMaps &&
                type->interfaceDispatchMaps[index].interfaceTypeId == expected) {
                return true;
            }
        }
        if (type->baseTypeId == expected) return true;
        current = type->baseTypeId;
    }
    return false;
}

const FunctionDescriptor* ExecutionContext::findFunction(
    semantic::SymbolId symbolId) const {
    if (!functionsPtr_) return nullptr;
    const auto found = functionsPtr_->find(symbolId);
    return found == functionsPtr_->end() ? nullptr : found->second;
}

void ExecutionContext::emitTrace(
    runtime::TraceEventKind kind,
    runtime::DeterminismOperationId operationId,
    std::string_view operation) {
    if (!traceEventsEnabled_) return;
    if (determinismEventsEnabled_) {
        determinismEventDigest_ = runtime::accumulateDeterminismEvent(
            determinismEventDigest_,
            kind,
            currentFunctionId_,
            operationId,
            executed_,
            stack_.size());
        ++determinismEventCount_;
    }
    if (options_.profile) {
        ++profileEvents_;
        auto& profile = profileFunctions_[currentFunctionId_];
        if (profile.function.empty() && !stack_.empty()) {
            profile.function = stack_.back();
        }
        profile.maximumCallDepth = std::max(
            profile.maximumCallDepth, stack_.size());
        switch (kind) {
        case runtime::TraceEventKind::FunctionEnter: ++profile.calls; break;
        case runtime::TraceEventKind::FunctionExit: ++profile.returns; break;
        case runtime::TraceEventKind::Instruction: ++profile.instructions; break;
        case runtime::TraceEventKind::Branch: ++profile.branches; break;
        case runtime::TraceEventKind::ExternalCall: ++profile.externalCalls; break;
        case runtime::TraceEventKind::GcStep: ++profile.gcSteps; break;
        case runtime::TraceEventKind::RuntimeError: ++profile.runtimeErrors; break;
        }
    }
    if (!diagnosticEventsEnabled_) return;
    runtime::TraceEvent event;
    event.kind = kind;
    event.function = stack_.empty() ? std::string{} : std::string(stack_.back());
    event.operation.assign(operation.data(), operation.size());
    event.instructionIndex = executed_;
    event.callDepth = stack_.size();
    if (options_.trace) options_.trace(event);
}

bool ExecutionContext::fail(
    runtime::ErrorCode code,
    std::string message,
    std::uint32_t generatedLine) {
    error_.code = code;
    error_.message = std::move(message);
    populateErrorStackTrace(generatedLine);
    emitTrace(runtime::TraceEventKind::RuntimeError, 0, error_.message);
    return false;
}

bool ExecutionContext::consume(std::string_view operation) {
    return consume(operationIdFromText(operation), operation);
}

bool ExecutionContext::consume(
    runtime::DeterminismOperationId operationId,
    std::string_view operation) {
    if (executed_ >= options_.limits.instructionBudget) {
        return fail(
            runtime::ErrorCode::InstructionBudgetExceeded,
            "instruction budget exceeded");
    }
    ++executed_;
    statistics_.instructionsExecuted = executed_;
    if (traceEventsEnabled_) {
        if (determinismOnly_) {
            determinismEventDigest_ = runtime::accumulateDeterminismEvent(
                determinismEventDigest_,
                runtime::TraceEventKind::Instruction,
                currentFunctionId_,
                operationId,
                executed_,
                stack_.size());
            ++determinismEventCount_;
        } else {
            emitTrace(runtime::TraceEventKind::Instruction,
                operationId, operation);
        }
    }
    if (heap_ && options_.limits.gcWorkBudget != 0) {
        const auto work = heap_->step(
            shadowStack_,
            options_.limits.gcWorkBudget);
        statistics_.gcWorkPerformed += work;
        if (work != 0) {
            if (diagnosticEventsEnabled_) {
                const auto workText = std::to_string(work);
                emitTrace(runtime::TraceEventKind::GcStep, work, workText);
            } else {
                emitTrace(runtime::TraceEventKind::GcStep, work);
            }
        }
    }
    return true;
}

bool ExecutionContext::branch(std::uint32_t blockId) {
    ++statistics_.branchesTaken;
    if (traceEventsEnabled_) {
        if (determinismOnly_) {
            determinismEventDigest_ = runtime::accumulateDeterminismEvent(
                determinismEventDigest_,
                runtime::TraceEventKind::Branch,
                currentFunctionId_,
                blockId,
                executed_,
                stack_.size());
            ++determinismEventCount_;
        } else if (diagnosticEventsEnabled_) {
            const auto operation = "bb" + std::to_string(blockId);
            emitTrace(runtime::TraceEventKind::Branch, blockId, operation);
        } else {
            emitTrace(runtime::TraceEventKind::Branch, blockId);
        }
    }
    return true;
}

bool ExecutionContext::throwScript(const runtime::Value& value) {
    const auto* reference = std::get_if<runtime::ObjectRef>(&value);
    if (!reference || !heap_ || !heap_->isAlive(*reference)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "throw requires a non-null script object");
    }
    pendingException_ = value;
    error_.stackTrace = materializeStackTrace();
    error_.sourceLocations = materializeSourceLocations();
    error_.symbolizedStackTrace = materializeSymbolizedStackTrace();
    return false;
}

bool ExecutionContext::hasPendingException() const noexcept {
    return pendingException_.has_value();
}

bool ExecutionContext::pendingExceptionMatches(
    semantic::SymbolId typeId) const {
    if (!pendingException_) return false;
    if (typeId == 0) return true;
    const auto* reference = std::get_if<runtime::ObjectRef>(
        &*pendingException_);
    if (!reference) return false;
    const auto* targetHeap = resolveHeap(*reference);
    if (!targetHeap) return false;
    const auto actual = targetHeap->objectTypeId(*reference);
    return actual && isAssignable(*actual, typeId);
}

bool ExecutionContext::takePendingException(runtime::Value& value) {
    if (!pendingException_) return false;
    value = *pendingException_;
    pendingException_.reset();
    if (error_.code == runtime::ErrorCode::None) {
        error_.stackTrace.clear();
        error_.sourceLocations.clear();
        error_.symbolizedStackTrace.clear();
    }
    return true;
}

void ExecutionContext::reportUnhandledScriptException() {
    if (!pendingException_ || error_.code != runtime::ErrorCode::None) return;
    error_.code = runtime::ErrorCode::ScriptException;
    error_.message = "unhandled script exception";
    if (error_.stackTrace.empty()) {
        populateErrorStackTrace();
    }
}

runtime::ManagedHeap* ExecutionContext::resolveHeap(
    runtime::ObjectRef reference) const noexcept {
    if (domainContext_) {
        if (auto* h = domainContext_->heapForObject(reference)) {
            return h;
        }
    }
    return heap_.get();
}

runtime::Value ExecutionContext::sanitizeCrossHeapFieldValue(
    runtime::ManagedHeap* sourceHeap,
    runtime::Value value) {
    if (sourceHeap && heap_ && sourceHeap != heap_.get()) {
        if (const auto* strRef = std::get_if<runtime::ObjectRef>(&value)) {
            if (strRef->kind == runtime::ObjectKind::String) {
                if (const auto sv = sourceHeap->stringView(*strRef)) {
                    runtime::RuntimeError err;
                    if (const auto interned = heap_->internString(*sv, &err)) {
                        return runtime::Value{*interned};
                    }
                    return runtime::Value{runtime::StringValue(std::string(*sv))};
                }
            }
        }
    }
    return value;
}

bool ExecutionContext::expectType(
    const runtime::Value& value,
    semantic::PrimitiveType type,
    std::string_view context) {
    if (const auto* reference = std::get_if<runtime::ObjectRef>(&value)) {
        const auto* targetHeap = resolveHeap(*reference);
        if (!targetHeap || !targetHeap->isAlive(*reference)) {
            return fail(
                runtime::ErrorCode::InvalidObjectReference,
                std::string(context) +
                    " contains an invalid managed object reference");
        }
    }
    if (runtime::valueType(value) != type) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            std::string(context) + " expected '" +
                semantic::primitiveTypeName(type) + "', got '" +
                semantic::primitiveTypeName(runtime::valueType(value)) + "'");
    }
    return true;
}

bool ExecutionContext::expectSignatureType(
    const runtime::Value& value,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::string_view context) {
    if (type == semantic::PrimitiveType::Void ||
        type == semantic::PrimitiveType::Null) {
        if (std::holds_alternative<std::monostate>(value)) return true;
        return fail(
            runtime::ErrorCode::TypeMismatch,
            std::string(context) + " expected an empty value");
    }
    if (!expectType(value, type, context)) return false;
    if (type == semantic::PrimitiveType::Object) {
        if (std::holds_alternative<runtime::NullObject>(value) || typeId == 0) {
            return true;
        }
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto* targetHeap = reference ? resolveHeap(*reference) : nullptr;
        const auto actual = reference && targetHeap
            ? targetHeap->objectTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || !isAssignable(*actual, typeId)) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong runtime object type");
        }
    } else if (type == semantic::PrimitiveType::Array) {
        if (std::holds_alternative<runtime::NullArray>(value) || typeId == 0) {
            return true;
        }
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto* targetHeap = reference ? resolveHeap(*reference) : nullptr;
        const auto actual = reference && targetHeap
            ? targetHeap->arrayTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || *actual != typeId) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong runtime array type");
        }
    } else if (type == semantic::PrimitiveType::Handle && typeId != 0) {
        runtime::NativeHandle handle{};
        if (!value.tryGetNativeHandle(handle) ||
            (handle.valid() && handle.typeId != typeId) ||
            (!handle.valid() && handle.typeId != 0 && handle.typeId != typeId)) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong native handle type");
        }
    } else if (type == semantic::PrimitiveType::Struct) {
        const auto actualTypeId = runtime::structValueTypeId(value);
        const auto* structure = std::get_if<runtime::StructValue>(&value);
        if (!actualTypeId || typeId == 0 || *actualTypeId != typeId ||
            (structure && !structure->valid())) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong struct type");
        }
    } else if (type == semantic::PrimitiveType::Enum) {
        runtime::EnumValue enumeration{};
        if (!value.tryGetEnum(enumeration) || typeId == 0 || enumeration.typeId != typeId) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong enum type");
        }
    }
    return true;
}

runtime::Value ExecutionContext::defaultValue(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::size_t depth) const {
    switch (type) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return runtime::ByteValue{};
    case semantic::PrimitiveType::SByte: return runtime::SByteValue{};
    case semantic::PrimitiveType::Short: return runtime::ShortValue{};
    case semantic::PrimitiveType::UShort: return runtime::UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return runtime::UIntValue{};
    case semantic::PrimitiveType::Long: return runtime::LongValue{};
    case semantic::PrimitiveType::ULong: return runtime::ULongValue{};
    case semantic::PrimitiveType::Float: return runtime::FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return runtime::CharValue{};
    case semantic::PrimitiveType::String: return runtime::NullString{};
    case semantic::PrimitiveType::Object: return runtime::NullObject{};
    case semantic::PrimitiveType::Array: return runtime::NullArray{};
    case semantic::PrimitiveType::Handle: {
        runtime::NativeHandle handle;
        handle.typeId = typeId;
        return handle;
    }
    case semantic::PrimitiveType::Enum:
        return runtime::EnumValue{typeId, 0};
    case semantic::PrimitiveType::Struct: {
        if (const auto inlined = runtime::makeDefaultInlineStructValue(typeId)) {
            return *inlined;
        }
        if (depth > 64) return runtime::StructValue{};
        const auto* descriptor = findType(typeId);
        if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
            return runtime::StructValue{};
        }
        std::vector<runtime::Value> fields;
        fields.reserve(descriptor->fieldCount);
        for (std::uint32_t index = 0; index < descriptor->fieldCount; ++index) {
            const auto& field = descriptor->fields[index];
            fields.push_back(defaultValue(
                field.type,
                field.typeId,
                depth + 1));
        }
        return runtime::makeStructValueFromFields(typeId, std::move(fields));
    }
    case semantic::PrimitiveType::Null: return std::monostate{};
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return std::monostate{};
}

bool ExecutionContext::invoke(
    semantic::SymbolId symbolId,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    runtime::Value& result) {
    const auto* function = findFunction(symbolId);
    if (!function || !function->implementation) {
        return fail(
            runtime::ErrorCode::FunctionNotFound,
            "AOT entry function was not found");
    }
    if (stack_.size() >= options_.limits.recursionLimit) {
        return fail(
            runtime::ErrorCode::RecursionLimitExceeded,
            "recursion limit exceeded");
    }
    if (argumentCount != function->parameterCount) {
        return fail(
            runtime::ErrorCode::InvalidArguments,
            "argument count does not match function signature");
    }
    for (std::size_t index = 0; index < argumentCount; ++index) {
        if (!expectSignatureType(
                arguments[index],
                function->parameterTypes[index],
                typeIdAt(
                    function->parameterTypeIds,
                    function->parameterCount,
                    index),
                "argument")) {
            return false;
        }
    }

    if (function->simCommand &&
        domainContext_ != nullptr &&
        domainKind_ == runtime::DomainKind::UiPresentation) {
        runtime::SimCommandEnvelope envelope;
        envelope.commandTypeId = function->symbolId;
        std::size_t startIdx = 0;
        if (argumentCount > 0 && std::holds_alternative<runtime::ObjectRef>(arguments[0])) {
            const auto recvRef = std::get<runtime::ObjectRef>(arguments[0]);
            if (recvRef.kind == runtime::ObjectKind::Record) {
                envelope.targetObject = recvRef;
                startIdx = 1;
            }
        }
        envelope.payload.reserve(argumentCount - startIdx);
        for (std::size_t i = startIdx; i < argumentCount; ++i) {
            envelope.payload.push_back(arguments[i]);
        }
        domainContext_->enqueueSimCommand(std::move(envelope));
        result = defaultValue(function->returnType, function->returnTypeId);
        return true;
    }

    if (trustedReleaseEnabled()) {
        const auto previousFunctionId = currentFunctionId_;
        currentFunctionId_ = function->symbolId;
        stack_.push_back(function->name ? function->name : "<aot-function>");
        stackSites_.push_back({function->symbolId, 0});
        ++statistics_.functionCalls;
        if (statistics_.maximumCallDepth == 0) {
            statistics_.maximumCallDepth = 1;
        }
        const bool ok = function->implementation(
            *this,
            arguments,
            argumentCount,
            result);
        if (!stack_.empty()) stack_.pop_back();
        if (!stackSites_.empty()) stackSites_.pop_back();
        currentFunctionId_ = previousFunctionId;
        return ok;
    }

    const auto previousFunctionId = currentFunctionId_;
    currentFunctionId_ = function->symbolId;
    stack_.push_back(function->name ? function->name : "<aot-function>");
    stackSites_.push_back({function->symbolId, 0});
    ++statistics_.functionCalls;
    statistics_.maximumCallDepth = std::max(
        statistics_.maximumCallDepth,
        stack_.size());
    emitTrace(runtime::TraceEventKind::FunctionEnter);

    bool succeeded = false;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        succeeded = function->implementation(
            *this,
            arguments,
            argumentCount,
            result);
    } catch (const std::bad_alloc&) {
        succeeded = fail(runtime::ErrorCode::OutOfMemory,
            "AOT execution exhausted native memory");
    } catch (const std::exception& exception) {
        succeeded = fail(runtime::ErrorCode::InvalidProgram,
            std::string("AOT execution threw: ") + exception.what());
    } catch (...) {
        succeeded = fail(runtime::ErrorCode::InvalidProgram,
            "AOT execution threw an unknown exception");
    }
#else
    succeeded = function->implementation(
        *this,
        arguments,
        argumentCount,
        result);
#endif
    if (!succeeded) {
        if (!stack_.empty()) stack_.pop_back();
        if (!stackSites_.empty()) stackSites_.pop_back();
        currentFunctionId_ = previousFunctionId;
        return false;
    }
    if (!expectSignatureType(
            result,
            function->returnType,
            function->returnTypeId,
            "return value")) {
        if (!stack_.empty()) stack_.pop_back();
        if (!stackSites_.empty()) stackSites_.pop_back();
        currentFunctionId_ = previousFunctionId;
        return false;
    }
    if (traceEventsEnabled_) {
        if (diagnosticEventsEnabled_) {
            const auto operation = runtime::valueToString(result, heap_.get());
            emitTrace(runtime::TraceEventKind::FunctionExit, 0, operation);
        } else {
            emitTrace(runtime::TraceEventKind::FunctionExit);
        }
    }
    if (!stack_.empty()) stack_.pop_back();
    if (!stackSites_.empty()) stackSites_.pop_back();
    currentFunctionId_ = previousFunctionId;
    return true;
}

bool ExecutionContext::call(
    const CallSignature& signature,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    runtime::Value& result) {
    if (argumentCount != signature.parameterCount) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT call argument count does not match its signature");
    }
    for (std::size_t index = 0; index < argumentCount; ++index) {
        if (!expectSignatureType(
                arguments[index],
                signature.parameterTypes[index],
                typeIdAt(
                    signature.parameterTypeIds,
                    signature.parameterCount,
                    index),
                "call argument")) {
            return false;
        }
    }
    auto targetSymbolId = signature.symbolId;
    if (signature.virtualDispatch && signature.interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT call cannot combine virtual and interface dispatch");
    }
    if (signature.virtualDispatch) {
        if (signature.virtualSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            argumentCount == 0 ||
            !signature.parameterTypes ||
            signature.parameterTypes[0] !=
                semantic::PrimitiveType::Object) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT virtual call has an invalid receiver contract");
        }
        if (std::holds_alternative<runtime::NullObject>(arguments[0])) {
            return fail(
                runtime::ErrorCode::NullReference,
                "AOT virtual call attempted to dereference null");
        }
        const auto* receiver =
            std::get_if<runtime::ObjectRef>(&arguments[0]);
        const auto* targetHeap = receiver ? resolveHeap(*receiver) : nullptr;
        const auto actualTypeId = receiver && targetHeap
            ? targetHeap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        const auto* actualType = actualTypeId
            ? findType(*actualTypeId)
            : nullptr;
        if (!actualType ||
            signature.virtualSlot >= actualType->virtualSlotCount ||
            !actualType->virtualDispatchTable) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT virtual call slot is outside the runtime dispatch table");
        }
        targetSymbolId =
            actualType->virtualDispatchTable[signature.virtualSlot];
        if (targetSymbolId == 0) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT virtual call reached an abstract runtime slot");
        }
    } else if (signature.interfaceDispatch) {
        if (signature.interfaceTypeId == 0 ||
            signature.interfaceSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            argumentCount == 0 ||
            !signature.parameterTypes ||
            signature.parameterTypes[0] !=
                semantic::PrimitiveType::Object ||
            typeIdAt(
                signature.parameterTypeIds,
                signature.parameterCount,
                0) != signature.interfaceTypeId) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT interface call has an invalid receiver contract");
        }
        if (std::holds_alternative<runtime::NullObject>(arguments[0])) {
            return fail(
                runtime::ErrorCode::NullReference,
                "AOT interface call attempted to dereference null");
        }
        const auto* receiver =
            std::get_if<runtime::ObjectRef>(&arguments[0]);
        const auto* targetHeap = receiver ? resolveHeap(*receiver) : nullptr;
        const auto actualTypeId = receiver && targetHeap
            ? targetHeap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        const auto* actualType = actualTypeId
            ? findType(*actualTypeId)
            : nullptr;
        const InterfaceDispatchDescriptor* implementation = nullptr;
        if (actualType && actualType->interfaceDispatchMaps) {
            for (std::uint32_t index = 0;
                 index < actualType->interfaceDispatchMapCount;
                 ++index) {
                const auto& candidate =
                    actualType->interfaceDispatchMaps[index];
                if (candidate.interfaceTypeId ==
                    signature.interfaceTypeId) {
                    implementation = &candidate;
                    break;
                }
            }
        }
        if (!implementation || !implementation->slots ||
            signature.interfaceSlot >= implementation->slotCount) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT interface call slot is outside the runtime dispatch map");
        }
        targetSymbolId = implementation->slots[signature.interfaceSlot];
        if (targetSymbolId == 0) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT interface call reached an unimplemented runtime slot");
        }
    }
    if (findFunction(targetSymbolId)) {
        return invoke(targetSymbolId, arguments, argumentCount, result);
    }
    if (signature.virtualDispatch || signature.interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT runtime dispatch target function is unavailable");
    }
    if (signature.name &&
        std::string_view(signature.name).rfind("$intrinsic::", 0) == 0) {
        runtime::RuntimeError intrinsicError;
        if (!runtime::tryExecuteRuntimeIntrinsic(
                signature.name,
                runtime::ValueSpan(arguments, argumentCount),
                heap_.get(),
                result,
                intrinsicError)) {
            const auto code = intrinsicError.code != runtime::ErrorCode::None
                ? intrinsicError.code
                : (intrinsicError.message.find("null") != std::string::npos
                    ? runtime::ErrorCode::NullReference
                    : runtime::ErrorCode::InvalidProgram);
            return fail(code, intrinsicError.message);
        }
        return true;
    }
    if (!bindings_) {
        return fail(
            runtime::ErrorCode::ExternalFunctionUnresolved,
            std::string("external function '") +
                (signature.name ? signature.name : "<unknown>") +
                "' is unresolved");
    }
    bytecode::FunctionReference reference;
    reference.symbolId = signature.symbolId;
    reference.name = signature.name ? signature.name : "";
    reference.returnType = signature.returnType;
    reference.returnTypeId = signature.returnTypeId;
    reference.virtualDispatch = signature.virtualDispatch;
    reference.virtualSlot = signature.virtualSlot;
    reference.interfaceDispatch = signature.interfaceDispatch;
    reference.interfaceTypeId = signature.interfaceTypeId;
    reference.interfaceSlot = signature.interfaceSlot;
    if (signature.parameterCount != 0) {
        reference.parameterTypes.assign(
            signature.parameterTypes,
            signature.parameterTypes + signature.parameterCount);
    }
    if (signature.parameterTypeIds && signature.parameterCount != 0) {
        reference.parameterTypeIds.assign(
            signature.parameterTypeIds,
            signature.parameterTypeIds + signature.parameterCount);
    }
    runtime::RuntimeError externalError;
    ++statistics_.externalCalls;
    emitTrace(runtime::TraceEventKind::ExternalCall,
        reference.symbolId, reference.name);
    std::optional<runtime::Value> externalResult;
    if (!determinism_.invokeExternal(
            bindings_.get(),
            nullptr,
            reference,
            runtime::ValueSpan(arguments, argumentCount),
            externalResult,
            externalError)) {
        error_ = std::move(externalError);
        if (error_.code == runtime::ErrorCode::None) {
            error_.code = runtime::ErrorCode::ExternalFunctionUnresolved;
            error_.message = "external function '" + reference.name + "' failed";
        }
        populateErrorStackTrace();
        return false;
    }
    if (!expectSignatureType(
            *externalResult,
            signature.returnType,
            signature.returnTypeId,
            "external return value")) {
        return false;
    }
    result = std::move(*externalResult);
    return true;
}

bool ExecutionContext::newDelegate(
    semantic::SymbolId delegateTypeId,
    const CallSignature& signature,
    const runtime::Value* target,
    std::size_t targetCount,
    runtime::Value& result) {
    const auto* delegateType = findType(delegateTypeId);
    if (!delegateType || !delegateType->delegateType ||
        targetCount > 1 || (targetCount != 0 && !target)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate creation has invalid metadata");
    }
    runtime::Value targetValue = runtime::NullObject{};
    if (targetCount == 1) {
        if (signature.parameterCount == 0 ||
            !expectSignatureType(
                target[0], signature.parameterTypes[0],
                typeIdAt(
                    signature.parameterTypeIds,
                    signature.parameterCount, 0),
                "delegate target")) {
            return false;
        }
        targetValue = target[0];
    } else if (signature.virtualDispatch ||
               signature.interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch target has no receiver");
    }
    const auto word = [](std::uint64_t value, bool high) {
        return static_cast<std::int64_t>(
            static_cast<std::uint32_t>(high ? value >> 32u : value));
    };
    std::uint64_t flags = 0;
    if (signature.virtualDispatch) flags |= 1u;
    if (signature.interfaceDispatch) flags |= 2u;
    std::vector<runtime::Value> fields{
        std::move(targetValue),
        word(signature.symbolId, false),
        word(signature.symbolId, true),
        static_cast<std::int64_t>(flags),
        static_cast<std::int64_t>(signature.virtualDispatch
            ? signature.virtualSlot
            : signature.interfaceDispatch
                ? signature.interfaceSlot
                : 0u),
        word(signature.interfaceTypeId, false),
        word(signature.interfaceTypeId, true),
        runtime::NullObject{},
    };
    runtime::RuntimeError allocationError;
    const auto reference = heap_->allocateObject(
        delegateTypeId, std::move(fields), {0u, 7u},
        &allocationError);
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "AOT delegate allocation failed"
                : allocationError.message);
    }
    result = *reference;
    return true;
}

bool ExecutionContext::combineDelegates(
    semantic::SymbolId delegateTypeId,
    const runtime::Value& left,
    const runtime::Value& right,
    bool remove,
    runtime::Value& result) {
    const auto* delegateType = findType(delegateTypeId);
    if (!delegateType || !delegateType->delegateType ||
        !expectSignatureType(
            left, semantic::PrimitiveType::Object,
            delegateTypeId, "delegate combination left operand") ||
        !expectSignatureType(
            right, semantic::PrimitiveType::Object,
            delegateTypeId, "delegate combination right operand")) {
        return false;
    }
    const auto flatten = [&](const auto& self,
                             const runtime::Value& value,
                             std::vector<runtime::Value>& leaves,
                             std::size_t depth) -> bool {
        if (std::holds_alternative<runtime::NullObject>(value)) {
            return true;
        }
        if (depth > 1024) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation list is too deep");
        }
        const auto* reference =
            std::get_if<runtime::ObjectRef>(&value);
        const auto type = reference
            ? heap_->objectTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        const auto flagsValue = reference
            ? heap_->fieldGet(*reference, 3u)
            : std::optional<runtime::Value>{};
        const auto* flags = flagsValue
            ? std::get_if<std::int64_t>(&*flagsValue)
            : nullptr;
        const std::int64_t baseFlags = flags
            ? (*flags & ~runtime::ManagedHeap::DelegateWeakTargetFlag)
            : -1;
        const bool isWeakTarget = flags &&
            ((*flags & runtime::ManagedHeap::DelegateWeakTargetFlag) != 0);
        if (!reference || !type || *type != delegateTypeId ||
            heap_->fieldCount(*reference) != 8u || !flags ||
            baseFlags < 0 || baseFlags > 4) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation-list metadata is corrupt");
        }
        if (baseFlags != 4) {
            if (isWeakTarget) {
                const auto targetVal = heap_->fieldGet(*reference, 0u);
                if (targetVal) {
                    if (const auto* targetRef =
                            std::get_if<runtime::ObjectRef>(&*targetVal)) {
                        if (!heap_->isAlive(*targetRef)) {
                            return true;
                        }
                    }
                }
            }
            leaves.push_back(value);
            return true;
        }
        const auto first = heap_->fieldGet(*reference, 0u);
        const auto second = heap_->fieldGet(*reference, 7u);
        return first && second &&
            self(self, *first, leaves, depth + 1) &&
            self(self, *second, leaves, depth + 1);
    };
    std::vector<runtime::Value> leftLeaves;
    std::vector<runtime::Value> rightLeaves;
    if (!flatten(flatten, left, leftLeaves, 0) ||
        !flatten(flatten, right, rightLeaves, 0)) {
        return false;
    }
    const auto sameLeaf = [&](const runtime::Value& a,
                              const runtime::Value& b) {
        const auto* aRef = std::get_if<runtime::ObjectRef>(&a);
        const auto* bRef = std::get_if<runtime::ObjectRef>(&b);
        if (!aRef || !bRef) return false;
        for (std::size_t index = 0; index < 7u; ++index) {
            const auto av = heap_->fieldGet(*aRef, index);
            const auto bv = heap_->fieldGet(*bRef, index);
            if (!av || !bv || !(*av == *bv)) return false;
        }
        return true;
    };
    if (!remove) {
        leftLeaves.insert(
            leftLeaves.end(), rightLeaves.begin(), rightLeaves.end());
    } else if (!rightLeaves.empty() &&
               rightLeaves.size() <= leftLeaves.size()) {
        std::optional<std::size_t> removeAt;
        for (std::size_t start = 0;
             start + rightLeaves.size() <= leftLeaves.size(); ++start) {
            bool matches = true;
            for (std::size_t index = 0;
                 index < rightLeaves.size(); ++index) {
                if (!sameLeaf(
                        leftLeaves[start + index], rightLeaves[index])) {
                    matches = false;
                    break;
                }
            }
            if (matches) removeAt = start;
        }
        if (removeAt) {
            leftLeaves.erase(
                leftLeaves.begin() +
                    static_cast<std::ptrdiff_t>(*removeAt),
                leftLeaves.begin() +
                    static_cast<std::ptrdiff_t>(
                        *removeAt + rightLeaves.size()));
        }
    }
    if (leftLeaves.empty()) {
        result = runtime::NullObject{};
        return true;
    }
    result = leftLeaves.front();
    for (std::size_t index = 1; index < leftLeaves.size(); ++index) {
        std::vector<runtime::Value> fields{
            result,
            std::int64_t{0}, std::int64_t{0},
            std::int64_t{4}, std::int64_t{0},
            std::int64_t{0}, std::int64_t{0},
            leftLeaves[index],
        };
        runtime::RuntimeError allocationError;
        const auto allocated = heap_->allocateObject(
            delegateTypeId, std::move(fields), {0u, 7u},
            &allocationError);
        if (!allocated) {
            return fail(
                allocationError.code == runtime::ErrorCode::None
                    ? runtime::ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "AOT delegate-list allocation failed"
                    : allocationError.message);
        }
        result = *allocated;
    }
    return true;
}

bool ExecutionContext::invokeDelegate(
    semantic::SymbolId delegateTypeId,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    runtime::Value& result) {
    const auto* delegateType = findType(delegateTypeId);
    if (!delegateType || !delegateType->delegateType ||
        !arguments || argumentCount == 0) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate invocation has invalid metadata");
    }
    if (std::holds_alternative<runtime::NullObject>(arguments[0])) {
        return fail(
            runtime::ErrorCode::NullReference,
            "AOT delegate invocation attempted to dereference null");
    }
    const auto* delegateRef =
        std::get_if<runtime::ObjectRef>(&arguments[0]);
    const auto actualType = delegateRef
        ? heap_->objectTypeId(*delegateRef)
        : std::optional<semantic::SymbolId>{};
    if (!delegateRef || !actualType || *actualType != delegateTypeId ||
        heap_->fieldCount(*delegateRef) != 8u) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "AOT delegate invocation received an incompatible value");
    }
    const auto readIdFromRef = [&](runtime::ObjectRef ref,
                                   std::size_t lowIndex,
                                   semantic::SymbolId& value) {
        const auto low = heap_->fieldGet(ref, lowIndex);
        const auto high = heap_->fieldGet(ref, lowIndex + 1u);
        const auto* lowValue = low
            ? std::get_if<std::int64_t>(&*low)
            : nullptr;
        const auto* highValue = high
            ? std::get_if<std::int64_t>(&*high)
            : nullptr;
        if (!lowValue || !highValue || *lowValue < 0 ||
            *highValue < 0 ||
            *lowValue > std::numeric_limits<std::uint32_t>::max() ||
            *highValue > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        value = static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(*lowValue)) |
            (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(*highValue)) << 32u);
        return true;
    };
    const auto readId = [&](std::size_t lowIndex,
                            semantic::SymbolId& value) {
        return readIdFromRef(*delegateRef, lowIndex, value);
    };
    const auto rootFlagsValue = heap_->fieldGet(*delegateRef, 3u);
    const auto* rootFlags = rootFlagsValue
        ? std::get_if<std::int64_t>(&*rootFlagsValue)
        : nullptr;
    if (rootFlags &&
        (*rootFlags & ~runtime::ManagedHeap::DelegateWeakTargetFlag) == 4) {
        std::vector<runtime::Value> leaves;
        std::optional<runtime::ObjectRef> anyLeafRef;
        const auto flatten = [&](const auto& self,
                                 const runtime::Value& value,
                                 std::size_t depth) -> bool {
            if (depth > 1024) return false;
            const auto* reference =
                std::get_if<runtime::ObjectRef>(&value);
            const auto flagsValue = reference
                ? heap_->fieldGet(*reference, 3u)
                : std::optional<runtime::Value>{};
            const auto* flags = flagsValue
                ? std::get_if<std::int64_t>(&*flagsValue)
                : nullptr;
            if (!reference || !flags) return false;
            const std::int64_t baseFlags =
                *flags & ~runtime::ManagedHeap::DelegateWeakTargetFlag;
            const bool isWeakTarget =
                (*flags & runtime::ManagedHeap::DelegateWeakTargetFlag) != 0;
            if (baseFlags < 0 || baseFlags > 4) return false;
            if (baseFlags != 4) {
                if (!anyLeafRef) {
                    anyLeafRef = *reference;
                }
                if (isWeakTarget) {
                    const auto targetVal = heap_->fieldGet(*reference, 0u);
                    if (targetVal) {
                        if (const auto* targetRef =
                                std::get_if<runtime::ObjectRef>(&*targetVal)) {
                            if (!heap_->isAlive(*targetRef)) {
                                return true;
                            }
                        }
                    }
                }
                leaves.push_back(value);
                return true;
            }
            const auto first = heap_->fieldGet(*reference, 0u);
            const auto second = heap_->fieldGet(*reference, 7u);
            return first && second &&
                self(self, *first, depth + 1) &&
                self(self, *second, depth + 1);
        };
        if (!flatten(flatten, arguments[0], 0)) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation list is corrupt");
        }
        std::vector<runtime::Value> nested(
            arguments, arguments + argumentCount);
        bool hasResult = false;
        for (const auto& leaf : leaves) {
            if (const auto* leafRef = std::get_if<runtime::ObjectRef>(&leaf)) {
                const auto leafFlagsVal = heap_->fieldGet(*leafRef, 3u);
                const auto* leafFlags = leafFlagsVal
                    ? std::get_if<std::int64_t>(&*leafFlagsVal)
                    : nullptr;
                if (leafFlags &&
                    (*leafFlags & runtime::ManagedHeap::DelegateWeakTargetFlag) != 0) {
                    const auto leafTarget = heap_->fieldGet(*leafRef, 0u);
                    if (leafTarget) {
                        if (const auto* targetRef =
                                std::get_if<runtime::ObjectRef>(&*leafTarget)) {
                            if (!heap_->isAlive(*targetRef)) {
                                continue;
                            }
                        }
                    }
                }
            }
            nested[0] = leaf;
            if (!consume("invoke.delegate")) return false;
            runtime::Value leafResult;
            if (!invokeDelegate(
                    delegateTypeId, nested.data(), nested.size(), leafResult)) {
                return false;
            }
            result = std::move(leafResult);
            hasResult = true;
        }
        if (!hasResult) {
            semantic::SymbolId fallbackFnId = 0;
            if (anyLeafRef && readIdFromRef(*anyLeafRef, 1u, fallbackFnId)) {
                if (const auto* fn = findFunction(fallbackFnId)) {
                    result = defaultValue(fn->returnType, fn->returnTypeId);
                    return true;
                }
            }
            result = runtime::Value{};
        }
        return true;
    }
    semantic::SymbolId targetSymbolId = 0;
    if (!readId(1u, targetSymbolId)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate callable identity is corrupt");
    }
    const auto target = heap_->fieldGet(*delegateRef, 0u);
    const auto flagsValue = heap_->fieldGet(*delegateRef, 3u);
    const auto slotValue = heap_->fieldGet(*delegateRef, 4u);
    const auto* flags = flagsValue
        ? std::get_if<std::int64_t>(&*flagsValue)
        : nullptr;
    const auto* slot = slotValue
        ? std::get_if<std::int64_t>(&*slotValue)
        : nullptr;
    const std::int64_t dispatchFlags = flags
        ? (*flags & ~runtime::ManagedHeap::DelegateWeakTargetFlag)
        : -1;
    const bool isWeakTarget = flags &&
        ((*flags & runtime::ManagedHeap::DelegateWeakTargetFlag) != 0);
    if (!target || !flags || !slot || dispatchFlags < 0 || dispatchFlags > 3 ||
        *slot < 0 ||
        *slot > std::numeric_limits<std::uint32_t>::max()) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch metadata is corrupt");
    }
    if (isWeakTarget) {
        if (const auto* targetRef = std::get_if<runtime::ObjectRef>(&*target)) {
            if (!heap_->isAlive(*targetRef)) {
                const auto* fn = findFunction(targetSymbolId);
                result = fn
                    ? defaultValue(fn->returnType, fn->returnTypeId)
                    : runtime::Value{};
                return true;
            }
        }
    }
    std::vector<runtime::Value> values;
    if (argumentCount > 1) {
        values.assign(arguments + 1, arguments + argumentCount);
    }
    if (!std::holds_alternative<runtime::NullObject>(*target)) {
        values.insert(values.begin(), *target);
    }
    const bool virtualDispatch = (dispatchFlags & 1) != 0;
    const bool interfaceDispatch = (dispatchFlags & 2) != 0;
    if (virtualDispatch && interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch modes conflict");
    }
    if (virtualDispatch || interfaceDispatch) {
        const auto* receiver = values.empty()
            ? nullptr
            : std::get_if<runtime::ObjectRef>(&values.front());
        const auto* targetHeap = receiver ? resolveHeap(*receiver) : nullptr;
        const auto runtimeTypeId = receiver && targetHeap
            ? targetHeap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        const auto* runtimeType = runtimeTypeId
            ? findType(*runtimeTypeId)
            : nullptr;
        if (!runtimeType) {
            return fail(
                runtime::ErrorCode::InvalidObjectReference,
                "AOT delegate receiver has no runtime type");
        }
        const auto dispatchSlot = static_cast<std::uint32_t>(*slot);
        if (virtualDispatch) {
            if (!runtimeType->virtualDispatchTable ||
                dispatchSlot >= runtimeType->virtualSlotCount) {
                return fail(
                    runtime::ErrorCode::InvalidProgram,
                    "AOT delegate virtual slot is invalid");
            }
            targetSymbolId =
                runtimeType->virtualDispatchTable[dispatchSlot];
        } else {
            semantic::SymbolId interfaceTypeId = 0;
            if (!readId(5u, interfaceTypeId)) {
                return fail(
                    runtime::ErrorCode::InvalidProgram,
                    "AOT delegate interface identity is corrupt");
            }
            const InterfaceDispatchDescriptor* map = nullptr;
            if (runtimeType->interfaceDispatchMaps) {
                for (std::uint32_t index = 0;
                     index < runtimeType->interfaceDispatchMapCount;
                     ++index) {
                    const auto& candidate =
                        runtimeType->interfaceDispatchMaps[index];
                    if (candidate.interfaceTypeId == interfaceTypeId) {
                        map = &candidate;
                        break;
                    }
                }
            }
            if (!map || !map->slots || dispatchSlot >= map->slotCount) {
                return fail(
                    runtime::ErrorCode::InvalidProgram,
                    "AOT delegate interface slot is invalid");
            }
            targetSymbolId = map->slots[dispatchSlot];
        }
    }
    if (!findFunction(targetSymbolId)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate target function is unavailable");
    }
    return invoke(
        targetSymbolId,
        values.empty() ? nullptr : values.data(),
        values.size(),
        result);
}

bool ExecutionContext::constantString(
    std::string_view value,
    runtime::Value& result) {
    runtime::RuntimeError allocationError;
    const auto reference = heap_->internString(value, &allocationError);
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "managed string allocation failed"
                : allocationError.message);
    }
    result = *reference;
    return true;
}

bool ExecutionContext::newObject(
    semantic::SymbolId typeId,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Class ||
        type->interfaceType || type->delegateType) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT object allocation references an invalid class descriptor");
    }
    std::vector<runtime::Value> fields;
    std::vector<std::size_t> references;
    fields.reserve(type->fieldCount);
    for (std::uint32_t index = 0; index < type->fieldCount; ++index) {
        const auto& field = type->fields[index];
        fields.push_back(defaultValue(field.type, field.typeId));
        if (semantic::isReferenceType(field.type) ||
            (field.type == semantic::PrimitiveType::Struct &&
             !runtime::makeDefaultInlineStructValue(field.typeId).has_value())) {
            references.push_back(index);
        }
    }
    runtime::RuntimeError allocationError;
    const auto reference = heap_->allocateObject(
        typeId,
        std::move(fields),
        std::move(references),
        &allocationError);
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "managed object allocation failed"
                : allocationError.message);
    }
    result = *reference;
    return true;
}

bool ExecutionContext::newStruct(
    semantic::SymbolId typeId,
    runtime::Value& result) {
    result = defaultValue(semantic::PrimitiveType::Struct, typeId);
    if (runtime::makeDefaultInlineStructValue(typeId).has_value()) {
        return true;
    }
    const auto* structure = std::get_if<runtime::StructValue>(&result);
    if (!structure || !structure->valid()) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT struct allocation references an invalid struct descriptor");
    }
    return true;
}

bool ExecutionContext::newArray(
    semantic::SymbolId arrayTypeId,
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    const runtime::Value& length,
    runtime::Value& result) {
    if (!expectType(length, semantic::PrimitiveType::Int, "array length")) {
        return false;
    }
    const auto size = std::get<std::int64_t>(length);
    if (size < 0) {
        return fail(
            runtime::ErrorCode::IndexOutOfRange,
            "array length cannot be negative");
    }
    if (static_cast<std::uint64_t>(size) >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return fail(runtime::ErrorCode::OutOfMemory, "array length is too large");
    }
    runtime::RuntimeError allocationError;
    const auto reference = heap_->allocateTypedArray(
        arrayTypeId,
        elementType,
        elementTypeId,
        static_cast<std::size_t>(size),
        defaultValue(elementType, elementTypeId),
        &allocationError);
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "managed array allocation failed"
                : allocationError.message);
    }
    result = *reference;
    return true;
}

bool ExecutionContext::checkNotNull(
    semantic::SymbolId typeId,
    const runtime::Value& receiver,
    runtime::Value& result) {
    if (std::holds_alternative<runtime::NullObject>(receiver)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "field access attempted to dereference null");
    }
    if (!expectSignatureType(
            receiver,
            semantic::PrimitiveType::Object,
            typeId,
            "field access")) {
        return false;
    }
    result = receiver;
    return true;
}

bool ExecutionContext::typeOperation(
    semantic::PrimitiveType sourceType,
    semantic::PrimitiveType targetType,
    semantic::SymbolId targetTypeId,
    const runtime::Value& value,
    bool safeCast,
    runtime::Value& result) {
    const bool nullValue =
        std::holds_alternative<std::monostate>(value) ||
        std::holds_alternative<runtime::NullString>(value) ||
        std::holds_alternative<runtime::NullObject>(value) ||
        std::holds_alternative<runtime::NullArray>(value);
    bool matches = false;
    if (!nullValue) {
        if (targetType == semantic::PrimitiveType::Object) {
            const auto* reference = std::get_if<runtime::ObjectRef>(&value);
            const auto* targetHeap = reference ? resolveHeap(*reference) : nullptr;
            const auto actual = reference && targetHeap
                ? targetHeap->objectTypeId(*reference)
                : std::optional<semantic::SymbolId>{};
            matches = actual && isAssignable(*actual, targetTypeId);
        } else if (targetType == semantic::PrimitiveType::Array) {
            const auto* reference = std::get_if<runtime::ObjectRef>(&value);
            const auto* targetHeap = reference ? resolveHeap(*reference) : nullptr;
            const auto actual = reference && targetHeap
                ? targetHeap->arrayTypeId(*reference)
                : std::optional<semantic::SymbolId>{};
            matches = actual && *actual == targetTypeId;
        } else {
            matches = sourceType == targetType;
        }
    }
    if (!safeCast) {
        result = matches;
        return true;
    }
    if (matches) {
        result = value;
    } else if (targetType == semantic::PrimitiveType::String) {
        result = runtime::NullString{};
    } else if (targetType == semantic::PrimitiveType::Array) {
        result = runtime::NullArray{};
    } else {
        result = runtime::NullObject{};
    }
    return true;
}

bool ExecutionContext::arrayLength(
    const runtime::Value& receiver,
    runtime::Value& result) {
    std::int64_t len = 0;
    if (!arrayLengthFast(receiver, len)) return false;
    result = len;
    return true;
}

bool ExecutionContext::arrayLengthFast(
    const runtime::Value& receiver,
    std::int64_t& result) {
    if (const auto* reference = std::get_if<runtime::ObjectRef>(&receiver)) {
        if (const auto* targetHeap = resolveHeap(*reference)) {
            if (const auto length = targetHeap->arrayLength(*reference)) {
                if (*length <= static_cast<std::size_t>(INT32_MAX)) {
                    result = static_cast<std::int64_t>(*length);
                    return true;
                }
            }
        }
        return fail(
            runtime::ErrorCode::InvalidObjectReference,
            "array length references an invalid managed array");
    }
    if (std::holds_alternative<runtime::NullArray>(receiver)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "array length attempted to dereference null");
    }
    return fail(
        runtime::ErrorCode::TypeMismatch,
        "array length expected 'array'");
}

bool ExecutionContext::loadElement(
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    const runtime::Value& receiver,
    const runtime::Value& index,
    runtime::Value& result) {
    if (std::holds_alternative<runtime::NullArray>(receiver)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "array element load attempted to dereference null");
    }
    if (!expectType(receiver, semantic::PrimitiveType::Array, "array element load") ||
        !expectType(index, semantic::PrimitiveType::Int, "array index")) {
        return false;
    }
    const auto numericIndex = std::get<std::int64_t>(index);
    const auto reference = std::get<runtime::ObjectRef>(receiver);
    auto* targetHeap = resolveHeap(reference);
    const auto length = targetHeap ? targetHeap->arrayLength(reference) : std::nullopt;
    if (!length) {
        return fail(
            runtime::ErrorCode::InvalidObjectReference,
            "array element load references an invalid managed array");
    }
    if (numericIndex < 0 ||
        static_cast<std::uint64_t>(numericIndex) >= *length) {
        return fail(
            runtime::ErrorCode::IndexOutOfRange,
            "array element index is out of range");
    }
    const auto actualType = targetHeap->arrayElementType(reference);
    const auto actualTypeId = targetHeap->arrayElementTypeId(reference);
    if (!actualType || !actualTypeId ||
        *actualType != elementType || *actualTypeId != elementTypeId) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "array element metadata does not match the AOT operation");
    }
    auto value = targetHeap->arrayGet(
        reference,
        static_cast<std::size_t>(numericIndex));
    if (value) {
        *value = sanitizeCrossHeapFieldValue(targetHeap, std::move(*value));
    }
    if (!value || !expectSignatureType(
            *value,
            elementType,
            elementTypeId,
            "array element load value")) {
        return false;
    }
    result = *value;
    return true;
}

bool ExecutionContext::storeElement(
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    const runtime::Value& receiver,
    const runtime::Value& index,
    const runtime::Value& value) {
    runtime::Value ignored;
    if (std::holds_alternative<runtime::NullArray>(receiver)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "array element store attempted to dereference null");
    }
    if (!expectType(receiver, semantic::PrimitiveType::Array, "array element store") ||
        !expectType(index, semantic::PrimitiveType::Int, "array index") ||
        !expectSignatureType(value, elementType, elementTypeId,
            "array element store value")) {
        return false;
    }
    const auto numericIndex = std::get<std::int64_t>(index);
    const auto reference = std::get<runtime::ObjectRef>(receiver);
    if (domainKind_ == runtime::DomainKind::UiPresentation &&
        heap_ &&
        reference.heapId != static_cast<std::uint32_t>(heap_->heapId())) {
        return fail(
            runtime::ErrorCode::CapabilityDenied,
            "CrossHeapWriteViolation: UiPresentation domain cannot mutate SimHeap array");
    }
    const auto length = heap_->arrayLength(reference);
    if (!length) {
        return fail(
            runtime::ErrorCode::InvalidObjectReference,
            "array element store references an invalid managed array");
    }
    if (numericIndex < 0 ||
        static_cast<std::uint64_t>(numericIndex) >= *length) {
        return fail(
            runtime::ErrorCode::IndexOutOfRange,
            "array element index is out of range");
    }
    const auto actualType = heap_->arrayElementType(reference);
    const auto actualTypeId = heap_->arrayElementTypeId(reference);
    if (!actualType || !actualTypeId ||
        *actualType != elementType || *actualTypeId != elementTypeId) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "array element metadata does not match the AOT operation");
    }
    const auto storePolicy = elementType == semantic::PrimitiveType::Object
        ? runtime::ArrayStoreTypePolicy::PrevalidatedAssignableObject
        : runtime::ArrayStoreTypePolicy::Exact;
    if (!heap_->arraySet(
            reference,
            static_cast<std::size_t>(numericIndex),
            value,
            nullptr,
            storePolicy)) {
        runtime::RuntimeError arrayError;
        (void)heap_->arraySet(
            reference,
            static_cast<std::size_t>(numericIndex),
            value,
            &arrayError,
            storePolicy);
        return fail(
            arrayError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::InvalidProgram
                : arrayError.code,
            arrayError.message.empty()
                ? "array element store failed"
                : arrayError.message);
    }
    return true;
}

bool ExecutionContext::loadField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const runtime::Value& receiver,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Class ||
        fieldIndex >= type->fieldCount) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT field load references invalid type metadata");
    }
    runtime::Value checked;
    if (!checkNotNull(typeId, receiver, checked)) return false;
    const auto reference = std::get<runtime::ObjectRef>(checked);
    auto* targetHeap = resolveHeap(reference);
    auto value = targetHeap ? targetHeap->fieldGet(reference, fieldIndex) : std::nullopt;
    if (!value) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "field load references an invalid field slot");
    }
    *value = sanitizeCrossHeapFieldValue(targetHeap, std::move(*value));
    const auto& field = type->fields[fieldIndex];
    if (!expectSignatureType(
            *value,
            field.type,
            field.typeId,
            "field load value")) {
        return false;
    }
    result = *value;
    return true;
}

bool ExecutionContext::storeField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const runtime::Value& receiver,
    const runtime::Value& value) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Class ||
        fieldIndex >= type->fieldCount) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT field store references invalid type metadata");
    }
    runtime::Value checked;
    if (!checkNotNull(typeId, receiver, checked)) return false;
    const auto reference = std::get<runtime::ObjectRef>(checked);
    const auto& field = type->fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "field store value")) {
        return false;
    }
    if (domainKind_ == runtime::DomainKind::UiPresentation &&
        ((heap_ && reference.heapId != static_cast<std::uint32_t>(heap_->heapId())) ||
         type->simState || field.simState)) {
        return fail(
            runtime::ErrorCode::CapabilityDenied,
            std::string("CrossHeapWriteViolation: UiPresentation domain cannot mutate [SimState] or SimHeap field '") +
                (field.name ? field.name : "") + "'");
    }
    if (!heap_->fieldSet(
            reference,
            fieldIndex,
            value,
            nullptr)) {
        runtime::RuntimeError fieldError;
        (void)heap_->fieldSet(reference, fieldIndex, value, &fieldError);
        return fail(
            fieldError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::InvalidProgram
                : fieldError.code,
            fieldError.message.empty()
                ? "field store failed"
                : fieldError.message);
    }
    return true;
}

bool ExecutionContext::loadStructField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const runtime::Value& receiver,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Struct ||
        fieldIndex >= type->fieldCount ||
        !expectSignatureType(
            receiver,
            semantic::PrimitiveType::Struct,
            typeId,
            "struct field load")) {
        return false;
    }
    const auto& field = type->fields[fieldIndex];
    if (const auto inlinedField = runtime::loadInlineStructField(receiver, fieldIndex)) {
        if (!expectSignatureType(
                *inlinedField,
                field.type,
                field.typeId,
                "struct field load value")) {
            return false;
        }
        result = *inlinedField;
        return true;
    }
    const auto& structure = std::get<runtime::StructValue>(receiver);
    if (!structure.storage || fieldIndex >= structure.storage->fields.size()) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "struct field load references an invalid field slot");
    }
    const auto& value = structure.storage->fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "struct field load value")) {
        return false;
    }
    result = value;
    return true;
}

bool ExecutionContext::storeStructField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const runtime::Value& receiver,
    const runtime::Value& value,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Struct ||
        fieldIndex >= type->fieldCount ||
        !expectSignatureType(
            receiver,
            semantic::PrimitiveType::Struct,
            typeId,
            "struct field store")) {
        return false;
    }
    const auto& field = type->fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "struct field store value")) {
        return false;
    }
    if (const auto updatedInline = runtime::storeInlineStructField(receiver, fieldIndex, value)) {
        result = *updatedInline;
        return true;
    }
    const auto& structure = std::get<runtime::StructValue>(receiver);
    if (!structure.storage || fieldIndex >= structure.storage->fields.size()) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "struct field store references an invalid field slot");
    }
    if (&result == &receiver && structure.storage.use_count() == 1) {
        const_cast<runtime::StructStorage&>(*structure.storage).fields[fieldIndex] = value;
        return true;
    }
    auto storage = std::make_shared<runtime::StructStorage>(*structure.storage);
    storage->fields[fieldIndex] = value;
    result = runtime::StructValue{typeId, std::move(storage)};
    return true;
}

bool ExecutionContext::initUnboxedStruct(
    semantic::SymbolId typeId,
    UnboxedCustomStruct& result) {
    const auto* descriptor = findType(typeId);
    if (!descriptor || descriptor->kind != semantic::TypeKind::Struct ||
        descriptor->fieldCount > 16) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT unboxed struct allocation references an invalid struct descriptor");
    }
    result.typeId = typeId;
    result.fieldCount = static_cast<std::uint8_t>(descriptor->fieldCount);
    for (std::uint32_t index = 0; index < descriptor->fieldCount; ++index) {
        const auto& field = descriptor->fields[index];
        result.fields[index] = defaultValue(field.type, field.typeId, 1);
    }
    return true;
}

bool ExecutionContext::loadUnboxedStructField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const UnboxedCustomStruct& receiver,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Struct ||
        receiver.typeId != typeId ||
        fieldIndex >= type->fieldCount ||
        fieldIndex >= receiver.fieldCount) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "struct field load references an invalid field slot");
    }
    const auto& field = type->fields[fieldIndex];
    const auto& value = receiver.fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "struct field load value")) {
        return false;
    }
    result = value;
    return true;
}

bool ExecutionContext::storeUnboxedStructField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const UnboxedCustomStruct& receiver,
    const runtime::Value& value,
    UnboxedCustomStruct& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Struct ||
        receiver.typeId != typeId ||
        fieldIndex >= type->fieldCount ||
        fieldIndex >= receiver.fieldCount) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "struct field store references an invalid field slot");
    }
    const auto& field = type->fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "struct field store value")) {
        return false;
    }
    if (&result != &receiver) {
        result = receiver;
    }
    result.fields[fieldIndex] = value;
    return true;
}

bool ExecutionContext::ecsWritebackBoxed(
    const CallSignature& signature,
    std::int64_t index,
    const runtime::Value& boxedStruct) {
    if (!heap_ || !signature.name) return true;
    if (!heap_->hasSparseEcsState()) return true;
    const std::array<runtime::Value, 2> args{{
        runtime::Value{index},
        boxedStruct,
    }};
    runtime::Value ignored;
    return call(signature, args.data(), args.size(), ignored);
}

bool ExecutionContext::resolveEcsColumnFast(
    semantic::SymbolId denseColSymbolId,
    const CallSignature& fallbackSignature,
    runtime::Value& outResult) {
    if (!hasSparseEcsState() && denseColSymbolId != 0 && heap_) {
        if (auto existing = heap_->getStaticField(denseColSymbolId)) {
            outResult = std::move(*existing);
            return true;
        }
    }
    return call(fallbackSignature, nullptr, 0, outResult);
}

TypedRawColumnCache ExecutionContext::acquireRawColumnCache(
    const runtime::Value& arrayValue,
    semantic::SymbolId expectedStructTypeId,
    std::size_t generatedStructSize,
    const GeneratedFieldLayout* generatedFields,
    std::size_t generatedFieldCount) const noexcept {
    TypedRawColumnCache cache{};
    const auto* ref = std::get_if<runtime::ObjectRef>(&arrayValue);
    if (!ref) return cache;
    const auto* targetHeap = resolveHeap(*ref);
    if (!targetHeap) return cache;
    const auto* view = targetHeap->rawColumnView(*ref);
    if (!view) return cache;
    if (expectedStructTypeId != 0 && view->structTypeId != expectedStructTypeId) {
        return cache;
    }
    if (view->fields.size() > cache.fields.size()) {
        return cache;
    }
    if (generatedFields && generatedFieldCount != view->fields.size()) {
        return cache;
    }
    cache.basePtr = static_cast<std::uint8_t*>(view->basePtr);
    cache.strideBytes = view->strideBytes;
    cache.count = static_cast<std::int64_t>(view->count);
    cache.readOnly = view->readOnly ||
        (domainKind_ == runtime::DomainKind::UiPresentation &&
         heap_ &&
         ref->heapId != static_cast<std::uint32_t>(heap_->heapId()));
    cache.fieldCount = static_cast<std::uint8_t>(view->fields.size());
    for (std::size_t i = 0; i < view->fields.size(); ++i) {
        cache.fields[i] = TypedRawFieldCache{
            view->fields[i].byteOffset,
            view->fields[i].kind,
        };
    }
    if (generatedFields &&
        generatedFieldCount == view->fields.size() &&
        generatedStructSize == view->strideBytes) {
        bool exact = true;
        for (std::size_t i = 0; i < generatedFieldCount; ++i) {
            if (view->fields[i].byteOffset != generatedFields[i].byteOffset ||
                view->fields[i].kind != generatedFields[i].expectedKind) {
                exact = false;
                break;
            }
        }
        cache.exactNativeLayout = exact;
    }
    return cache;
}

bool ExecutionContext::convert(
    semantic::ConversionKind conversion,
    semantic::PrimitiveType targetType,
    bool checkedArithmetic,
    const runtime::Value& value,
    runtime::Value& result) {
    switch (conversion) {
    case semantic::ConversionKind::Identity:
        result = value;
        return true;
    case semantic::ConversionKind::NullToString:
        if (!expectType(value, semantic::PrimitiveType::Null, "conversion")) {
            return false;
        }
        result = runtime::NullString{};
        return true;
    case semantic::ConversionKind::NullToObject:
        if (!expectType(value, semantic::PrimitiveType::Null, "conversion")) {
            return false;
        }
        result = runtime::NullObject{};
        return true;
    case semantic::ConversionKind::NullToArray:
        if (!expectType(value, semantic::PrimitiveType::Null, "conversion")) {
            return false;
        }
        result = runtime::NullArray{};
        return true;
    case semantic::ConversionKind::IntToLong:
        if (!expectType(value, semantic::PrimitiveType::Int, "numeric conversion")) {
            return false;
        }
        result = runtime::LongValue{std::get<std::int64_t>(value)};
        return true;
    case semantic::ConversionKind::IntToDouble:
        if (!expectType(value, semantic::PrimitiveType::Int, "numeric conversion")) {
            return false;
        }
        result = static_cast<double>(std::get<std::int64_t>(value));
        return true;
    case semantic::ConversionKind::LongToDouble:
        if (!expectType(value, semantic::PrimitiveType::Long, "numeric conversion")) {
            return false;
        }
        result = static_cast<double>(std::get<runtime::LongValue>(value).value);
        return true;
    case semantic::ConversionKind::Numeric:
        if (runtime::tryConvertNumeric(
                value, targetType, result, checkedArithmetic)) return true;
        return fail(
            runtime::ErrorCode::IntegerOverflow,
            "numeric conversion overflow");
    case semantic::ConversionKind::None:
        break;
    }
    return fail(
        runtime::ErrorCode::InvalidProgram,
        "unsupported AOT conversion");
}

bool ExecutionContext::unary(
    UnaryOperation operation,
    bool checkedArithmetic,
    const runtime::Value& value,
    runtime::Value& result) {
    switch (operation) {
    case UnaryOperation::NegateInt:
        if (!expectType(value, semantic::PrimitiveType::Int, "negation")) {
            return false;
        }
        if (!checkedArithmetic &&
            std::get<std::int64_t>(value) ==
                std::numeric_limits<std::int32_t>::min()) {
            result = std::get<std::int64_t>(value);
            return true;
        }
        return checkedIntResult(*this, -std::get<std::int64_t>(value), result);
    case UnaryOperation::NegateLong: {
        if (!expectType(value, semantic::PrimitiveType::Long, "negation")) {
            return false;
        }
        const auto number = std::get<runtime::LongValue>(value).value;
        if (number == std::numeric_limits<std::int64_t>::min()) {
            if (!checkedArithmetic) {
                result = runtime::LongValue{number};
                return true;
            }
            return fail(
                runtime::ErrorCode::IntegerOverflow,
                "long negation overflow");
        }
        result = runtime::LongValue{-number};
        return true;
    }
    case UnaryOperation::NegateDouble:
        if (runtime::valueType(value) == semantic::PrimitiveType::Float) {
            result = runtime::FloatValue{
                -std::get<runtime::FloatValue>(value).value};
            return true;
        }
        if (!expectType(value, semantic::PrimitiveType::Double, "negation")) {
            return false;
        }
        result = -std::get<double>(value);
        return true;
    case UnaryOperation::LogicalNot:
        if (!expectType(value, semantic::PrimitiveType::Bool, "logical not")) {
            return false;
        }
        result = !std::get<bool>(value);
        return true;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "unsupported AOT unary operation");
}

bool ExecutionContext::binary(
    BinaryOperation operation,
    bool checkedArithmetic,
    const runtime::Value& left,
    const runtime::Value& right,
    runtime::Value& result) {
    if (operation == BinaryOperation::Equal || operation == BinaryOperation::NotEqual) {
        bool equal = false;
        if (runtime::valueType(left) != runtime::valueType(right)) {
            equal = false;
        } else if (runtime::valueType(left) == semantic::PrimitiveType::String) {
            if (std::holds_alternative<runtime::NullString>(left) ||
                std::holds_alternative<runtime::NullString>(right)) {
                equal = std::holds_alternative<runtime::NullString>(left) &&
                    std::holds_alternative<runtime::NullString>(right);
            } else {
                const auto text = [&](const runtime::Value& value)
                    -> std::optional<std::string_view> {
                    if (const auto* direct = std::get_if<std::string>(&value)) {
                        return *direct;
                    }
                    if (const auto* reference =
                            std::get_if<runtime::ObjectRef>(&value)) {
                        const auto* targetHeap = resolveHeap(*reference);
                        return targetHeap ? targetHeap->stringView(*reference) : std::nullopt;
                    }
                    return std::nullopt;
                };
                const auto a = text(left);
                const auto b = text(right);
                if (!a || !b) {
                    return fail(
                        runtime::ErrorCode::InvalidObjectReference,
                        "string equality encountered an invalid managed reference");
                }
                equal = *a == *b;
            }
        } else {
            const auto checkAliveRef = [&](const runtime::Value& val) {
                const auto* ref = std::get_if<runtime::ObjectRef>(&val);
                if (!ref) return true;
                const auto* targetHeap = resolveHeap(*ref);
                return targetHeap != nullptr && targetHeap->isAlive(*ref);
            };
            if ((runtime::valueType(left) == semantic::PrimitiveType::Object ||
                 runtime::valueType(left) == semantic::PrimitiveType::Array) &&
                (!checkAliveRef(left) || !checkAliveRef(right))) {
                return fail(
                    runtime::ErrorCode::InvalidObjectReference,
                    "reference equality encountered an invalid managed reference");
            }
            equal = left == right;
        }
        result = operation == BinaryOperation::Equal ? equal : !equal;
        return true;
    }

    const auto longOperation =
        operation >= BinaryOperation::AddLong &&
        operation <= BinaryOperation::GreaterOrEqualLong;
    const auto doubleOperation =
        operation >= BinaryOperation::AddDouble &&
        operation <= BinaryOperation::GreaterOrEqualDouble;
    if (longOperation) {
        if (runtime::valueType(left) == semantic::PrimitiveType::ULong) {
            if (!expectType(right, semantic::PrimitiveType::ULong,
                    "binary operation")) return false;
            const auto a = std::get<runtime::ULongValue>(left).value;
            const auto b = std::get<runtime::ULongValue>(right).value;
            switch (operation) {
            case BinaryOperation::AddLong:
                if (checkedArithmetic &&
                    b > std::numeric_limits<std::uint64_t>::max() - a)
                    return fail(runtime::ErrorCode::IntegerOverflow, "ulong addition overflow");
                result = runtime::ULongValue{a + b}; return true;
            case BinaryOperation::SubtractLong:
                if (checkedArithmetic && a < b) return fail(runtime::ErrorCode::IntegerOverflow, "ulong subtraction overflow");
                result = runtime::ULongValue{a - b}; return true;
            case BinaryOperation::MultiplyLong:
                if (checkedArithmetic && b != 0 &&
                    a > std::numeric_limits<std::uint64_t>::max() / b)
                    return fail(runtime::ErrorCode::IntegerOverflow, "ulong multiplication overflow");
                result = runtime::ULongValue{a * b}; return true;
            case BinaryOperation::DivideLong:
                if (b == 0) return fail(runtime::ErrorCode::DivisionByZero, "division by zero");
                result = runtime::ULongValue{a / b}; return true;
            case BinaryOperation::RemainderLong:
                if (b == 0) return fail(runtime::ErrorCode::DivisionByZero, "remainder by zero");
                result = runtime::ULongValue{a % b}; return true;
            case BinaryOperation::LessLong: result = a < b; return true;
            case BinaryOperation::LessOrEqualLong: result = a <= b; return true;
            case BinaryOperation::GreaterLong: result = a > b; return true;
            case BinaryOperation::GreaterOrEqualLong: result = a >= b; return true;
            default: break;
            }
        }
        if (!expectType(left, semantic::PrimitiveType::Long, "binary operation") ||
            !expectType(right, semantic::PrimitiveType::Long, "binary operation")) {
            return false;
        }
        const auto a = std::get<runtime::LongValue>(left).value;
        const auto b = std::get<runtime::LongValue>(right).value;
        switch (operation) {
        case BinaryOperation::AddLong:
            if (!checkedArithmetic) {
                result = runtime::LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) +
                    static_cast<std::uint64_t>(b))};
                return true;
            }
            return checkedLongAdd(*this, a, b, result);
        case BinaryOperation::SubtractLong:
            if (!checkedArithmetic) {
                result = runtime::LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) -
                    static_cast<std::uint64_t>(b))};
                return true;
            }
            return checkedLongSubtract(*this, a, b, result);
        case BinaryOperation::MultiplyLong:
            if (!checkedArithmetic) {
                result = runtime::LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) *
                    static_cast<std::uint64_t>(b))};
                return true;
            }
            return checkedLongMultiply(*this, a, b, result);
        case BinaryOperation::DivideLong:
            if (b == 0) {
                return fail(runtime::ErrorCode::DivisionByZero, "division by zero");
            }
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return fail(
                    runtime::ErrorCode::IntegerOverflow,
                    "long division overflow");
            }
            result = runtime::LongValue{a / b};
            return true;
        case BinaryOperation::RemainderLong:
            if (b == 0) {
                return fail(runtime::ErrorCode::DivisionByZero, "remainder by zero");
            }
            result = runtime::LongValue{
                a == std::numeric_limits<std::int64_t>::min() && b == -1
                    ? 0
                    : a % b};
            return true;
        case BinaryOperation::LessLong: result = a < b; return true;
        case BinaryOperation::LessOrEqualLong: result = a <= b; return true;
        case BinaryOperation::GreaterLong: result = a > b; return true;
        case BinaryOperation::GreaterOrEqualLong: result = a >= b; return true;
        default: break;
        }
    }
    if (doubleOperation) {
        if (runtime::valueType(left) == semantic::PrimitiveType::Float) {
            if (!expectType(right, semantic::PrimitiveType::Float,
                    "binary operation")) return false;
            const auto a = std::get<runtime::FloatValue>(left).value;
            const auto b = std::get<runtime::FloatValue>(right).value;
            switch (operation) {
            case BinaryOperation::AddDouble: result = runtime::FloatValue{a + b}; return true;
            case BinaryOperation::SubtractDouble: result = runtime::FloatValue{a - b}; return true;
            case BinaryOperation::MultiplyDouble: result = runtime::FloatValue{a * b}; return true;
            case BinaryOperation::DivideDouble: result = runtime::FloatValue{a / b}; return true;
            case BinaryOperation::LessDouble: result = a < b; return true;
            case BinaryOperation::LessOrEqualDouble: result = a <= b; return true;
            case BinaryOperation::GreaterDouble: result = a > b; return true;
            case BinaryOperation::GreaterOrEqualDouble: result = a >= b; return true;
            default: break;
            }
        }
        if (!expectType(left, semantic::PrimitiveType::Double, "binary operation") ||
            !expectType(right, semantic::PrimitiveType::Double, "binary operation")) {
            return false;
        }
        const auto a = std::get<double>(left);
        const auto b = std::get<double>(right);
        switch (operation) {
        case BinaryOperation::AddDouble: result = a + b; return true;
        case BinaryOperation::SubtractDouble: result = a - b; return true;
        case BinaryOperation::MultiplyDouble: result = a * b; return true;
        case BinaryOperation::DivideDouble: result = a / b; return true;
        case BinaryOperation::LessDouble: result = a < b; return true;
        case BinaryOperation::LessOrEqualDouble: result = a <= b; return true;
        case BinaryOperation::GreaterDouble: result = a > b; return true;
        case BinaryOperation::GreaterOrEqualDouble: result = a >= b; return true;
        default: break;
        }
    }
    if (runtime::valueType(left) == semantic::PrimitiveType::UInt) {
        if (!expectType(right, semantic::PrimitiveType::UInt,
                "binary operation")) return false;
        const auto a = std::get<runtime::UIntValue>(left).value;
        const auto b = std::get<runtime::UIntValue>(right).value;
        switch (operation) {
        case BinaryOperation::AddInt:
            if (checkedArithmetic &&
                b > std::numeric_limits<std::uint32_t>::max() - a)
                return fail(runtime::ErrorCode::IntegerOverflow, "uint addition overflow");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a + b)}; return true;
        case BinaryOperation::SubtractInt:
            if (checkedArithmetic && a < b) return fail(runtime::ErrorCode::IntegerOverflow, "uint subtraction overflow");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a - b)}; return true;
        case BinaryOperation::MultiplyInt:
            if (checkedArithmetic && b != 0 &&
                a > std::numeric_limits<std::uint32_t>::max() / b)
                return fail(runtime::ErrorCode::IntegerOverflow, "uint multiplication overflow");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a * b)}; return true;
        case BinaryOperation::DivideInt:
            if (b == 0) return fail(runtime::ErrorCode::DivisionByZero, "division by zero");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a / b)}; return true;
        case BinaryOperation::RemainderInt:
            if (b == 0) return fail(runtime::ErrorCode::DivisionByZero, "remainder by zero");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a % b)}; return true;
        case BinaryOperation::LessInt: result = a < b; return true;
        case BinaryOperation::LessOrEqualInt: result = a <= b; return true;
        case BinaryOperation::GreaterInt: result = a > b; return true;
        case BinaryOperation::GreaterOrEqualInt: result = a >= b; return true;
        default: break;
        }
    }
    if (!expectType(left, semantic::PrimitiveType::Int, "binary operation") ||
        !expectType(right, semantic::PrimitiveType::Int, "binary operation")) {
        return false;
    }
    const auto a = std::get<std::int64_t>(left);
    const auto b = std::get<std::int64_t>(right);
    switch (operation) {
    case BinaryOperation::AddInt:
        if (!checkedArithmetic) { result = wrapInt32(a + b); return true; }
        return checkedIntResult(*this, a + b, result);
    case BinaryOperation::SubtractInt:
        if (!checkedArithmetic) { result = wrapInt32(a - b); return true; }
        return checkedIntResult(*this, a - b, result);
    case BinaryOperation::MultiplyInt:
        if (!checkedArithmetic) { result = wrapInt32(a * b); return true; }
        return checkedIntResult(*this, a * b, result);
    case BinaryOperation::DivideInt:
        if (b == 0) {
            return fail(runtime::ErrorCode::DivisionByZero, "division by zero");
        }
        if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
            return fail(
                runtime::ErrorCode::IntegerOverflow,
                "integer division overflow");
        }
        result = a / b;
        return true;
    case BinaryOperation::RemainderInt:
        if (b == 0) {
            return fail(runtime::ErrorCode::DivisionByZero, "remainder by zero");
        }
        result = a == std::numeric_limits<std::int32_t>::min() && b == -1
            ? std::int64_t{0}
            : a % b;
        return true;
    case BinaryOperation::LessInt: result = a < b; return true;
    case BinaryOperation::LessOrEqualInt: result = a <= b; return true;
    case BinaryOperation::GreaterInt: result = a > b; return true;
    case BinaryOperation::GreaterOrEqualInt: result = a >= b; return true;
    default: break;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "unsupported AOT binary operation");
}

const ProgramDescriptor& ExecutionContext::program() const noexcept {
    return *program_;
}

std::shared_ptr<runtime::ManagedHeap> ExecutionContext::heap() const noexcept {
    return heap_;
}

void ExecutionContext::setDomainContext(
    runtime::ExecutionDomainContext* domains,
    runtime::DomainKind domainKind) noexcept {
    domainContext_ = domains;
    domainKind_ = domainKind;
    if (domains && program_) {
        registerAotProgramObservableTypes(domains->simHeap(), *program_);
        registerAotProgramObservableTypes(domains->uiHeap(), *program_);
    }
}

runtime::ShadowStack& ExecutionContext::shadowStack() noexcept {
    return shadowStack_;
}

const runtime::RuntimeError& ExecutionContext::error() const noexcept {
    return error_;
}

const runtime::RuntimeStatistics& ExecutionContext::statistics() const noexcept {
    return statistics_;
}

std::uint64_t ExecutionContext::instructionsExecuted() const noexcept {
    return executed_;
}

bool ExecutionContext::finalizeDeterminism(
    bool succeeded,
    const runtime::Value& value) {
    if (determinismEventsEnabled_) {
        determinism_.mergeEventDigest(
            determinismEventDigest_, determinismEventCount_);
    }
    succeeded = determinism_.finish(succeeded, value, error_, statistics_);
    if (options_.profile) {
        runtime::ExecutionProfile profile;
        profile.totalEvents = profileEvents_;
        profile.functions.reserve(profileFunctions_.size());
        for (auto& [symbolId, function] : profileFunctions_) {
            (void)symbolId;
            profile.functions.push_back(std::move(function));
        }
        options_.profile->merge(profile);
    }
    return succeeded;
}

std::uint64_t ExecutionContext::determinismDigest() const noexcept {
    return determinism_.digest();
}

std::size_t ExecutionContext::replayEntriesConsumed() const noexcept {
    return determinism_.replayEntriesConsumed();
}

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define RS_AOT_THROW_INVALID_ARG(msg) throw std::invalid_argument(msg)
#else
#define RS_AOT_THROW_INVALID_ARG(msg) std::abort()
#endif

Program::Program(const ProgramDescriptor& descriptor)
    : descriptor_(&descriptor),
      heap_(std::make_shared<runtime::ManagedHeap>()) {
    if (descriptor.abiMajor != RuntimeAbiMajor ||
        descriptor.abiMinor > RuntimeAbiMinor) {
        RS_AOT_THROW_INVALID_ARG("AOT program requires an incompatible runtime ABI");
    }
    if (!descriptor.name || descriptor.name[0] == '\0' ||
        descriptor.moduleCount == 0 || !descriptor.moduleNames ||
        descriptor.functionCount == 0 || !descriptor.functions ||
        (descriptor.typeCount != 0 && !descriptor.types) ||
        (descriptor.sourceMapCount != 0 && !descriptor.sourceMap)) {
        RS_AOT_THROW_INVALID_ARG("AOT program descriptor is incomplete");
    }

    std::unordered_set<std::string> moduleNames;
    for (std::uint32_t index = 0; index < descriptor.moduleCount; ++index) {
        const auto* name = descriptor.moduleNames[index];
        if (!name || name[0] == '\0' || !moduleNames.emplace(name).second) {
            RS_AOT_THROW_INVALID_ARG("AOT program contains an invalid module table");
        }
    }

    std::unordered_set<semantic::SymbolId> typeIds;
    for (std::uint32_t index = 0; index < descriptor.typeCount; ++index) {
        const auto& type = descriptor.types[index];
        if (type.id == 0 || !type.moduleName || type.moduleName[0] == '\0' ||
            !type.name || type.name[0] == '\0' ||
            !typeIds.insert(type.id).second ||
            (type.fieldCount != 0 && !type.fields) ||
            (type.enumMemberCount != 0 && !type.enumMembers)) {
            RS_AOT_THROW_INVALID_ARG("AOT program contains an invalid type table");
        }
        if (moduleNames.count(type.moduleName) == 0 ||
            (type.kind != semantic::TypeKind::Class &&
             type.kind != semantic::TypeKind::Struct &&
             type.kind != semantic::TypeKind::Enum) ||
            (type.kind == semantic::TypeKind::Enum && type.fieldCount != 0) ||
            (type.kind != semantic::TypeKind::Enum &&
             type.enumMemberCount != 0)) {
            RS_AOT_THROW_INVALID_ARG("AOT type descriptor kind is inconsistent");
        }
        std::unordered_set<std::string> fieldNames;
        for (std::uint32_t fieldIndex = 0;
             fieldIndex < type.fieldCount;
             ++fieldIndex) {
            const auto& field = type.fields[fieldIndex];
            if (!field.name || field.name[0] == '\0' ||
                field.index != fieldIndex ||
                !validStorageType(field.type) ||
                !validSignatureIdentity(field.type, field.typeId) ||
                !fieldNames.emplace(field.name).second) {
                RS_AOT_THROW_INVALID_ARG("AOT program contains an invalid field table");
            }
        }
        std::unordered_set<std::string> enumNames;
        for (std::uint32_t memberIndex = 0;
             memberIndex < type.enumMemberCount;
             ++memberIndex) {
            const auto& member = type.enumMembers[memberIndex];
            if (!member.name || member.name[0] == '\0' ||
                !enumNames.emplace(member.name).second) {
                RS_AOT_THROW_INVALID_ARG("AOT program contains an invalid enum table");
            }
        }
    }

    for (std::uint32_t index = 0; index < descriptor.typeCount; ++index) {
        const auto& type = descriptor.types[index];
        types_.emplace(type.id, &type);
        for (std::uint32_t fieldIndex = 0;
             fieldIndex < type.fieldCount;
             ++fieldIndex) {
            const auto& field = type.fields[fieldIndex];
            if (requiresDescriptor(field.type) &&
                typeIds.count(field.typeId) == 0) {
                RS_AOT_THROW_INVALID_ARG(
                    "AOT field references an unknown exact type");
            }
        }
    }

    std::unordered_set<semantic::SymbolId> functionIds;
    for (std::uint32_t index = 0; index < descriptor.functionCount; ++index) {
        const auto& function = descriptor.functions[index];
        const std::string_view qualifiedName =
            function.name ? std::string_view(function.name) : std::string_view{};
        const auto separator = qualifiedName.find("::");
        const auto moduleName = separator == std::string_view::npos
            ? std::string_view{}
            : qualifiedName.substr(0, separator);
        if (function.symbolId == 0 || !function.name ||
            function.name[0] == '\0' || !function.implementation ||
            moduleName.empty() ||
            moduleNames.count(std::string(moduleName)) == 0 ||
            !functionIds.insert(function.symbolId).second ||
            !validReturnType(function.returnType) ||
            !validSignatureIdentity(function.returnType, function.returnTypeId) ||
            (requiresDescriptor(function.returnType) &&
             typeIds.count(function.returnTypeId) == 0) ||
            (function.parameterCount != 0 &&
             (!function.parameterTypes || !function.parameterTypeIds))) {
            RS_AOT_THROW_INVALID_ARG("AOT program contains an invalid function table");
        }
        // Overloads intentionally share a qualified source name. Calls emitted
        // by the compiler use SymbolId; the name-only embedding convenience API
        // retains the first deterministic descriptor.
        names_.try_emplace(function.name, function.symbolId);
        functions_.emplace(function.symbolId, &function);
        for (std::uint32_t parameter = 0;
             parameter < function.parameterCount;
             ++parameter) {
            if (!validStorageType(function.parameterTypes[parameter]) ||
                !validSignatureIdentity(
                    function.parameterTypes[parameter],
                    function.parameterTypeIds[parameter]) ||
                (requiresDescriptor(function.parameterTypes[parameter]) &&
                 typeIds.count(function.parameterTypeIds[parameter]) == 0)) {
                RS_AOT_THROW_INVALID_ARG(
                    "AOT program contains an invalid function signature");
            }
        }
    }

    for (std::uint32_t index = 0; index < descriptor.sourceMapCount; ++index) {
        const auto& entry = descriptor.sourceMap[index];
        if (functionIds.count(entry.symbolId) == 0 ||
            entry.generatedLine == 0 || !entry.sourcePath ||
            entry.sourcePath[0] == '\0' || entry.sourceLine == 0 ||
            entry.sourceColumn == 0) {
            RS_AOT_THROW_INVALID_ARG("AOT program contains an invalid source map");
        }
    }
    if (heap_) {
        registerAotProgramObservableTypes(*heap_, descriptor);
    }
}

#undef RS_AOT_THROW_INVALID_ARG

void Program::setBindings(
    std::shared_ptr<const runtime::BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
}

void Program::setHeap(std::shared_ptr<runtime::ManagedHeap> heap) {
    heap_ = heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>();
    if (heap_ && descriptor_) {
        registerAotProgramObservableTypes(*heap_, *descriptor_);
    }
}

std::shared_ptr<runtime::ManagedHeap> Program::heap() const noexcept {
    return heap_;
}

void Program::setDomainContext(
    runtime::ExecutionDomainContext* domains,
    runtime::DomainKind domainKind) noexcept {
    domainContext_ = domains;
    domainKind_ = domainKind;
    if (domains && descriptor_) {
        registerAotProgramObservableTypes(domains->simHeap(), *descriptor_);
        registerAotProgramObservableTypes(domains->uiHeap(), *descriptor_);
    }
}

const ProgramDescriptor& Program::descriptor() const noexcept {
    return *descriptor_;
}

runtime::ExecutionResult Program::invoke(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    ExecutionContext context(
        *descriptor_,
        heap_,
        bindings_,
        std::move(options),
        &types_,
        &functions_);
    if (domainContext_) {
        context.setDomainContext(domainContext_, domainKind_);
    }
    runtime::Value value;
    runtime::ExecutionResult execution;
    execution.succeeded = context.invoke(
        symbolId,
        arguments.data(),
        arguments.size(),
        value);
    if (!execution.succeeded && context.hasPendingException()) {
        context.reportUnhandledScriptException();
    }
    execution.succeeded = context.finalizeDeterminism(
        execution.succeeded, value);
    execution.value = std::move(value);
    execution.error = context.error();
    execution.instructionsExecuted = context.instructionsExecuted();
    execution.statistics = context.statistics();
    execution.determinismDigest = context.determinismDigest();
    execution.replayEntriesConsumed = context.replayEntriesConsumed();
    return execution;
}

runtime::ExecutionResult Program::invoke(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    const auto found = names_.find(qualifiedName);
    if (found == names_.end()) {
        runtime::ExecutionResult missing;
        missing.error.code = runtime::ErrorCode::FunctionNotFound;
        missing.error.message = "AOT entry function was not found";
        return missing;
    }
    return invoke(found->second, arguments, std::move(options));
}

std::optional<runtime::StackTraceLocation> Program::resolveSourceLocation(
    semantic::SymbolId symbolId,
    std::uint32_t generatedLine) const {
    if (!descriptor_) return std::nullopt;
    auto loc = lookupDescriptorSourceLocation(*descriptor_, symbolId, nullptr, generatedLine);
    if (loc && loc->hasSourceLocation()) return loc;
    return std::nullopt;
}

std::optional<runtime::StackTraceLocation> Program::resolveSourceLocation(
    const std::string& qualifiedName,
    std::uint32_t generatedLine) const {
    const auto found = names_.find(qualifiedName);
    if (found == names_.end()) return std::nullopt;
    return resolveSourceLocation(found->second, generatedLine);
}

std::vector<std::string> Program::symbolizeStackTrace(
    const runtime::RuntimeError& error) const {
    if (!error.symbolizedStackTrace.empty()) {
        return error.symbolizedStackTrace;
    }
    if (!error.sourceLocations.empty()) {
        std::vector<std::string> out;
        out.reserve(error.sourceLocations.size());
        for (const auto& loc : error.sourceLocations) {
            out.push_back(loc.format());
        }
        return out;
    }
    std::vector<std::string> out;
    out.reserve(error.stackTrace.size());
    for (const auto& frameName : error.stackTrace) {
        if (const auto loc = resolveSourceLocation(frameName, 0)) {
            out.push_back(loc->format());
        } else {
            out.push_back(frameName);
        }
    }
    return out;
}

} // namespace realscript::aot
