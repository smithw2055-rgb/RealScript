#include "realscript/aot_cpp/AotRuntime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
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

FrameScope::FrameScope(
    ExecutionContext& context,
    const std::vector<runtime::Value>& arguments,
    const std::vector<runtime::Value>& locals,
    const std::vector<runtime::Value>& registers,
    bool allowTransientStackSlices)
    : context_(&context),
      savedStackRefTop_(context.stackRefTop_),
      allowTransientStackSlices_(allowTransientStackSlices) {
    if (allowTransientStackSlices_) {
        ++context.activeStackRefScopes_;
    }
    context_->shadowStack().pushFrame(&arguments, &locals, &registers);
}

FrameScope::FrameScope(
    ExecutionContext& context,
    runtime::ValueSpan arguments,
    runtime::ValueSpan locals,
    runtime::ValueSpan registers,
    bool allowTransientStackSlices)
    : context_(&context),
      savedStackRefTop_(context.stackRefTop_),
      allowTransientStackSlices_(allowTransientStackSlices) {
    if (allowTransientStackSlices_) {
        ++context.activeStackRefScopes_;
    }
    context_->shadowStack().pushSpanFrame(arguments, locals, registers);
}

FrameScope::~FrameScope() {
    if (!context_) return;
    context_->shadowStack().popFrame();
    if (allowTransientStackSlices_ && context_->activeStackRefScopes_ > 0) {
        --context_->activeStackRefScopes_;
    }
    if (context_->stackRefTop_ > savedStackRefTop_) {
        for (std::uint32_t i = savedStackRefTop_; i < context_->stackRefTop_ && i < context_->stackRefSlots_.size(); ++i) {
            if (context_->stackRefSlots_[i].isTraceable()) {
                context_->stackRefSlots_[i] = runtime::Value{};
            }
        }
        context_->stackRefTop_ = savedStackRefTop_;
    }
}

StackRefScope::StackRefScope(ExecutionContext& context) noexcept
    : context_(&context), savedTop_(context.stackRefTop_) {
    ++context.activeStackRefScopes_;
}

StackRefScope::~StackRefScope() noexcept {
    if (!context_) return;
    if (context_->activeStackRefScopes_ > 0) {
        --context_->activeStackRefScopes_;
    }
    if (context_->stackRefTop_ > savedTop_) {
        for (std::uint32_t i = savedTop_; i < context_->stackRefTop_ && i < context_->stackRefSlots_.size(); ++i) {
            if (context_->stackRefSlots_[i].isTraceable()) {
                context_->stackRefSlots_[i] = runtime::Value{};
            }
        }
        context_->stackRefTop_ = savedTop_;
    }
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
      executionPolicy_(options.executionPolicy),
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
    telemetry_ = runtime::activeFrameTelemetry();
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
        options_.profile == nullptr && telemetry_ == nullptr &&
        !diagnosticEventsEnabled_;
    if (prebuiltTypes && prebuiltFunctions) {
        typesPtr_ = prebuiltTypes;
        functionsPtr_ = prebuiltFunctions;
    } else {
        struct CachedDescriptorMaps {
            std::uint64_t contentHash = 0;
            const TypeDescriptor* types = nullptr;
            std::uint32_t typeCount = 0;
            const FunctionDescriptor* functions = nullptr;
            std::uint32_t functionCount = 0;
            std::unordered_map<semantic::SymbolId, const TypeDescriptor*> typeMap;
            std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> functionMap;
        };
        static std::mutex descriptorCacheMutex;
        static std::unordered_map<const ProgramDescriptor*, std::shared_ptr<CachedDescriptorMaps>>
            descriptorCache;

        std::shared_ptr<CachedDescriptorMaps> cached;
        if (program.contentHash != 0) {
            std::lock_guard<std::mutex> lock(descriptorCacheMutex);
            auto it = descriptorCache.find(&program);
            if (it != descriptorCache.end()) {
                const auto& e = *it->second;
                if (e.contentHash == program.contentHash &&
                    e.types == program.types &&
                    e.typeCount == program.typeCount &&
                    e.functions == program.functions &&
                    e.functionCount == program.functionCount) {
                    cached = it->second;
                } else {
                    descriptorCache.erase(it);
                }
            }
        }
        if (!cached) {
            auto entry = std::make_shared<CachedDescriptorMaps>();
            entry->contentHash = program.contentHash;
            entry->types = program.types;
            entry->typeCount = program.typeCount;
            entry->functions = program.functions;
            entry->functionCount = program.functionCount;
            entry->typeMap.reserve(program.types ? program.typeCount : 0u);
            std::vector<std::pair<semantic::SymbolId, std::vector<runtime::BlittableFieldInput>>> structSpecs;
            if (program.types) {
                for (std::uint32_t index = 0; index < program.typeCount; ++index) {
                    const auto& type = program.types[index];
                    entry->typeMap.emplace(type.id, &type);
                    if (type.kind == semantic::TypeKind::Struct &&
                        !type.hasValueLifecycle &&
                        type.fields && type.fieldCount > 0) {
                        bool hasStaticOrConstField = false;
                        std::vector<runtime::BlittableFieldInput> fInputs;
                        fInputs.reserve(type.fieldCount);
                        for (std::uint32_t fi = 0; fi < type.fieldCount; ++fi) {
                            if (type.fields[fi].staticField || type.fields[fi].constField) {
                                hasStaticOrConstField = true;
                                break;
                            }
                            fInputs.push_back(runtime::BlittableFieldInput{
                                type.fields[fi].type, type.fields[fi].typeId});
                        }
                        if (!hasStaticOrConstField && !fInputs.empty()) {
                            structSpecs.emplace_back(type.id, std::move(fInputs));
                        }
                    }
                }
            }
            if (!structSpecs.empty()) {
                runtime::registerBlittableStructTypes(structSpecs);
            }
            entry->functionMap.reserve(program.functions ? program.functionCount : 0u);
            if (program.functions) {
                for (std::uint32_t index = 0; index < program.functionCount; ++index) {
                    const auto& function = program.functions[index];
                    entry->functionMap.emplace(function.symbolId, &function);
                }
            }
            cached = entry;
            if (program.contentHash != 0) {
                std::lock_guard<std::mutex> lock(descriptorCacheMutex);
                if (descriptorCache.size() >= 256u) {
                    descriptorCache.clear();
                }
                descriptorCache.insert_or_assign(&program, entry);
            }
        }
        descriptorMapsOwner_ = cached;
        typesPtr_ = prebuiltTypes ? prebuiltTypes : &cached->typeMap;
        functionsPtr_ = prebuiltFunctions ? prebuiltFunctions : &cached->functionMap;
    }
    stack_.reserve(64);
    stackSites_.reserve(64);
    stackRefBankId_ = runtime::detail::acquireFlatStackBank(&stackRefSlots_);
    shadowStack_.pushFrame(&stackRefEmptyRoot_, &stackRefSlots_, &stackRefEmptyRoot_);
    stackRefRootPushed_ = true;
    if (heap_) {
        activeShadowStackScope_.emplace(heap_.get(), &shadowStack_);
    }
}

ExecutionContext::~ExecutionContext() {
    activeShadowStackScope_.reset();
    if (stackRefRootPushed_) {
        shadowStack_.popFrame();
        stackRefRootPushed_ = false;
    }
    if (stackRefBankId_ != 0u) {
        runtime::detail::releaseFlatStackBank(stackRefBankId_);
        stackRefBankId_ = 0u;
    }
}

runtime::CrossHeapAccessPolicy ExecutionContext::effectiveCrossHeapPolicy(
    const runtime::ManagedHeap* targetHeap) const noexcept {
    auto policy = options_.crossHeapPolicy;
    if (executionPolicy_) {
        const auto domainPolicy =
            executionPolicy_->crossHeapAccessPolicy(heap_.get(), targetHeap);
        if (static_cast<std::uint8_t>(domainPolicy) >
            static_cast<std::uint8_t>(policy)) {
            policy = domainPolicy;
        }
    }
    return policy;
}

bool ExecutionContext::checkCrossHeapRefAccess(
    runtime::ObjectRef reference,
    bool isWrite,
    std::string_view context) {
    if (reference.isStackSlotRef() || !heap_ || !reference.valid()) {
        return true;
    }
    if (reference.heapId != 0 &&
        reference.heapId != static_cast<std::uint32_t>(heap_->heapId())) {
        runtime::ManagedHeap* targetHeap = resolveHeap(reference);
        const auto policy = effectiveCrossHeapPolicy(targetHeap);
        if (isWrite) {
            if (policy != runtime::CrossHeapAccessPolicy::AllowReadWrite) {
                return fail(
                    runtime::ErrorCode::CrossHeapAccessViolation,
                    std::string(context) +
                        " rejected cross-heap mutation by active CrossHeapAccessPolicy");
            }
        } else if (policy == runtime::CrossHeapAccessPolicy::Deny) {
            return fail(
                runtime::ErrorCode::CrossHeapAccessViolation,
                std::string(context) +
                    " rejected cross-heap access by active CrossHeapAccessPolicy");
        }
    }
    return true;
}

runtime::Value ExecutionContext::sanitizeCrossHeapValue(
    runtime::ManagedHeap* sourceHeap,
    runtime::Value val) {
    if (sourceHeap && heap_ && sourceHeap != heap_.get()) {
        if (const auto* strRef = std::get_if<runtime::ObjectRef>(&val)) {
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
    return val;
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
    if (actual == 0) return false;
    if (expected == 0) return findType(actual) != nullptr;
    const std::size_t cIdx =
        static_cast<std::size_t>(((actual >> 4u) ^ (expected * 0x9e3779b97f4a7c15ULL)) & 31ULL);
    const auto& cached = assignableCache_[cIdx];
    if (cached.actual == actual && cached.expected == expected && cached.result != 0) {
        return cached.result == 2u;
    }
    auto current = actual;
    bool matched = false;
    for (std::size_t depth = 0; current != 0 && depth < 256; ++depth) {
        const auto* type = findType(current);
        if (!type) break;
        if (type->interfaceDispatchMaps && type->interfaceDispatchMapCount > 0) {
            if (type->interfaceDispatchMapCount <= 4) {
                for (std::uint32_t index = 0;
                     index < type->interfaceDispatchMapCount;
                     ++index) {
                    if (type->interfaceDispatchMaps[index].interfaceTypeId == expected) {
                        matched = true;
                        break;
                    }
                }
            } else {
                std::uint32_t lo = 0;
                std::uint32_t hi = type->interfaceDispatchMapCount;
                while (lo < hi) {
                    const std::uint32_t mid = lo + ((hi - lo) >> 1u);
                    if (type->interfaceDispatchMaps[mid].interfaceTypeId < expected) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                if (lo < type->interfaceDispatchMapCount &&
                    type->interfaceDispatchMaps[lo].interfaceTypeId == expected) {
                    matched = true;
                }
            }
            if (matched) break;
        }
        if (type->baseTypeId == expected) {
            matched = true;
            break;
        }
        current = type->baseTypeId;
    }
    assignableCache_[cIdx] = {actual, expected, static_cast<std::uint8_t>(matched ? 2u : 1u)};
    return matched;
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
    if (heap_ && options_.limits.gcWorkBudget != 0 &&
        (heap_->phase() != runtime::GcPhase::Idle || heap_->collectionRequested())) {
        const auto gcSliceStart = telemetry_
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        const auto work = heap_->step(
            shadowStack_,
            options_.limits.gcWorkBudget);
        statistics_.gcWorkPerformed += work;
        if (work != 0 && traceEventsEnabled_) {
            if (diagnosticEventsEnabled_) {
                const auto workText = std::to_string(work);
                emitTrace(runtime::TraceEventKind::GcStep, work, workText);
            } else {
                emitTrace(runtime::TraceEventKind::GcStep, work);
            }
        }
        if (telemetry_ != nullptr) {
            const auto elapsedNs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - gcSliceStart).count());
            telemetry_->recordGcSliceNanoseconds(elapsedNs);
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
    if (!reference) {
        return fail(
            runtime::ErrorCode::NullReference,
            "throw requires a non-null script object");
    }
    if (!checkCrossHeapRefAccess(*reference, false, "throw")) {
        return false;
    }
    auto* targetHeap = resolveHeap(*reference);
    if (!targetHeap || !targetHeap->isAlive(*reference)) {
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
    auto* targetHeap = resolveHeap(*reference);
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

bool ExecutionContext::expectType(
    const runtime::Value& value,
    semantic::PrimitiveType type,
    std::string_view context) {
    if (const auto* reference = std::get_if<runtime::ObjectRef>(&value)) {
        if (reference->isStackSlotRef()) {
            if (reference->generation != stackRefBankId_ || reference->slot >= stackRefTop_) {
                return fail(
                    runtime::ErrorCode::InvalidObjectReference,
                    std::string(context) +
                        " contains an expired AOT stack slot reference");
            }
        } else {
            if (!checkCrossHeapRefAccess(*reference, false, context)) {
                return false;
            }
            auto* targetHeap = resolveHeap(*reference);
            if (!targetHeap || !targetHeap->isAlive(*reference)) {
                return fail(
                    runtime::ErrorCode::InvalidObjectReference,
                    std::string(context) +
                        " contains an invalid managed object reference");
            }
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
        if (reference && reference->isStackSlotRef()) {
            const auto* expType = findType(typeId);
            if (reference->generation == stackRefBankId_ &&
                reference->slot < stackRefTop_ &&
                expType && expType->name &&
                runtime::isSynthesizedRefWrapperTypeName(expType->name)) {
                return true;
            }
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has an incompatible AOT stack reference type");
        }
        auto* targetHeap = reference ? resolveHeap(*reference) : nullptr;
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
        auto* targetHeap = reference ? resolveHeap(*reference) : nullptr;
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
        if (descriptor->fieldCount <= 8u) {
            std::array<runtime::Value, 8> fields{};
            for (std::uint32_t index = 0; index < descriptor->fieldCount; ++index) {
                const auto& field = descriptor->fields[index];
                fields[index] = defaultValue(
                    field.type,
                    field.typeId,
                    depth + 1);
            }
            return runtime::makeStructValueFromFields(
                typeId, runtime::ValueSpan(fields.data(), descriptor->fieldCount));
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
    runtime::ScopedValuePoolDomain domainScope(
        heap_ ? heap_->valuePoolDomain().get() : nullptr);
    const auto* function = findFunction(symbolId);
    const FunctionImplementation overrideImpl =
        function ? resolveHotPatchImplementation(function->symbolId) : nullptr;
    const FunctionImplementation targetImpl =
        overrideImpl ? overrideImpl : (function ? function->implementation : nullptr);
    if (!function || !targetImpl) {
        return fail(
            runtime::ErrorCode::FunctionNotFound,
            "AOT entry function was not found");
    }
    if (stack_.size() >= options_.limits.recursionLimit) {
        return fail(
            runtime::ErrorCode::RecursionLimitExceeded,
            "recursion limit exceeded");
    }
    const bool isResumingThisFunction =
        resumingFramePtr_ != nullptr &&
        resumingCoroutine_.valid() &&
        resumingFramePtr_->functionSymbolId == symbolId;
    if (!isResumingThisFunction) {
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
    }
    if (!isResumingThisFunction && executionPolicy_) {
        bool intercepted = false;
        runtime::Value interceptedResult{};
        runtime::RuntimeError policyError{};
        if (!executionPolicy_->interceptCall(
                function->symbolId,
                runtime::ValueSpan(arguments, argumentCount),
                intercepted,
                interceptedResult,
                policyError)) {
            const auto previousFunctionId = currentFunctionId_;
            currentFunctionId_ = function->symbolId;
            stack_.push_back(function->name ? function->name : "<aot-function>");
            stackSites_.push_back({function->symbolId, 0});
            error_ = std::move(policyError);
            if (error_.code == runtime::ErrorCode::None) {
                error_.code = runtime::ErrorCode::CapabilityDenied;
            }
            if (error_.message.empty()) {
                error_.message = "call intercepted and rejected by execution policy";
            }
            populateErrorStackTrace();
            if (!stack_.empty()) stack_.pop_back();
            if (!stackSites_.empty()) stackSites_.pop_back();
            currentFunctionId_ = previousFunctionId;
            return false;
        }
        if (intercepted) {
            if (function->returnType == semantic::PrimitiveType::Void) {
                result = std::monostate{};
            } else if (std::holds_alternative<std::monostate>(interceptedResult)) {
                result = defaultValue(function->returnType, function->returnTypeId);
            } else {
                if (!expectSignatureType(
                        interceptedResult,
                        function->returnType,
                        function->returnTypeId,
                        "intercepted return value")) {
                    return false;
                }
                result = std::move(interceptedResult);
            }
            return true;
        }
    }

    if (trustedReleaseEnabled()) {
        const auto previousFunctionId = currentFunctionId_;
        currentFunctionId_ = function->symbolId;
        const runtime::ScopedAllocationSite fnAllocSite(function->symbolId, 0);
        stack_.push_back(function->name ? function->name : "<aot-function>");
        stackSites_.push_back({function->symbolId, 0});
        ++statistics_.functionCalls;
        if (statistics_.maximumCallDepth == 0) {
            statistics_.maximumCallDepth = 1;
        }
        const bool ok = targetImpl(
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
    const runtime::ScopedAllocationSite fnAllocSite(function->symbolId, 0);
    stack_.push_back(function->name ? function->name : "<aot-function>");
    stackSites_.push_back({function->symbolId, 0});
    ++statistics_.functionCalls;
    statistics_.maximumCallDepth = std::max(
        statistics_.maximumCallDepth,
        stack_.size());
    struct AotInvokeProfileGuard {
        runtime::ProfileCollector* collector = nullptr;
        semantic::SymbolId functionId = 0;
        const char* functionName = nullptr;
        std::size_t depth = 0;
        std::uint64_t startNs = 0;
        ~AotInvokeProfileGuard() noexcept {
            if (collector) {
                const std::uint64_t endNs = collector->nowSteadyNs();
                const std::uint64_t durationNs =
                    endNs >= startNs ? (endNs - startNs) : 0ULL;
                collector->recordTraceSpan(
                    functionId,
                    functionName ? std::string_view(functionName)
                                 : std::string_view("<aot-function>"),
                    startNs,
                    durationNs,
                    depth);
            }
        }
    } profileGuard{
        options_.profile.get(),
        function->symbolId,
        function->name,
        stack_.size(),
        options_.profile ? options_.profile->nowSteadyNs() : 0ULL};
    struct AotInvokeTelemetryScope {
        ExecutionContext& ctx;
        semantic::SymbolId functionId = 0;
        std::uint64_t entryExecuted = 0;
        std::uint64_t savedParentChildExecuted = 0;
        AotInvokeTelemetryScope(ExecutionContext& context, semantic::SymbolId symId) noexcept
            : ctx(context),
              functionId(symId),
              entryExecuted(context.executed_),
              savedParentChildExecuted(context.telemetryChildExecuted_) {
            if (ctx.telemetry_ != nullptr) {
                ctx.telemetryChildExecuted_ = 0;
            }
        }
        ~AotInvokeTelemetryScope() noexcept {
            if (ctx.telemetry_ != nullptr) {
                const std::uint64_t totalSubtree = ctx.executed_ >= entryExecuted
                    ? (ctx.executed_ - entryExecuted)
                    : 0u;
                const std::uint64_t selfInst = totalSubtree >= ctx.telemetryChildExecuted_
                    ? (totalSubtree - ctx.telemetryChildExecuted_)
                    : 0u;
                if (selfInst > 0) {
                    ctx.telemetry_->recordFunctionInstructions(functionId, selfInst);
                }
                ctx.telemetryChildExecuted_ = savedParentChildExecuted + totalSubtree;
            }
        }
    } telemetryScope{*this, function->symbolId};
    emitTrace(runtime::TraceEventKind::FunctionEnter);

    bool succeeded = false;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        succeeded = targetImpl(
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
    succeeded = targetImpl(
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
            const auto* resHeap =
                result.rawTag() == runtime::ValueTag::ObjectRef
                    ? resolveHeap(result.asObjectRefUnchecked())
                    : heap_.get();
            const auto operation = runtime::valueToString(
                result,
                resHeap ? resHeap : heap_.get());
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

bool ExecutionContext::invokeParallelBatch(
    semantic::SymbolId symbolId,
    ParallelBatchImplementation batchKernel,
    runtime::ParallelNativeBatchContext& batch) {
    runtime::ScopedValuePoolDomain domainScope(
        heap_ ? heap_->valuePoolDomain().get() : nullptr);
    const auto* function = findFunction(symbolId);
    const FunctionImplementation overrideImpl =
        function ? resolveHotPatchImplementation(function->symbolId) : nullptr;
    const FunctionImplementation targetImpl =
        overrideImpl ? overrideImpl : (function ? function->implementation : nullptr);
    if (!function || (!batchKernel && !targetImpl)) {
        const bool ok = fail(
            runtime::ErrorCode::FunctionNotFound,
            "AOT entry function was not found");
        batch.error = error_;
        return ok;
    }
    if (stack_.size() >= options_.limits.recursionLimit) {
        const bool ok = fail(
            runtime::ErrorCode::RecursionLimitExceeded,
            "recursion limit exceeded");
        batch.error = error_;
        return ok;
    }

    const bool isRange = (batch.kernelKind == runtime::ParallelKernelKind::Range);
    const std::size_t prefixParams = isRange ? 2u : 1u;
    const std::size_t userArgCount = batch.userArguments ? batch.userArguments->size() : 0u;
    if (function->parameterCount != prefixParams + userArgCount) {
        const bool ok = fail(
            runtime::ErrorCode::InvalidArguments,
            "argument count does not match function signature");
        batch.error = error_;
        return ok;
    }
    if (!isRange && batch.count == 0u) {
        batch.hasReducedValue = false;
        batch.reducedValue = runtime::Value{std::monostate{}};
        batch.statistics = statistics_;
        return true;
    }

    const auto previousFunctionId = currentFunctionId_;
    currentFunctionId_ = function->symbolId;
    const runtime::ScopedAllocationSite fnAllocSite(function->symbolId, 0);
    stack_.push_back(function->name ? function->name : "<aot-function>");
    stackSites_.push_back({function->symbolId, 0});
    statistics_.maximumCallDepth = std::max(statistics_.maximumCallDepth, stack_.size());

    auto runBatchBody = [&]() -> bool {
        if (batchKernel != nullptr && !overrideImpl) {
            statistics_.functionCalls += isRange ? 1u : batch.count;
            return batchKernel(*this, batch);
        }
        std::vector<runtime::Value> callArgs(prefixParams + userArgCount);
        for (std::size_t u = 0; u < userArgCount; ++u) {
            callArgs[prefixParams + u] = (*batch.userArguments)[u];
        }
        if (isRange) {
            callArgs[0] = runtime::Value(static_cast<std::int64_t>(batch.start));
            callArgs[1] = runtime::Value(static_cast<std::int64_t>(batch.count));
            ++statistics_.functionCalls;
            runtime::Value retVal{};
            if (!targetImpl(*this, callArgs.data(), callArgs.size(), retVal)) {
                return false;
            }
            if (!expectSignatureType(
                    retVal,
                    function->returnType,
                    function->returnTypeId,
                    "return value")) {
                return false;
            }
            batch.reducedValue = std::move(retVal);
            batch.hasReducedValue = true;
            return true;
        }
        const runtime::ParallelReduceOp reduceOp = batch.config
            ? batch.config->reduceOp
            : runtime::ParallelReduceOp::None;
        const runtime::ParallelCustomReducer& customReducer = batch.config
            ? batch.config->customReducer
            : runtime::ParallelCustomReducer{};
        const bool strictDet = batch.config &&
            (batch.config->executionOptions.determinism.mode == runtime::DeterminismMode::Strict);
        bool hasAcc = false;
        runtime::Value accValue{std::monostate{}};
        for (std::size_t batchOff = 0u; batchOff < batch.count; ++batchOff) {
            callArgs[0] = runtime::Value(static_cast<std::int64_t>(batch.start + batchOff));
            ++statistics_.functionCalls;
            runtime::Value elemVal{};
            if (!targetImpl(*this, callArgs.data(), callArgs.size(), elemVal)) {
                return false;
            }
            if (!expectSignatureType(
                    elemVal,
                    function->returnType,
                    function->returnTypeId,
                    "return value")) {
                return false;
            }
            if (!runtime::accumulateParallelReductionValue(
                    reduceOp,
                    customReducer,
                    strictDet,
                    accValue,
                    hasAcc,
                    std::move(elemVal),
                    error_)) {
                return false;
            }
        }
        batch.hasReducedValue = hasAcc;
        batch.reducedValue = (reduceOp == runtime::ParallelReduceOp::None || !hasAcc)
            ? runtime::Value{std::monostate{}}
            : std::move(accValue);
        return true;
    };

    bool succeeded = false;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        succeeded = runBatchBody();
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
    succeeded = runBatchBody();
#endif

    if (!succeeded && hasPendingException()) {
        reportUnhandledScriptException();
    }
    if (!stack_.empty()) stack_.pop_back();
    if (!stackSites_.empty()) stackSites_.pop_back();
    currentFunctionId_ = previousFunctionId;
    batch.statistics = statistics_;
    if (!succeeded) {
        batch.error = error_;
        return false;
    }
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
        if (!receiver) {
            return fail(
                runtime::ErrorCode::InvalidObjectReference,
                "AOT virtual call receiver is invalid");
        }
        if (!checkCrossHeapRefAccess(*receiver, false, "virtual call")) {
            return false;
        }
        auto* targetHeap = resolveHeap(*receiver);
        const auto actualTypeId = targetHeap
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
        if (!receiver) {
            return fail(
                runtime::ErrorCode::InvalidObjectReference,
                "AOT interface call receiver is invalid");
        }
        if (!checkCrossHeapRefAccess(*receiver, false, "interface call")) {
            return false;
        }
        auto* targetHeap = resolveHeap(*receiver);
        const auto actualTypeId = targetHeap
            ? targetHeap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        const auto* actualType = actualTypeId
            ? findType(*actualTypeId)
            : nullptr;
        const InterfaceDispatchDescriptor* implementation = nullptr;
        const auto* curType = actualType;
        for (std::size_t depth = 0;
             curType != nullptr && depth < 256 && implementation == nullptr;
             ++depth) {
            if (curType->interfaceDispatchMaps) {
                for (std::uint32_t index = 0;
                     index < curType->interfaceDispatchMapCount;
                     ++index) {
                    const auto& candidate =
                        curType->interfaceDispatchMaps[index];
                    if (candidate.interfaceTypeId ==
                        signature.interfaceTypeId) {
                        implementation = &candidate;
                        break;
                    }
                }
            }
            curType = curType->baseTypeId != 0
                ? findType(curType->baseTypeId)
                : nullptr;
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
        std::string_view(signature.name).rfind("$intrinsic::Async.__", 0) == 0) {
        std::string intrinsicError;
        if (!runtime::tryExecuteAsyncIntrinsic(
                std::string_view(signature.name).substr(12u),
                runtime::ValueSpan(arguments, argumentCount),
                asyncRuntime_.get(),
                heap_.get(),
                result,
                intrinsicError) ||
            !intrinsicError.empty()) {
            const auto code = intrinsicError.find("null") != std::string::npos
                ? runtime::ErrorCode::NullReference
                : runtime::ErrorCode::InvalidProgram;
            return fail(code, intrinsicError);
        }
        return true;
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

namespace {

const std::shared_ptr<const std::vector<std::size_t>>& aotDelegateReferenceFields() {
    static const auto refs = std::make_shared<const std::vector<std::size_t>>(
        std::initializer_list<std::size_t>{0u, 7u});
    return refs;
}

bool readAotDelegateIdFromFields(
    const runtime::Value* fields,
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

bool isValidAotDelegateFlags(
    std::int64_t rawFlags,
    bool allowMulticast) noexcept {
    if (rawFlags < 0) return false;
    if (rawFlags == 4) return allowMulticast;
    const auto base = static_cast<std::uint64_t>(rawFlags) &
        ~static_cast<std::uint64_t>(runtime::ManagedHeap::DelegateWeakTargetFlag);
    return base <= 3u;
}

} // namespace

bool ExecutionContext::hasFunction(semantic::SymbolId symbolId) const noexcept {
    return findFunction(symbolId) != nullptr;
}

bool ExecutionContext::newDelegate(
    semantic::SymbolId delegateTypeId,
    const CallSignature& signature,
    const runtime::Value* target,
    std::size_t targetCount,
    runtime::Value& result) {
    if (RS_AOT_UNLIKELY(!heap_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT delegate creation has no heap");
    }
    if (RS_AOT_UNLIKELY(lastVerifiedDelegateTypeId_ != delegateTypeId)) {
        const auto* delegateType = findType(delegateTypeId);
        if (!delegateType || !delegateType->delegateType) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT delegate creation has invalid metadata");
        }
        lastVerifiedDelegateTypeId_ = delegateTypeId;
    }
    runtime::Value targetValue = runtime::NullObject{};
    if (targetCount == 1) {
        if (RS_AOT_UNLIKELY(!target || signature.parameterCount == 0)) {
            return fail(runtime::ErrorCode::InvalidProgram,
                "AOT delegate creation has invalid metadata");
        }
        const auto expType = signature.parameterTypes[0];
        const auto expTid = signature.parameterTypeIds ? signature.parameterTypeIds[0] : 0;
        if (RS_AOT_LIKELY(expType == semantic::PrimitiveType::Object)) {
            const auto vt = target[0].rawTag();
            if (vt == runtime::ValueTag::NullObject) {
                // Valid null object target
            } else if (RS_AOT_LIKELY(vt == runtime::ValueTag::ObjectRef)) {
                const auto actualTid = heap_->objectTypeIdFast(target[0].asObjectRefUnchecked());
                if (RS_AOT_UNLIKELY(actualTid == 0 || (expTid != 0 && actualTid != expTid && !isAssignable(actualTid, expTid)))) {
                    if (!expectSignatureType(target[0], expType, expTid, "delegate target")) return false;
                }
            } else {
                return expectSignatureType(target[0], expType, expTid, "delegate target");
            }
        } else if (!expectSignatureType(target[0], expType, expTid, "delegate target")) {
            return false;
        }
        targetValue = target[0];
    } else if (RS_AOT_UNLIKELY(targetCount > 1 || signature.virtualDispatch || signature.interfaceDispatch)) {
        if (targetCount > 1) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT delegate creation has invalid metadata");
        }
        return fail(runtime::ErrorCode::InvalidProgram, "AOT delegate dispatch target has no receiver");
    }

    const auto word = [](std::uint64_t value, bool high) noexcept {
        return static_cast<std::int64_t>(
            static_cast<std::uint32_t>(high ? (value >> 32u) : value));
    };
    std::uint64_t flags = 0;
    if (signature.virtualDispatch) flags |= 1u;
    if (signature.interfaceDispatch) flags |= 2u;
    const std::array<runtime::Value, 8> fields{{
        std::move(targetValue),
        word(signature.symbolId, false),
        word(signature.symbolId, true),
        static_cast<std::int64_t>(flags),
        static_cast<std::int64_t>(signature.virtualDispatch
            ? signature.virtualSlot
            : (signature.interfaceDispatch ? signature.interfaceSlot : 0u)),
        word(signature.interfaceTypeId, false),
        word(signature.interfaceTypeId, true),
        runtime::NullObject{},
    }};
    runtime::RuntimeError allocationError;
    std::optional<runtime::ObjectRef> reference;
    {
        ScopedTypedGcRootSpan<true> fieldsRoot(*this, fields);
        maybeStepGcBeforeAllocation();
        reference = heap_->allocateObjectFromTemplate(
            delegateTypeId,
            fields.data(),
            fields.size(),
            aotDelegateReferenceFields(),
            &allocationError);
        if (RS_AOT_UNLIKELY(!reference.has_value()) &&
            (allocationError.code == runtime::ErrorCode::None ||
             allocationError.code == runtime::ErrorCode::OutOfMemory)) {
            (void)heap_->collectYoung(shadowStack_);
            allocationError = {};
            reference = heap_->allocateObjectFromTemplate(
                delegateTypeId,
                fields.data(),
                fields.size(),
                aotDelegateReferenceFields(),
                &allocationError);
            if (!reference.has_value() &&
                (allocationError.code == runtime::ErrorCode::None ||
                 allocationError.code == runtime::ErrorCode::OutOfMemory)) {
                heap_->collectFull(shadowStack_);
                allocationError = {};
                reference = heap_->allocateObjectFromTemplate(
                    delegateTypeId,
                    fields.data(),
                    fields.size(),
                    aotDelegateReferenceFields(),
                    &allocationError);
            }
        }
    }
    if (RS_AOT_UNLIKELY(!reference.has_value())) {
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
    if (RS_AOT_UNLIKELY(!heap_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT delegate combination has no heap");
    }
    if (RS_AOT_UNLIKELY(lastVerifiedDelegateTypeId_ != delegateTypeId)) {
        const auto* delegateType = findType(delegateTypeId);
        if (!delegateType || !delegateType->delegateType) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT delegate combination has invalid metadata");
        }
        lastVerifiedDelegateTypeId_ = delegateTypeId;
    }
    const bool leftNull = (left.rawTag() == runtime::ValueTag::NullObject);
    const bool rightNull = (right.rawTag() == runtime::ValueTag::NullObject);
    runtime::ObjectFastView leftView{}, rightView{};
    std::int64_t leftFlags = -1, rightFlags = -1;

    if (!leftNull) {
        if (left.rawTag() == runtime::ValueTag::ObjectRef &&
            !checkCrossHeapRefAccess(
                left.asObjectRefUnchecked(),
                false,
                "delegate combination left operand")) {
            return false;
        }
        auto* leftHeap = left.rawTag() == runtime::ValueTag::ObjectRef
            ? resolveHeap(left.asObjectRefUnchecked())
            : nullptr;
        if (RS_AOT_UNLIKELY(left.rawTag() != runtime::ValueTag::ObjectRef ||
                            !leftHeap ||
                            !leftHeap->tryBindObjectFastView(left.asObjectRefUnchecked(), leftView) ||
                            leftView.typeId != delegateTypeId ||
                            leftView.fieldCount != 8u)) {
            return expectSignatureType(left, semantic::PrimitiveType::Object, delegateTypeId,
                "delegate combination left operand");
        }
        if (RS_AOT_UNLIKELY(!leftView.fields[3].tryGetInt64(leftFlags) ||
                            !isValidAotDelegateFlags(leftFlags, true))) {
            return fail(runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation-list metadata is corrupt");
        }
    }
    if (!rightNull) {
        if (right.rawTag() == runtime::ValueTag::ObjectRef &&
            !checkCrossHeapRefAccess(
                right.asObjectRefUnchecked(),
                false,
                "delegate combination right operand")) {
            return false;
        }
        auto* rightHeap = right.rawTag() == runtime::ValueTag::ObjectRef
            ? resolveHeap(right.asObjectRefUnchecked())
            : nullptr;
        if (RS_AOT_UNLIKELY(right.rawTag() != runtime::ValueTag::ObjectRef ||
                            !rightHeap ||
                            !rightHeap->tryBindObjectFastView(right.asObjectRefUnchecked(), rightView) ||
                            rightView.typeId != delegateTypeId ||
                            rightView.fieldCount != 8u)) {
            return expectSignatureType(right, semantic::PrimitiveType::Object, delegateTypeId,
                "delegate combination right operand");
        }
        if (RS_AOT_UNLIKELY(!rightView.fields[3].tryGetInt64(rightFlags) ||
                            !isValidAotDelegateFlags(rightFlags, true))) {
            return fail(runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation-list metadata is corrupt");
        }
    }

    const auto allocCombineNode = [&](const runtime::Value& l,
                                      const runtime::Value& r,
                                      runtime::Value& outNode) -> bool {
        const std::array<runtime::Value, 8> fields{{
            l,
            std::int64_t{0}, std::int64_t{0},
            std::int64_t{4}, std::int64_t{0},
            std::int64_t{0}, std::int64_t{0},
            r,
        }};
        runtime::RuntimeError allocationError;
        std::optional<runtime::ObjectRef> allocated;
        {
            ScopedTypedGcRootSpan<true> fieldsRoot(*this, fields);
            maybeStepGcBeforeAllocation();
            allocated = heap_->allocateObjectFromTemplate(
                delegateTypeId, fields.data(), fields.size(),
                aotDelegateReferenceFields(), &allocationError);
            if (RS_AOT_UNLIKELY(!allocated.has_value()) &&
                (allocationError.code == runtime::ErrorCode::None ||
                 allocationError.code == runtime::ErrorCode::OutOfMemory)) {
                (void)heap_->collectYoung(shadowStack_);
                allocationError = {};
                allocated = heap_->allocateObjectFromTemplate(
                    delegateTypeId, fields.data(), fields.size(),
                    aotDelegateReferenceFields(), &allocationError);
                if (!allocated.has_value() &&
                    (allocationError.code == runtime::ErrorCode::None ||
                     allocationError.code == runtime::ErrorCode::OutOfMemory)) {
                    heap_->collectFull(shadowStack_);
                    allocationError = {};
                    allocated = heap_->allocateObjectFromTemplate(
                        delegateTypeId, fields.data(), fields.size(),
                        aotDelegateReferenceFields(), &allocationError);
                }
            }
        }
        if (RS_AOT_UNLIKELY(!allocated.has_value())) {
            return fail(
                allocationError.code == runtime::ErrorCode::None
                    ? runtime::ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "AOT delegate-list allocation failed"
                    : allocationError.message);
        }
        outNode = *allocated;
        return true;
    };

    const auto flattenFast = [&](const runtime::Value& rootVal,
                                 std::vector<runtime::Value>& leaves) -> bool {
        if (rootVal.rawTag() == runtime::ValueTag::NullObject) return true;
        std::array<runtime::Value, 64> inlineStack{};
        std::size_t inlineTop = 0;
        std::vector<runtime::Value> heapStack;
        inlineStack[inlineTop++] = rootVal;
        std::size_t visited = 0;
        while (inlineTop > 0 || !heapStack.empty()) {
            runtime::Value cur;
            if (!heapStack.empty()) {
                cur = heapStack.back();
                heapStack.pop_back();
            } else {
                cur = inlineStack[--inlineTop];
            }
            if (cur.rawTag() == runtime::ValueTag::NullObject) continue;
            if (++visited > 65536u) {
                return fail(runtime::ErrorCode::InvalidProgram,
                    "AOT delegate invocation list is too deep");
            }
            if (cur.rawTag() != runtime::ValueTag::ObjectRef) {
                return fail(runtime::ErrorCode::InvalidProgram,
                    "AOT delegate invocation-list metadata is corrupt");
            }
            const auto curRef = cur.asObjectRefUnchecked();
            if (!checkCrossHeapRefAccess(curRef, false, "delegate combination")) {
                return false;
            }
            auto* curHeap = resolveHeap(curRef);
            runtime::ObjectFastView v{};
            if (!curHeap ||
                !curHeap->tryBindObjectFastView(curRef, v) ||
                v.typeId != delegateTypeId ||
                v.fieldCount != 8u) {
                return fail(runtime::ErrorCode::InvalidProgram,
                    "AOT delegate invocation-list metadata is corrupt");
            }
            std::int64_t fl = -1;
            if (!v.fields[3].tryGetInt64(fl) || !isValidAotDelegateFlags(fl, true)) {
                return fail(runtime::ErrorCode::InvalidProgram,
                    "AOT delegate invocation-list metadata is corrupt");
            }
            if (fl != 4) {
                leaves.push_back(cur);
                continue;
            }
            const runtime::Value f0 = v.fields[0];
            const runtime::Value f7 = v.fields[7];
            if (!heapStack.empty() || inlineTop + 2u > inlineStack.size()) {
                heapStack.push_back(f7);
                heapStack.push_back(f0);
            } else {
                inlineStack[inlineTop++] = f7;
                inlineStack[inlineTop++] = f0;
            }
        }
        return true;
    };

    if (!remove) {
        if (leftNull && rightNull) {
            result = runtime::NullObject{};
            return true;
        }
        if (leftNull) {
            if (rightFlags != 4) {
                result = right;
                return true;
            }
            std::vector<runtime::Value> rLeaves;
            if (!flattenFast(right, rLeaves)) return false;
            result = right;
            return true;
        }
        if (rightNull) {
            if (leftFlags != 4) {
                result = left;
                return true;
            }
            std::vector<runtime::Value> lLeaves;
            if (!flattenFast(left, lLeaves)) return false;
            result = left;
            return true;
        }
        if (leftFlags == 4) {
            const auto checkChild = [&](const runtime::Value& child) -> bool {
                if (child.rawTag() == runtime::ValueTag::NullObject) return true;
                if (child.rawTag() != runtime::ValueTag::ObjectRef) {
                    return fail(runtime::ErrorCode::InvalidProgram,
                        "AOT delegate invocation-list metadata is corrupt");
                }
                const auto childRef = child.asObjectRefUnchecked();
                if (!checkCrossHeapRefAccess(childRef, false, "delegate combination")) {
                    return false;
                }
                auto* childHeap = resolveHeap(childRef);
                runtime::ObjectFastView cv{};
                std::int64_t cf = -1;
                if (!childHeap ||
                    !childHeap->tryBindObjectFastView(childRef, cv) ||
                    cv.typeId != delegateTypeId ||
                    cv.fieldCount != 8u ||
                    !cv.fields[3].tryGetInt64(cf) ||
                    !isValidAotDelegateFlags(cf, true)) {
                    return fail(runtime::ErrorCode::InvalidProgram,
                        "AOT delegate invocation-list metadata is corrupt");
                }
                return true;
            };
            if (!checkChild(leftView.fields[0]) || !checkChild(leftView.fields[7])) {
                return false;
            }
        }
        // O(1) fast append for `+= singleLeaf`
        if (rightFlags != 4) {
            return allocCombineNode(left, right, result);
        }
        std::vector<runtime::Value> rightLeaves;
        if (!flattenFast(right, rightLeaves)) return false;
        ScopedTypedGcRootSpan<true> rightLeavesRoot(
            *this, rightLeaves.data(), rightLeaves.size());
        runtime::Value cur = left;
        ScopedTypedGcRootSpan<true> curRoot(*this, &cur, 1u);
        for (const auto& rLeaf : rightLeaves) {
            runtime::Value nextNode;
            if (!allocCombineNode(cur, rLeaf, nextNode)) return false;
            cur = nextNode;
        }
        result = cur;
        return true;
    }

    // RemoveDelegate (`-=`)
    if (leftNull) {
        result = runtime::Value{runtime::NullObject{}};
        return true;
    }
    if (rightNull) {
        if (leftFlags == 4) {
            std::vector<runtime::Value> lLeaves;
            if (!flattenFast(left, lLeaves)) return false;
        }
        result = left;
        return true;
    }
    const auto sameLeaf = [&](const runtime::Value& a, const runtime::Value& b) noexcept -> bool {
        if (a.rawTag() != runtime::ValueTag::ObjectRef || b.rawTag() != runtime::ValueTag::ObjectRef) {
            return false;
        }
        const auto aRef = a.asObjectRefUnchecked();
        const auto bRef = b.asObjectRefUnchecked();
        auto* aHeap = resolveHeap(aRef);
        auto* bHeap = resolveHeap(bRef);
        if (aRef == bRef && aHeap && aHeap->isAlive(aRef)) return true;
        runtime::ObjectFastView av{}, bv{};
        if (!aHeap || !bHeap ||
            !aHeap->tryBindObjectFastView(aRef, av) ||
            !bHeap->tryBindObjectFastView(bRef, bv) ||
            av.fieldCount < 7u || bv.fieldCount < 7u) {
            return false;
        }
        for (std::size_t index = 0; index < 7u; ++index) {
            if (!(av.fields[index] == bv.fields[index])) return false;
        }
        return true;
    };
    if (leftFlags != 4 && rightFlags != 4) {
        result = sameLeaf(left, right) ? runtime::Value{runtime::NullObject{}} : left;
        return true;
    }
    std::vector<runtime::Value> leftLeaves;
    std::vector<runtime::Value> rightLeaves;
    if (!flattenFast(left, leftLeaves) || !flattenFast(right, rightLeaves)) {
        return false;
    }
    if (!rightLeaves.empty() && rightLeaves.size() <= leftLeaves.size()) {
        std::optional<std::size_t> removeAt;
        for (std::size_t start = 0;
             start + rightLeaves.size() <= leftLeaves.size(); ++start) {
            bool matches = true;
            for (std::size_t index = 0; index < rightLeaves.size(); ++index) {
                if (!sameLeaf(leftLeaves[start + index], rightLeaves[index])) {
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
    if (leftLeaves.empty()) {
        result = runtime::NullObject{};
        return true;
    }
    ScopedTypedGcRootSpan<true> leftLeavesRoot(
        *this, leftLeaves.data(), leftLeaves.size());
    result = leftLeaves.front();
    ScopedTypedGcRootSpan<true> resultRoot(*this, &result, 1u);
    for (std::size_t index = 1; index < leftLeaves.size(); ++index) {
        runtime::Value nextNode;
        if (!allocCombineNode(result, leftLeaves[index], nextNode)) return false;
        result = nextNode;
    }
    return true;
}

int ExecutionContext::inspectDelegateSlow(
    semantic::SymbolId delegateTypeId,
    const runtime::Value& delValue,
    ResolvedAotDelegateLeaf& outSingle) {
    const auto* delegateType = findType(delegateTypeId);
    if (RS_AOT_UNLIKELY(!delegateType || !delegateType->delegateType || !heap_)) {
        (void)fail(runtime::ErrorCode::InvalidProgram,
            "AOT delegate invocation has invalid metadata");
        return 0;
    }
    if (delValue.rawTag() == runtime::ValueTag::NullObject ||
        delValue.rawTag() == runtime::ValueTag::Monostate) {
        (void)fail(runtime::ErrorCode::NullReference,
            "AOT delegate invocation attempted to dereference null");
        return 0;
    }
    if (delValue.rawTag() != runtime::ValueTag::ObjectRef) {
        (void)fail(runtime::ErrorCode::TypeMismatch,
            "AOT delegate invocation received an incompatible value");
        return 0;
    }
    const auto delRef = delValue.asObjectRefUnchecked();
    if (!checkCrossHeapRefAccess(delRef, false, "delegate invocation")) {
        return 0;
    }
    auto* delHeap = resolveHeap(delRef);
    runtime::ObjectFastView view{};
    if (!delHeap ||
        !delHeap->tryBindObjectFastView(delRef, view) ||
        view.typeId != delegateTypeId ||
        view.fieldCount != 8u) {
        (void)fail(runtime::ErrorCode::TypeMismatch,
            "AOT delegate invocation received an incompatible value");
        return 0;
    }
    std::int64_t flags = -1;
    if (!view.fields[3].tryGetInt64(flags) || !isValidAotDelegateFlags(flags, true)) {
        (void)fail(runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch metadata is corrupt");
        return 0;
    }
    if (flags == 4) {
        if (delHeap == heap_.get()) {
            auto& ce = delLeafCache_[delRef.slot & 15u];
            ce.delRef = delRef;
            ce.delegateTypeId = delegateTypeId;
            ce.isMulticast = true;
            ce.valid = true;
        }
        return 2;
    }

    const bool isWeakTarget =
        (flags & static_cast<std::int64_t>(runtime::ManagedHeap::DelegateWeakTargetFlag)) != 0;
    const std::int64_t baseFlags =
        flags & ~static_cast<std::int64_t>(runtime::ManagedHeap::DelegateWeakTargetFlag);
    std::int64_t slotVal = -1;
    semantic::SymbolId targetSymbolId = 0;
    if (baseFlags < 0 || baseFlags > 3 ||
        !view.fields[4].tryGetInt64(slotVal) ||
        slotVal < 0 || slotVal > std::numeric_limits<std::uint32_t>::max() ||
        !readAotDelegateIdFromFields(view.fields, 1u, targetSymbolId)) {
        (void)fail(runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch metadata is corrupt");
        return 0;
    }
    const bool virtualDispatch = (baseFlags & 1) != 0;
    const bool interfaceDispatch = (baseFlags & 2) != 0;
    if (virtualDispatch && interfaceDispatch) {
        (void)fail(runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch modes conflict");
        return 0;
    }
    const runtime::Value& target = view.fields[0];
    const bool hasRecv = (target.rawTag() != runtime::ValueTag::NullObject);
    if (isWeakTarget) {
        if (target.rawTag() != runtime::ValueTag::ObjectRef) {
            return 3;
        }
        const auto targetRef = target.asObjectRefUnchecked();
        auto* weakRecvHeap = resolveHeap(targetRef);
        if (!weakRecvHeap || !weakRecvHeap->isAlive(targetRef)) {
            return 3;
        }
    }
    if (virtualDispatch || interfaceDispatch) {
        semantic::SymbolId recvTid = 0;
        if (hasRecv && target.rawTag() == runtime::ValueTag::ObjectRef) {
            const auto targetRef = target.asObjectRefUnchecked();
            if (!checkCrossHeapRefAccess(targetRef, false, "delegate invocation")) {
                return 0;
            }
            auto* recvHeap = resolveHeap(targetRef);
            recvTid = recvHeap ? recvHeap->objectTypeIdFast(targetRef) : 0;
        }
        const auto* runtimeType = recvTid != 0 ? findType(recvTid) : nullptr;
        if (!runtimeType) {
            (void)fail(runtime::ErrorCode::InvalidObjectReference,
                "AOT delegate receiver has no runtime type");
            return 0;
        }
        const auto dispatchSlot = static_cast<std::uint32_t>(slotVal);
        if (virtualDispatch) {
            if (!runtimeType->virtualDispatchTable ||
                dispatchSlot >= runtimeType->virtualSlotCount) {
                (void)fail(runtime::ErrorCode::InvalidProgram,
                    "AOT delegate virtual slot is invalid");
                return 0;
            }
            targetSymbolId = runtimeType->virtualDispatchTable[dispatchSlot];
        } else {
            semantic::SymbolId interfaceTypeId = 0;
            if (!readAotDelegateIdFromFields(view.fields, 5u, interfaceTypeId)) {
                (void)fail(runtime::ErrorCode::InvalidProgram,
                    "AOT delegate interface identity is corrupt");
                return 0;
            }
            const InterfaceDispatchDescriptor* map = nullptr;
            const auto* curType = runtimeType;
            for (std::size_t depth = 0; curType != nullptr && depth < 256 && map == nullptr; ++depth) {
                if (curType->interfaceDispatchMaps) {
                    for (std::uint32_t idx = 0; idx < curType->interfaceDispatchMapCount; ++idx) {
                        if (curType->interfaceDispatchMaps[idx].interfaceTypeId == interfaceTypeId) {
                            map = &curType->interfaceDispatchMaps[idx];
                            break;
                        }
                    }
                }
                curType = curType->baseTypeId != 0 ? findType(curType->baseTypeId) : nullptr;
            }
            if (!map || !map->slots || dispatchSlot >= map->slotCount) {
                (void)fail(runtime::ErrorCode::InvalidProgram,
                    "AOT delegate interface slot is invalid");
                return 0;
            }
            targetSymbolId = map->slots[dispatchSlot];
        }
    }
    outSingle.targetSymbolId = targetSymbolId;
    outSingle.receiver = target;
    outSingle.hasReceiver = hasRecv;

    const bool hasObjRecv = (hasRecv && target.rawTag() == runtime::ValueTag::ObjectRef);
    const bool recvLocal = !hasObjRecv || resolveHeap(target.asObjectRefUnchecked()) == heap_.get();
    if (!isWeakTarget && delHeap == heap_.get() && recvLocal) {
        auto& ce = delLeafCache_[delRef.slot & 15u];
        ce.delRef = delRef;
        ce.hasObjectReceiver = hasObjRecv;
        ce.recvRef = hasObjRecv ? target.asObjectRefUnchecked() : runtime::ObjectRef{};
        ce.delegateTypeId = delegateTypeId;
        ce.resolved = outSingle;
        ce.isMulticast = false;
        ce.valid = true;
    }
    return 1;
}

const ResolvedAotDelegateLeaf* ExecutionContext::acquireMulticastLeavesFast(
    semantic::SymbolId delegateTypeId,
    const runtime::Value& rootValue,
    std::size_t& outCount,
    std::vector<ResolvedAotDelegateLeaf>& fallbackBuf) {
    static const ResolvedAotDelegateLeaf kEmptyMulticastLeaf{};
    outCount = 0;
    if (RS_AOT_UNLIKELY(!heap_ || rootValue.rawTag() != runtime::ValueTag::ObjectRef)) {
        (void)fail(runtime::ErrorCode::InvalidProgram, "AOT delegate invocation list is corrupt");
        return nullptr;
    }
    const auto rootRef = rootValue.asObjectRefUnchecked();
    if (!checkCrossHeapRefAccess(rootRef, false, "delegate invocation")) {
        return nullptr;
    }
    auto* rootHeap = resolveHeap(rootRef);
    if (!rootHeap) {
        (void)fail(runtime::ErrorCode::InvalidProgram, "AOT delegate invocation list is corrupt");
        return nullptr;
    }
    if (RS_AOT_LIKELY(rootHeap == heap_.get() &&
                      cachedMulticastTypeId_ == delegateTypeId &&
                      cachedMulticastRef_ == rootRef &&
                      !cachedMulticastLeaves_.empty() &&
                      heap_->objectTypeIdFast(rootRef) == delegateTypeId)) {
        bool allAlive = true;
        for (const auto& item : cachedMulticastMeta_) {
            if (!heap_->isAlive(item.leafRef) ||
                (item.hasObjRecv && !heap_->isAlive(item.recvRef))) {
                allAlive = false;
                break;
            }
        }
        if (RS_AOT_LIKELY(allAlive)) {
            ++cachedMulticastPinCount_;
            outCount = cachedMulticastLeaves_.size();
            return cachedMulticastLeaves_.data();
        }
    }

    bool sawWeakOrCrossHeap = (rootHeap != heap_.get());
    bool canPopulateCache = (cachedMulticastPinCount_ == 0 && !sawWeakOrCrossHeap);
    if (canPopulateCache) {
        cachedMulticastMeta_.clear();
        cachedMulticastLeaves_.clear();
        cachedMulticastTypeId_ = 0;
    } else {
        fallbackBuf.clear();
    }
    const auto disableCache = [&]() {
        sawWeakOrCrossHeap = true;
        if (canPopulateCache) {
            fallbackBuf = std::move(cachedMulticastLeaves_);
            cachedMulticastMeta_.clear();
            cachedMulticastLeaves_.clear();
            cachedMulticastTypeId_ = 0;
            canPopulateCache = false;
        }
    };

    std::array<runtime::Value, 64> inlineStack{};
    std::size_t inlineTop = 0;
    std::vector<runtime::Value> nodeStack;
    inlineStack[inlineTop++] = rootValue;
    std::size_t visited = 0;

    while (inlineTop > 0 || !nodeStack.empty()) {
        runtime::Value cur;
        if (!nodeStack.empty()) {
            cur = nodeStack.back();
            nodeStack.pop_back();
        } else {
            cur = inlineStack[--inlineTop];
        }
        if (cur.rawTag() == runtime::ValueTag::NullObject) continue;
        if (++visited > 65536u) {
            (void)fail(runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation list is corrupt");
            return nullptr;
        }
        if (cur.rawTag() != runtime::ValueTag::ObjectRef) {
            (void)fail(runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation list is corrupt");
            return nullptr;
        }
        const auto curRef = cur.asObjectRefUnchecked();
        if (!checkCrossHeapRefAccess(curRef, false, "delegate invocation")) {
            return nullptr;
        }
        auto* curHeap = resolveHeap(curRef);
        runtime::ObjectFastView v{};
        std::int64_t fl = -1;
        if (!curHeap ||
            !curHeap->tryBindObjectFastView(curRef, v) ||
            v.typeId != delegateTypeId ||
            v.fieldCount != 8u ||
            !v.fields[3].tryGetInt64(fl) ||
            !isValidAotDelegateFlags(fl, true)) {
            (void)fail(runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation list is corrupt");
            return nullptr;
        }
        if (curHeap != heap_.get()) {
            disableCache();
        }
        if (fl != 4) {
            ResolvedAotDelegateLeaf resolved{};
            if (RS_AOT_LIKELY(fl == 0)) {
                std::int64_t lo = -1, hi = -1, slotVal = -1;
                if (RS_AOT_UNLIKELY(
                        !v.fields[1].tryGetInt64(lo) ||
                        !v.fields[2].tryGetInt64(hi) ||
                        !v.fields[4].tryGetInt64(slotVal) ||
                        lo < 0 || hi < 0 || slotVal < 0 ||
                        lo > std::numeric_limits<std::uint32_t>::max() ||
                        hi > std::numeric_limits<std::uint32_t>::max() ||
                        slotVal > std::numeric_limits<std::uint32_t>::max())) {
                    (void)fail(runtime::ErrorCode::InvalidProgram,
                        "AOT delegate dispatch metadata is corrupt");
                    return nullptr;
                }
                resolved.targetSymbolId =
                    static_cast<std::uint64_t>(static_cast<std::uint32_t>(lo)) |
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32u);
                resolved.receiver = v.fields[0];
                resolved.hasReceiver = (resolved.receiver.rawTag() != runtime::ValueTag::NullObject);
            } else {
                const int status = inspectDelegateSlow(delegateTypeId, cur, resolved);
                if (status == 0) {
                    return nullptr;
                }
                if (status == 3) {
                    disableCache();
                    continue;
                }
            }
            const bool isWeakLeaf =
                (fl & static_cast<std::int64_t>(runtime::ManagedHeap::DelegateWeakTargetFlag)) != 0;
            const bool hasObjR =
                resolved.hasReceiver && resolved.receiver.rawTag() == runtime::ValueTag::ObjectRef;
            if (isWeakLeaf ||
                (hasObjR && resolveHeap(resolved.receiver.asObjectRefUnchecked()) != heap_.get())) {
                disableCache();
            }
            if (canPopulateCache) {
                cachedMulticastMeta_.push_back({
                    resolved,
                    curRef,
                    hasObjR ? resolved.receiver.asObjectRefUnchecked() : runtime::ObjectRef{},
                    hasObjR,
                });
                cachedMulticastLeaves_.push_back(std::move(resolved));
            } else {
                fallbackBuf.push_back(std::move(resolved));
            }
            continue;
        }
        const runtime::Value f0 = v.fields[0];
        const runtime::Value f7 = v.fields[7];
        if (!nodeStack.empty() || inlineTop + 2u > inlineStack.size()) {
            nodeStack.push_back(f7);
            nodeStack.push_back(f0);
        } else {
            inlineStack[inlineTop++] = f7;
            inlineStack[inlineTop++] = f0;
        }
    }

    if (canPopulateCache) {
        if (cachedMulticastLeaves_.empty()) {
            outCount = 0;
            return &kEmptyMulticastLeaf;
        }
        cachedMulticastTypeId_ = delegateTypeId;
        cachedMulticastRef_ = rootRef;
        ++cachedMulticastPinCount_;
        outCount = cachedMulticastLeaves_.size();
        return cachedMulticastLeaves_.data();
    }
    outCount = fallbackBuf.size();
    return fallbackBuf.empty() ? &kEmptyMulticastLeaf : fallbackBuf.data();
}

bool ExecutionContext::invokeDelegate(
    semantic::SymbolId delegateTypeId,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    runtime::Value& result) {
    return invokeDelegate(
        delegateTypeId,
        arguments,
        argumentCount,
        semantic::PrimitiveType::Error,
        0,
        result);
}

bool ExecutionContext::invokeDelegate(
    semantic::SymbolId delegateTypeId,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    semantic::PrimitiveType expectedReturnType,
    semantic::SymbolId expectedReturnTypeId,
    runtime::Value& result) {
    runtime::ScopedValuePoolDomain domainScope(
        heap_ ? heap_->valuePoolDomain().get() : nullptr);
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
    if (!delegateRef) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "AOT delegate invocation received an incompatible value");
    }
    if (!checkCrossHeapRefAccess(*delegateRef, false, "delegate invocation")) {
        return false;
    }
    auto* delegateHeap = resolveHeap(*delegateRef);
    const auto actualType = delegateHeap
        ? delegateHeap->objectTypeId(*delegateRef)
        : std::optional<semantic::SymbolId>{};
    if (!actualType || *actualType != delegateTypeId ||
        delegateHeap->fieldCount(*delegateRef) != 8u) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "AOT delegate invocation received an incompatible value");
    }
    const auto computeDeadReturn = [&](semantic::SymbolId fallbackFnId) -> runtime::Value {
        if (expectedReturnType == semantic::PrimitiveType::Void ||
            expectedReturnType == semantic::PrimitiveType::Null) {
            return std::monostate{};
        }
        if (expectedReturnType != semantic::PrimitiveType::Error) {
            return defaultValue(expectedReturnType, expectedReturnTypeId);
        }
        if (fallbackFnId != 0) {
            if (const auto* fn = findFunction(fallbackFnId)) {
                if (fn->returnType == semantic::PrimitiveType::Void ||
                    fn->returnType == semantic::PrimitiveType::Null) {
                    return std::monostate{};
                }
                return defaultValue(fn->returnType, fn->returnTypeId);
            }
        }
        return std::monostate{};
    };
    const auto rootFlagsValue = delegateHeap->fieldGet(*delegateRef, 3u);
    const auto* rootFlags = rootFlagsValue
        ? std::get_if<std::int64_t>(&*rootFlagsValue)
        : nullptr;
    if (rootFlags && *rootFlags == 4) {
        std::vector<runtime::Value> leaves;
        const auto flatten = [&](const auto& self,
                                 const runtime::Value& value,
                                 std::size_t depth) -> bool {
            if (depth > 1024) return false;
            const auto* reference =
                std::get_if<runtime::ObjectRef>(&value);
            if (!reference) return false;
            if (!checkCrossHeapRefAccess(*reference, false, "delegate invocation")) {
                return false;
            }
            auto* curHeap = resolveHeap(*reference);
            const auto flagsValue = curHeap
                ? curHeap->fieldGet(*reference, 3u)
                : std::optional<runtime::Value>{};
            const auto* flags = flagsValue
                ? std::get_if<std::int64_t>(&*flagsValue)
                : nullptr;
            if (!flags || !isValidAotDelegateFlags(*flags, true)) return false;
            if (*flags != 4) {
                leaves.push_back(value);
                return true;
            }
            const auto first = curHeap->fieldGet(*reference, 0u);
            const auto second = curHeap->fieldGet(*reference, 7u);
            return first && second &&
                self(self, *first, depth + 1) &&
                self(self, *second, depth + 1);
        };
        if (!flatten(flatten, arguments[0], 0)) {
            return error_.code != runtime::ErrorCode::None
                ? false
                : fail(
                    runtime::ErrorCode::InvalidProgram,
                    "AOT delegate invocation list is corrupt");
        }
        runtime::Value lastResult = computeDeadReturn(0);
        std::vector<runtime::Value> nested(
            arguments, arguments + argumentCount);
        for (const auto& leaf : leaves) {
            if (const auto* leafRef = std::get_if<runtime::ObjectRef>(&leaf)) {
                if (!checkCrossHeapRefAccess(*leafRef, false, "delegate invocation")) {
                    return false;
                }
                if (auto* leafHeap = resolveHeap(*leafRef)) {
                    const auto leafFlagsVal = leafHeap->fieldGet(*leafRef, 3u);
                    const auto leafTargetVal = leafHeap->fieldGet(*leafRef, 0u);
                    if (leafFlagsVal && leafTargetVal) {
                        if (const auto* lf = std::get_if<std::int64_t>(&*leafFlagsVal);
                            lf && (*lf & static_cast<std::int64_t>(runtime::ManagedHeap::DelegateWeakTargetFlag)) != 0) {
                            const auto* tRef = std::get_if<runtime::ObjectRef>(&*leafTargetVal);
                            auto* tHeap = tRef ? resolveHeap(*tRef) : nullptr;
                            if (!tRef || !tHeap || !tHeap->isAlive(*tRef)) {
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
                    delegateTypeId,
                    nested.data(),
                    nested.size(),
                    expectedReturnType,
                    expectedReturnTypeId,
                    leafResult)) {
                return false;
            }
            lastResult = std::move(leafResult);
        }
        result = std::move(lastResult);
        return true;
    }
    const auto readId = [&](std::size_t lowIndex,
                            semantic::SymbolId& value) {
        const auto low = delegateHeap->fieldGet(*delegateRef, lowIndex);
        const auto high = delegateHeap->fieldGet(*delegateRef, lowIndex + 1u);
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
    semantic::SymbolId targetSymbolId = 0;
    if (!readId(1u, targetSymbolId)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate callable identity is corrupt");
    }
    const auto target = delegateHeap->fieldGet(*delegateRef, 0u);
    const auto flagsValue = delegateHeap->fieldGet(*delegateRef, 3u);
    const auto slotValue = delegateHeap->fieldGet(*delegateRef, 4u);
    const auto* flags = flagsValue
        ? std::get_if<std::int64_t>(&*flagsValue)
        : nullptr;
    const auto* slot = slotValue
        ? std::get_if<std::int64_t>(&*slotValue)
        : nullptr;
    if (!target || !flags || !slot ||
        !isValidAotDelegateFlags(*flags, false) ||
        *slot < 0 ||
        *slot > std::numeric_limits<std::uint32_t>::max()) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch metadata is corrupt");
    }
    if ((*flags & static_cast<std::int64_t>(runtime::ManagedHeap::DelegateWeakTargetFlag)) != 0) {
        const auto* weakTargetRef = std::get_if<runtime::ObjectRef>(&*target);
        auto* weakTargetHeap = weakTargetRef ? resolveHeap(*weakTargetRef) : nullptr;
        if (!weakTargetRef || !weakTargetHeap || !weakTargetHeap->isAlive(*weakTargetRef)) {
            result = computeDeadReturn(targetSymbolId);
            return true;
        }
    }
    std::vector<runtime::Value> values;
    if (argumentCount > 1) {
        values.assign(arguments + 1, arguments + argumentCount);
    }
    if (!std::holds_alternative<runtime::NullObject>(*target)) {
        values.insert(values.begin(), *target);
    }
    const bool virtualDispatch = (*flags & 1) != 0;
    const bool interfaceDispatch = (*flags & 2) != 0;
    if (virtualDispatch && interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch modes conflict");
    }
    if (virtualDispatch || interfaceDispatch) {
        const auto* receiver = values.empty()
            ? nullptr
            : std::get_if<runtime::ObjectRef>(&values.front());
        if (receiver && !checkCrossHeapRefAccess(*receiver, false, "delegate invocation")) {
            return false;
        }
        auto* recvHeap = receiver ? resolveHeap(*receiver) : heap_.get();
        const auto runtimeTypeId = receiver && recvHeap
            ? recvHeap->objectTypeId(*receiver)
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
            const auto* curType = runtimeType;
            for (std::size_t depth = 0; curType != nullptr && depth < 256 && map == nullptr; ++depth) {
                if (curType->interfaceDispatchMaps) {
                    for (std::uint32_t index = 0;
                         index < curType->interfaceDispatchMapCount;
                         ++index) {
                        const auto& candidate =
                            curType->interfaceDispatchMaps[index];
                        if (candidate.interfaceTypeId == interfaceTypeId) {
                            map = &candidate;
                            break;
                        }
                    }
                }
                curType = curType->baseTypeId != 0 ? findType(curType->baseTypeId) : nullptr;
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

void ExecutionContext::maybeStepGcBeforeAllocation() {
    if (!heap_ || options_.limits.gcWorkBudget == 0 || options_.trustedRelease) {
        return;
    }
    if (heap_->phase() == runtime::GcPhase::Idle && !heap_->collectionRequested()) {
        return;
    }
    const auto gcSliceStart = telemetry_
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    const auto work = heap_->step(shadowStack_, options_.limits.gcWorkBudget);
    statistics_.gcWorkPerformed += work;
    if (work != 0 && traceEventsEnabled_) {
        if (diagnosticEventsEnabled_) {
            const auto workText = std::to_string(work);
            emitTrace(runtime::TraceEventKind::GcStep, work, workText);
        } else {
            emitTrace(runtime::TraceEventKind::GcStep, work);
        }
    }
    if (telemetry_ != nullptr) {
        const auto elapsedNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - gcSliceStart).count());
        telemetry_->recordGcSliceNanoseconds(elapsedNs);
    }
}

bool ExecutionContext::constantString(
    std::string_view value,
    runtime::Value& result) {
    if (RS_AOT_UNLIKELY(!heap_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "managed string allocation has no heap");
    }
    maybeStepGcBeforeAllocation();
    runtime::RuntimeError allocationError;
    auto reference = heap_->internString(value, &allocationError);
    if (RS_AOT_UNLIKELY(!reference) &&
        (allocationError.code == runtime::ErrorCode::None ||
         allocationError.code == runtime::ErrorCode::OutOfMemory)) {
        (void)heap_->collectYoung(shadowStack_);
        allocationError = {};
        reference = heap_->internString(value, &allocationError);
        if (!reference &&
            (allocationError.code == runtime::ErrorCode::None ||
             allocationError.code == runtime::ErrorCode::OutOfMemory)) {
            heap_->collectFull(shadowStack_);
            allocationError = {};
            reference = heap_->internString(value, &allocationError);
        }
    }
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

bool ExecutionContext::constantStringIndexed(
    std::uint32_t literalSlot,
    std::string_view value,
    runtime::Value& result) {
    if (RS_AOT_LIKELY(heap_ != nullptr)) {
        const std::uint64_t heapId = static_cast<std::uint64_t>(heap_->heapId());
        const std::uint64_t epoch = heap_->stringInternEpoch();
        if (RS_AOT_UNLIKELY(heapId != stringLiteralCacheHeapId_ ||
                            epoch != stringLiteralCacheEpoch_)) {
            stringLiteralCache_.clear();
            stringLiteralCacheHeapId_ = heapId;
            stringLiteralCacheEpoch_ = epoch;
        }
        if (RS_AOT_LIKELY(heap_->phase() == runtime::GcPhase::Idle &&
                          literalSlot < stringLiteralCache_.size())) {
            const auto& entry = stringLiteralCache_[literalSlot];
            if (entry.ref.valid() &&
                entry.data == value.data() &&
                entry.size == value.size()) {
                result = entry.ref;
                return true;
            }
        }
    }
    if (!constantString(value, result)) {
        return false;
    }
    if (heap_ != nullptr &&
        result.rawTag() == runtime::ValueTag::ObjectRef &&
        literalSlot < 1048576u) {
        const std::uint64_t heapId = static_cast<std::uint64_t>(heap_->heapId());
        const std::uint64_t epoch = heap_->stringInternEpoch();
        if (heapId != stringLiteralCacheHeapId_ || epoch != stringLiteralCacheEpoch_) {
            stringLiteralCache_.clear();
            stringLiteralCacheHeapId_ = heapId;
            stringLiteralCacheEpoch_ = epoch;
        }
        if (literalSlot >= stringLiteralCache_.size()) {
            stringLiteralCache_.resize(static_cast<std::size_t>(literalSlot) + 1u);
        }
        auto& entry = stringLiteralCache_[literalSlot];
        if (!entry.ref.valid() ||
            (entry.data == value.data() && entry.size == value.size())) {
            entry.ref = result.u_.objectRef;
            entry.data = value.data();
            entry.size = value.size();
        }
    }
    return true;
}

bool ExecutionContext::newStackRefObject(
    semantic::SymbolId typeId,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Class ||
        type->interfaceType || type->delegateType) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT object allocation references an invalid class descriptor");
    }
    if (activeStackRefScopes_ > 0 &&
        stackRefBankId_ != 0u &&
        type->fieldCount == 1 &&
        type->fields != nullptr &&
        type->name != nullptr &&
        runtime::isSynthesizedRefWrapperTypeName(type->name)) {
        const auto& fld = type->fields[0];
        if (fld.type == semantic::PrimitiveType::Struct && fld.typeId != 0) {
            if (const auto* sc = runtime::findFlatStructSchemaByTypeId(fld.typeId)) {
                const std::uint32_t baseOff = stackRefTop_;
                const std::size_t need = static_cast<std::size_t>(baseOff) + sc->flatSlotCount;
                if (need > runtime::StructStorageHandle::FrameOffsetMask) {
                    return fail(
                        runtime::ErrorCode::OutOfMemory,
                        "stack-ref slot capacity exceeded");
                }
                if (need > stackRefSlots_.size()) {
                    if (need > stackRefSlots_.capacity()) {
                        stackRefSlots_.reserve(std::max<std::size_t>(need, stackRefSlots_.capacity() * 2u + 64u));
                    }
                    stackRefSlots_.resize(need);
                }
                std::memcpy(
                    static_cast<void*>(stackRefSlots_.data() + baseOff),
                    static_cast<const void*>(sc->prototypeLeaves.data()),
                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                stackRefTop_ = static_cast<std::uint32_t>(need);
                result = runtime::ObjectRef::makeStackSlotRef(baseOff, stackRefBankId_, sc->schemaId);
                return true;
            }
        }
        const std::uint32_t slotOff = stackRefTop_;
        const std::size_t need = static_cast<std::size_t>(slotOff) + 1u;
        if (need > runtime::StructStorageHandle::FrameOffsetMask) {
            return fail(
                runtime::ErrorCode::OutOfMemory,
                "stack-ref slot capacity exceeded");
        }
        if (need > stackRefSlots_.size()) {
            if (need > stackRefSlots_.capacity()) {
                stackRefSlots_.reserve(std::max<std::size_t>(need, stackRefSlots_.capacity() * 2u + 64u));
            }
            stackRefSlots_.resize(need);
        }
        stackRefSlots_[slotOff] = defaultValue(fld.type, fld.typeId);
        stackRefTop_ = static_cast<std::uint32_t>(need);
        result = runtime::ObjectRef::makeStackSlotRef(slotOff, stackRefBankId_, 0u);
        return true;
    }
    return newObject(typeId, result);
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
    const auto buildObjectFields = [&](std::vector<runtime::Value>& outFields,
                                       std::vector<std::size_t>& outRefs) {
        outFields.clear();
        outRefs.clear();
        outFields.reserve(type->fieldCount);
        for (std::uint32_t index = 0; index < type->fieldCount; ++index) {
            const auto& field = type->fields[index];
            outFields.push_back(defaultValue(field.type, field.typeId));
            if (semantic::isReferenceType(field.type) ||
                (field.type == semantic::PrimitiveType::Struct &&
                 !runtime::makeDefaultInlineStructValue(field.typeId).has_value())) {
                outRefs.push_back(index);
            }
        }
    };
    maybeStepGcBeforeAllocation();
    std::vector<runtime::Value> fields;
    std::vector<std::size_t> references;
    buildObjectFields(fields, references);
    runtime::RuntimeError allocationError;
    auto reference = heap_->allocateObject(
        typeId,
        std::move(fields),
        std::move(references),
        &allocationError);
    if (RS_AOT_UNLIKELY(!reference) &&
        (allocationError.code == runtime::ErrorCode::None ||
         allocationError.code == runtime::ErrorCode::OutOfMemory)) {
        (void)heap_->collectYoung(shadowStack_);
        buildObjectFields(fields, references);
        allocationError = {};
        reference = heap_->allocateObject(
            typeId,
            std::move(fields),
            std::move(references),
            &allocationError);
        if (!reference &&
            (allocationError.code == runtime::ErrorCode::None ||
             allocationError.code == runtime::ErrorCode::OutOfMemory)) {
            heap_->collectFull(shadowStack_);
            buildObjectFields(fields, references);
            allocationError = {};
            reference = heap_->allocateObject(
                typeId,
                std::move(fields),
                std::move(references),
                &allocationError);
        }
    }
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
    if (const auto inl = runtime::makeDefaultInlineStructValue(typeId); inl.has_value()) {
        result = *inl;
        return true;
    }
    constexpr std::size_t kMaxTransientStackSliceSlots = 65536u;
    if (activeStackRefScopes_ > 0 && stackRefBankId_ != 0u) {
        if (const auto* sc = runtime::findFlatStructSchemaByTypeId(typeId)) {
            const std::uint32_t baseOff = stackRefTop_;
            const std::size_t need = static_cast<std::size_t>(baseOff) + sc->flatSlotCount;
            if (need <= kMaxTransientStackSliceSlots &&
                need <= runtime::StructStorageHandle::FrameOffsetMask) {
                if (need > stackRefSlots_.size()) {
                    stackRefSlots_.resize(std::max<std::size_t>(need, stackRefSlots_.capacity() * 2u + 64u));
                }
                std::memcpy(
                    static_cast<void*>(stackRefSlots_.data() + baseOff),
                    static_cast<const void*>(sc->prototypeLeaves.data()),
                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                stackRefTop_ = static_cast<std::uint32_t>(need);
                result = runtime::StructValue::makeFrameSlice(typeId, stackRefBankId_, baseOff, sc->schemaId);
                return true;
            }
        }
    }
    result = defaultValue(semantic::PrimitiveType::Struct, typeId);
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
    runtime::Value initElemVal =
        (elementType == semantic::PrimitiveType::Struct &&
         elementTypeId != 0 &&
         (runtime::isBlittableStructType(elementTypeId) ||
          runtime::findFlatStructSchemaByTypeId(elementTypeId) != nullptr))
        ? runtime::Value{}
        : defaultValue(elementType, elementTypeId);
    maybeStepGcBeforeAllocation();
    runtime::RuntimeError allocationError;
    auto reference = heap_->allocateTypedArray(
        arrayTypeId,
        elementType,
        elementTypeId,
        static_cast<std::size_t>(size),
        initElemVal,
        &allocationError);
    if (RS_AOT_UNLIKELY(!reference) &&
        (allocationError.code == runtime::ErrorCode::None ||
         allocationError.code == runtime::ErrorCode::OutOfMemory)) {
        (void)heap_->collectYoung(shadowStack_);
        allocationError = {};
        reference = heap_->allocateTypedArray(
            arrayTypeId,
            elementType,
            elementTypeId,
            static_cast<std::size_t>(size),
            initElemVal,
            &allocationError);
        if (!reference &&
            (allocationError.code == runtime::ErrorCode::None ||
             allocationError.code == runtime::ErrorCode::OutOfMemory)) {
            heap_->collectFull(shadowStack_);
            allocationError = {};
            reference = heap_->allocateTypedArray(
                arrayTypeId,
                elementType,
                elementTypeId,
                static_cast<std::size_t>(size),
                initElemVal,
                &allocationError);
        }
    }
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "managed array allocation failed"
                : allocationError.message);
    }
    if (telemetry_ != nullptr && heap_->isPackedArray(*reference)) {
        telemetry_->noteCompactHeapObject(*reference);
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
            if (const auto* reference = std::get_if<runtime::ObjectRef>(&value)) {
                if (!checkCrossHeapRefAccess(*reference, false, "runtime type check")) {
                    return false;
                }
                auto* targetHeap = resolveHeap(*reference);
                const auto actual = targetHeap
                    ? targetHeap->objectTypeId(*reference)
                    : std::optional<semantic::SymbolId>{};
                matches = actual && isAssignable(*actual, targetTypeId);
            }
        } else if (targetType == semantic::PrimitiveType::Array) {
            if (const auto* reference = std::get_if<runtime::ObjectRef>(&value)) {
                if (!checkCrossHeapRefAccess(*reference, false, "runtime type check")) {
                    return false;
                }
                auto* targetHeap = resolveHeap(*reference);
                const auto actual = targetHeap
                    ? targetHeap->arrayTypeId(*reference)
                    : std::optional<semantic::SymbolId>{};
                matches = actual && *actual == targetTypeId;
            }
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
        if (!checkCrossHeapRefAccess(*reference, false, "array length")) {
            return false;
        }
        if (auto* targetHeap = resolveHeap(*reference)) {
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
    const auto length = targetHeap ? targetHeap->arrayLength(reference) : std::optional<std::size_t>{};
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
    if (targetHeap == heap_.get() &&
        elementType == semantic::PrimitiveType::Struct &&
        activeStackRefScopes_ > 0 &&
        stackRefBankId_ != 0u) {
        constexpr std::size_t kMaxTransientStackSliceSlots = 65536u;
        if (const auto* sc = runtime::findFlatStructSchemaByTypeId(elementTypeId)) {
            runtime::FlatStructArrayFastView fView{};
            if ((targetHeap->tryBindFlatStructArrayFastView(reference, fView) && fView.elementTypeId == elementTypeId && fView.flatStride == sc->flatSlotCount) &&
                fView.flatSchemaId == sc->schemaId) {
                const std::uint32_t baseOff = stackRefTop_;
                const std::size_t need = static_cast<std::size_t>(baseOff) + sc->flatSlotCount;
                if (need <= kMaxTransientStackSliceSlots &&
                    need <= runtime::StructStorageHandle::FrameOffsetMask) {
                    if (need > stackRefSlots_.size()) {
                        stackRefSlots_.resize(std::max<std::size_t>(need, stackRefSlots_.capacity() * 2u + 64u));
                    }
                    const runtime::Value* src =
                        fView.data + static_cast<std::size_t>(numericIndex) * fView.flatStride;
                    std::memcpy(
                        static_cast<void*>(stackRefSlots_.data() + baseOff),
                        static_cast<const void*>(src),
                        static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                    stackRefTop_ = static_cast<std::uint32_t>(need);
                    result = runtime::StructValue::makeFrameSlice(
                        elementTypeId, stackRefBankId_, baseOff, sc->schemaId);
                    return true;
                }
            }
        }
    }
    const auto value = targetHeap->arrayGet(
        reference,
        static_cast<std::size_t>(numericIndex));
    if (!value) {
        return false;
    }
    runtime::Value loaded = sanitizeCrossHeapValue(targetHeap, *value);
    if (!expectSignatureType(
            loaded,
            elementType,
            elementTypeId,
            "array element load value")) {
        return false;
    }
    result = std::move(loaded);
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
    if (!checkCrossHeapRefAccess(reference, executionPolicy_ == nullptr, "array element store")) {
        return false;
    }
    auto* targetHeap = resolveHeap(reference);
    const auto length = targetHeap ? targetHeap->arrayLength(reference) : std::optional<std::size_t>{};
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
    const auto actualType = targetHeap->arrayElementType(reference);
    const auto actualTypeId = targetHeap->arrayElementTypeId(reference);
    if (!actualType || !actualTypeId ||
        *actualType != elementType || *actualTypeId != elementTypeId) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "array element metadata does not match the AOT operation");
    }
    runtime::RuntimeError arrayError;
    if (executionPolicy_ &&
        !executionPolicy_->checkArrayWrite(
            heap_.get(),
            targetHeap,
            reference,
            value,
            arrayError)) {
        error_ = std::move(arrayError);
        if (error_.code == runtime::ErrorCode::None) {
            error_.code = runtime::ErrorCode::CapabilityDenied;
        }
        if (error_.message.empty()) {
            error_.message = "array write rejected by execution policy";
        }
        populateErrorStackTrace();
        return false;
    }
    if (!checkCrossHeapRefAccess(reference, true, "array element store")) {
        return false;
    }
    if (!targetHeap->arraySet(
            reference,
            static_cast<std::size_t>(numericIndex),
            value,
            &arrayError,
            elementType == semantic::PrimitiveType::Object
                ? runtime::ArrayStoreTypePolicy::PrevalidatedAssignableObject
                : runtime::ArrayStoreTypePolicy::Exact)) {
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
    const auto recvRef = std::get<runtime::ObjectRef>(checked);
    if (recvRef.isStackSlotRef()) {
        const auto& field = type->fields[fieldIndex];
        runtime::Value sVal{};
        if (recvRef.stackRefSchemaId() == 0u) {
            sVal = stackRefSlots_[recvRef.slot];
        } else {
            sVal = runtime::StructValue::makeFrameSlice(
                field.typeId, stackRefBankId_, recvRef.slot, recvRef.stackRefSchemaId());
        }
        if (!expectSignatureType(sVal, field.type, field.typeId, "field load value")) {
            return false;
        }
        result = sVal;
        return true;
    }
    auto* targetHeap = resolveHeap(recvRef);
    const auto value = targetHeap
        ? targetHeap->fieldGet(recvRef, fieldIndex)
        : std::optional<runtime::Value>{};
    if (!value) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "field load references an invalid field slot");
    }
    const auto& field = type->fields[fieldIndex];
    runtime::Value loaded = sanitizeCrossHeapValue(targetHeap, *value);
    if (!expectSignatureType(
            loaded,
            field.type,
            field.typeId,
            "field load value")) {
        return false;
    }
    result = std::move(loaded);
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
    const auto& field = type->fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "field store value")) {
        return false;
    }
    const auto recvRef = std::get<runtime::ObjectRef>(checked);
    if (recvRef.isStackSlotRef()) {
        if (recvRef.stackRefSchemaId() == 0u) {
            stackRefSlots_[recvRef.slot] = runtime::detachFrameSliceValue(value);
            return true;
        }
        const auto* sc = runtime::getFlatStructSchema(recvRef.stackRefSchemaId());
        if (!sc || !runtime::extractStructLeavesToBuffer(value, *sc, stackRefSlots_.data() + recvRef.slot)) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT stack ref struct store failed");
        }
        return true;
    }
    auto* targetHeap = resolveHeap(recvRef);
    runtime::RuntimeError fieldError;
    if (executionPolicy_ &&
        !executionPolicy_->checkFieldWrite(
            heap_.get(),
            targetHeap,
            recvRef,
            typeId,
            fieldIndex,
            field.name ? field.name : "",
            value,
            fieldError)) {
        error_ = std::move(fieldError);
        if (error_.code == runtime::ErrorCode::None) {
            error_.code = runtime::ErrorCode::CapabilityDenied;
        }
        if (error_.message.empty()) {
            error_.message = "field write rejected by execution policy";
        }
        populateErrorStackTrace();
        return false;
    }
    if (!checkCrossHeapRefAccess(recvRef, true, "field store")) {
        return false;
    }
    if (!targetHeap ||
        !targetHeap->fieldSet(
            recvRef,
            fieldIndex,
            value,
            &fieldError)) {
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
    if (structure.isFrameSlice()) {
        if (const auto* sc = runtime::getFlatStructSchema(structure.frameSchemaId());
            sc != nullptr && fieldIndex < sc->directFields.size()) {
            const auto& fd = sc->directFields[fieldIndex];
            const std::uint32_t subAbs = structure.storage.frameSliceOffset() + fd.leafOffset;
            if (fd.subSchemaId != 0u && subAbs <= runtime::StructStorageHandle::FrameOffsetMask) {
                result = runtime::StructValue::makeFrameSlice(
                    field.typeId, structure.storage.frameSliceBank(), subAbs, fd.subSchemaId);
                return true;
            }
            if (const runtime::Value* leaf = runtime::detail::lookupFlatStackSlice(
                    structure.storage.frameSliceBank(), subAbs, 1u)) {
                if (!expectSignatureType(*leaf, field.type, field.typeId, "struct field load value")) {
                    return false;
                }
                result = *leaf;
                return true;
            }
        }
    }
    const auto fieldVal = runtime::structValueFieldAt(structure, fieldIndex);
    if (!fieldVal) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "struct field load references an invalid field slot");
    }
    if (!expectSignatureType(
            *fieldVal,
            field.type,
            field.typeId,
            "struct field load value")) {
        return false;
    }
    result = *fieldVal;
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
    if (activeStackRefScopes_ > 0 && stackRefBankId_ != 0u) {
        constexpr std::size_t kMaxTransientStackSliceSlots = 65536u;
        if (const auto* sc = runtime::findFlatStructSchemaByTypeId(typeId);
            sc != nullptr && fieldIndex < sc->directFields.size()) {
            const std::uint32_t baseOff = stackRefTop_;
            const std::size_t need = static_cast<std::size_t>(baseOff) + sc->flatSlotCount;
            if (need <= kMaxTransientStackSliceSlots &&
                need <= runtime::StructStorageHandle::FrameOffsetMask) {
                if (need > stackRefSlots_.size()) {
                    stackRefSlots_.resize(std::max<std::size_t>(need, stackRefSlots_.capacity() * 2u + 64u));
                }
                runtime::Value* dstLeaves = stackRefSlots_.data() + baseOff;
                if (runtime::extractStructLeavesToBuffer(receiver, *sc, dstLeaves)) {
                    const auto& fd = sc->directFields[fieldIndex];
                    bool leafOk = false;
                    if (fd.subSchemaId == 0u) {
                        dstLeaves[fd.leafOffset] = runtime::detachFrameSliceValue(value);
                        leafOk = true;
                    } else if (const auto* subSc = runtime::getFlatStructSchema(fd.subSchemaId)) {
                        leafOk = runtime::extractStructLeavesToBuffer(value, *subSc, dstLeaves + fd.leafOffset);
                    }
                    if (leafOk) {
                        stackRefTop_ = static_cast<std::uint32_t>(need);
                        result = runtime::StructValue::makeFrameSlice(typeId, stackRefBankId_, baseOff, sc->schemaId);
                        return true;
                    }
                }
            }
        }
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
    std::size_t generatedFieldCount,
    std::size_t generatedStructAlign) const noexcept {
    TypedRawColumnCache cache{};
    if (!heap_ || executionPolicy_ != nullptr) return cache;
    const auto* ref = std::get_if<runtime::ObjectRef>(&arrayValue);
    if (!ref) return cache;
    const auto* view = heap_->rawColumnView(*ref);
    if (!view || view->basePtr == nullptr) return cache;
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
    cache.readOnly = view->readOnly;
    cache.fieldCount = static_cast<std::uint8_t>(view->fields.size());
    for (std::size_t i = 0; i < view->fields.size(); ++i) {
        cache.fields[i] = TypedRawFieldCache{
            view->fields[i].byteOffset,
            view->fields[i].kind,
        };
    }
    const std::size_t alignBytes = std::max<std::size_t>(1u, generatedStructAlign);
    const bool baseAligned =
        (reinterpret_cast<std::uintptr_t>(view->basePtr) % alignBytes) == 0u;
    if (baseAligned &&
        generatedFields &&
        generatedFieldCount == view->fields.size() &&
        generatedStructSize == view->strideBytes) {
        const auto nativeKindSize = [](runtime::NativeFieldKind kind) noexcept -> std::size_t {
            switch (kind) {
            case runtime::NativeFieldKind::Bool: return sizeof(bool);
            case runtime::NativeFieldKind::Int8: return sizeof(std::int8_t);
            case runtime::NativeFieldKind::UInt8: return sizeof(std::uint8_t);
            case runtime::NativeFieldKind::Int16: return sizeof(std::int16_t);
            case runtime::NativeFieldKind::UInt16: return sizeof(std::uint16_t);
            case runtime::NativeFieldKind::Int32: return sizeof(std::int32_t);
            case runtime::NativeFieldKind::UInt32: return sizeof(std::uint32_t);
            case runtime::NativeFieldKind::Int64: return sizeof(std::int64_t);
            case runtime::NativeFieldKind::UInt64: return sizeof(std::uint64_t);
            case runtime::NativeFieldKind::Float32: return sizeof(float);
            case runtime::NativeFieldKind::Float64: return sizeof(double);
            case runtime::NativeFieldKind::Fixed: return sizeof(runtime::FixedValue);
            case runtime::NativeFieldKind::Fixed16: return sizeof(std::int16_t);
            case runtime::NativeFieldKind::Fixed64: return sizeof(runtime::Fixed64Value);
            case runtime::NativeFieldKind::Vec2: return sizeof(runtime::Vec2Value);
            case runtime::NativeFieldKind::Vec3: return sizeof(runtime::Vec3Value);
            case runtime::NativeFieldKind::Quat: return sizeof(runtime::QuatValue);
            case runtime::NativeFieldKind::Vec2f: return sizeof(runtime::Vec2fValue);
            case runtime::NativeFieldKind::Vec3f: return sizeof(runtime::Vec3fValue);
            case runtime::NativeFieldKind::Quatf: return sizeof(runtime::QuatfValue);
            }
            return 0u;
        };
        bool exact = true;
        for (std::size_t i = 0; i < generatedFieldCount; ++i) {
            if (view->fields[i].byteOffset != generatedFields[i].byteOffset ||
                view->fields[i].kind != generatedFields[i].expectedKind ||
                nativeKindSize(view->fields[i].kind) != generatedFields[i].byteSize ||
                view->fields[i].kind == runtime::NativeFieldKind::Fixed16) {
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
                const auto text = [&](const runtime::Value& value,
                                      std::optional<std::string_view>& outText) -> bool {
                    if (const auto* direct = std::get_if<std::string>(&value)) {
                        outText = *direct;
                        return true;
                    }
                    if (const auto* reference =
                            std::get_if<runtime::ObjectRef>(&value)) {
                        if (!checkCrossHeapRefAccess(*reference, false, "string equality")) {
                            return false;
                        }
                        auto* targetHeap = resolveHeap(*reference);
                        outText = targetHeap ? targetHeap->stringView(*reference) : std::nullopt;
                        if (!outText) {
                            return fail(
                                runtime::ErrorCode::InvalidObjectReference,
                                "string equality encountered an invalid managed reference");
                        }
                        return true;
                    }
                    return fail(
                        runtime::ErrorCode::InvalidObjectReference,
                        "string equality encountered an invalid managed reference");
                };
                std::optional<std::string_view> a;
                std::optional<std::string_view> b;
                if (!text(left, a) || !text(right, b)) {
                    return false;
                }
                equal = *a == *b;
            }
        } else if (runtime::valueType(left) == semantic::PrimitiveType::Struct) {
            if (left == right) {
                equal = true;
            } else if (left.rawTag() == runtime::ValueTag::Struct &&
                       right.rawTag() == runtime::ValueTag::Struct) {
                const auto& ls = left.asStructUnchecked();
                const auto& rs = right.asStructUnchecked();
                if (ls.typeId == rs.typeId && ls.fieldCount() == rs.fieldCount()) {
                    const auto count = ls.fieldCount();
                    bool allEq = true;
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto lf = runtime::structValueFieldAt(ls, i);
                        const auto rf = runtime::structValueFieldAt(rs, i);
                        runtime::Value fieldEqVal;
                        if (!lf || !rf) {
                            allEq = false;
                            break;
                        }
                        if (!binary(BinaryOperation::Equal, checkedArithmetic, *lf, *rf, fieldEqVal)) {
                            return false;
                        }
                        if (!std::holds_alternative<bool>(fieldEqVal) ||
                            !std::get<bool>(fieldEqVal)) {
                            allEq = false;
                            break;
                        }
                    }
                    equal = allEq;
                } else {
                    equal = false;
                }
            } else {
                equal = false;
            }
        } else {
            if (runtime::valueType(left) == semantic::PrimitiveType::Object ||
                runtime::valueType(left) == semantic::PrimitiveType::Array) {
                const auto checkRef = [&](const runtime::Value& side) -> bool {
                    if (const auto* r = std::get_if<runtime::ObjectRef>(&side)) {
                        if (!checkCrossHeapRefAccess(*r, false, "reference equality")) {
                            return false;
                        }
                        auto* h = resolveHeap(*r);
                        if (!h || !h->isAlive(*r)) {
                            return fail(
                                runtime::ErrorCode::InvalidObjectReference,
                                "reference equality encountered an invalid managed reference");
                        }
                    } else if (!std::holds_alternative<runtime::NullObject>(side) &&
                               !std::holds_alternative<runtime::NullArray>(side)) {
                        return fail(
                            runtime::ErrorCode::InvalidObjectReference,
                            "reference equality encountered an invalid managed reference");
                    }
                    return true;
                };
                if (!checkRef(left) || !checkRef(right)) {
                    return false;
                }
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

namespace {

[[noreturn]] void abortOrThrowInvalidArg(const char* message) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::invalid_argument(message);
#else
    (void)message;
    std::abort();
#endif
}

} // namespace

Program::Program(const ProgramDescriptor& descriptor)
    : descriptor_(&descriptor),
      heap_(std::make_shared<runtime::ManagedHeap>()),
      asyncRuntime_(std::make_shared<runtime::AsyncRuntimeContext>(heap_.get())),
      lifecycleTarget_(std::make_shared<LifecycleTarget>()) {
    lifecycleTarget_->program = this;
    if (descriptor.abiMajor != RuntimeAbiMajor ||
        descriptor.abiMinor > RuntimeAbiMinor) {
        abortOrThrowInvalidArg("AOT program requires an incompatible runtime ABI");
    }
    if (!descriptor.name || descriptor.name[0] == '\0' ||
        descriptor.moduleCount == 0 || !descriptor.moduleNames ||
        descriptor.functionCount == 0 || !descriptor.functions ||
        (descriptor.typeCount != 0 && !descriptor.types) ||
        (descriptor.sourceMapCount != 0 && !descriptor.sourceMap) ||
        (descriptor.attributeCount != 0 && !descriptor.attributes)) {
        abortOrThrowInvalidArg("AOT program descriptor is incomplete");
    }

    std::unordered_set<std::string> moduleNames;
    for (std::uint32_t index = 0; index < descriptor.moduleCount; ++index) {
        const auto* name = descriptor.moduleNames[index];
        if (!name || name[0] == '\0' || !moduleNames.emplace(name).second) {
            abortOrThrowInvalidArg("AOT program contains an invalid module table");
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
            abortOrThrowInvalidArg("AOT program contains an invalid type table");
        }
        if (moduleNames.count(type.moduleName) == 0 ||
            (type.kind != semantic::TypeKind::Class &&
             type.kind != semantic::TypeKind::Struct &&
             type.kind != semantic::TypeKind::Enum) ||
            (type.kind == semantic::TypeKind::Enum && type.fieldCount != 0) ||
            (type.kind != semantic::TypeKind::Enum &&
             type.enumMemberCount != 0)) {
            abortOrThrowInvalidArg("AOT type descriptor kind is inconsistent");
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
                abortOrThrowInvalidArg("AOT program contains an invalid field table");
            }
        }
        std::unordered_set<std::string> enumNames;
        for (std::uint32_t memberIndex = 0;
             memberIndex < type.enumMemberCount;
             ++memberIndex) {
            const auto& member = type.enumMembers[memberIndex];
            if (!member.name || member.name[0] == '\0' ||
                !enumNames.emplace(member.name).second) {
                abortOrThrowInvalidArg("AOT program contains an invalid enum table");
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
                abortOrThrowInvalidArg(
                    "AOT field references an unknown exact type");
            }
        }
    }
    {
        std::vector<std::pair<semantic::SymbolId, std::vector<runtime::BlittableFieldInput>>> structSpecs;
        for (std::uint32_t index = 0; index < descriptor.typeCount; ++index) {
            const auto& type = descriptor.types[index];
            if (type.kind == semantic::TypeKind::Struct &&
                !type.hasValueLifecycle &&
                type.fields && type.fieldCount > 0) {
                bool hasStaticOrConstField = false;
                std::vector<runtime::BlittableFieldInput> fInputs;
                fInputs.reserve(type.fieldCount);
                for (std::uint32_t fi = 0; fi < type.fieldCount; ++fi) {
                    if (type.fields[fi].staticField || type.fields[fi].constField) {
                        hasStaticOrConstField = true;
                        break;
                    }
                    fInputs.push_back(runtime::BlittableFieldInput{
                        type.fields[fi].type, type.fields[fi].typeId});
                }
                if (!hasStaticOrConstField && !fInputs.empty()) {
                    structSpecs.emplace_back(type.id, std::move(fInputs));
                }
            }
        }
        if (!structSpecs.empty()) {
            runtime::registerBlittableStructTypes(structSpecs);
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
            abortOrThrowInvalidArg("AOT program contains an invalid function table");
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
                abortOrThrowInvalidArg(
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
            abortOrThrowInvalidArg("AOT program contains an invalid source map");
        }
    }

    registerValueLifecycleHooks();
}

Program::~Program() {
    for (const auto& record : modifiedHotPatchSlots_) {
        if (record.slot) {
            record.slot->patchedImplementation = record.originalImplementation;
            record.slot->revision = 0;
        }
    }
    modifiedHotPatchSlots_.clear();
    if (lifecycleTarget_) {
        std::shared_ptr<runtime::ManagedHeap> regHeap;
        std::uint64_t token = 0;
        {
            std::lock_guard<std::mutex> lock(lifecycleTarget_->mutex);
            lifecycleTarget_->program = nullptr;
            lifecycleTarget_->alive = false;
            regHeap = lifecycleTarget_->registeredHeap.lock();
            token = lifecycleTarget_->sweepToken;
            lifecycleTarget_->registeredHeap.reset();
            lifecycleTarget_->sweepToken = 0;
        }
        if (regHeap && token != 0) {
            regHeap->removeValueLifecycleSweepHandler(token);
        }
    }
}


struct AotResumeEnv {
    const ProgramDescriptor* descriptor = nullptr;
    std::shared_ptr<runtime::ManagedHeap> heap;
    std::shared_ptr<const runtime::BindingRegistry> bindings;
    runtime::ExecutionOptions options;
    std::weak_ptr<runtime::AsyncRuntimeContext> asyncRuntimeWeak;
    std::unordered_map<semantic::SymbolId, const TypeDescriptor*> ownedTypes;
    std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> ownedFunctions;
    std::unordered_map<semantic::SymbolId, FunctionImplementation> hotPatchedBodies;
};

namespace {

thread_local bool tlsInAotValueLifecycleDispose = false;

std::string extractAotExceptionMessage(
    const std::unordered_map<semantic::SymbolId, const TypeDescriptor*>* typesMap,
    const runtime::ManagedHeap* heap,
    const runtime::Value& excVal) {
    if (!heap || !std::holds_alternative<runtime::ObjectRef>(excVal)) {
        return "unhandled script exception in async function";
    }
    const auto ref = std::get<runtime::ObjectRef>(excVal);
    const auto tid = heap->objectTypeId(ref);
    const TypeDescriptor* desc = nullptr;
    if (tid && typesMap) {
        const auto it = typesMap->find(*tid);
        if (it != typesMap->end()) desc = it->second;
    }
    if (desc) {
        for (std::uint32_t f = 0; f < desc->fieldCount; ++f) {
            if (desc->fields && desc->fields[f].type == semantic::PrimitiveType::String) {
                if (const auto fv = heap->fieldGet(ref, f)) {
                    if (!std::holds_alternative<runtime::NullString>(*fv) &&
                        !std::holds_alternative<std::monostate>(*fv)) {
                        const auto s = runtime::valueToString(*fv, heap);
                        if (!s.empty()) return s;
                    }
                }
            }
        }
        if (desc->name) return desc->name;
    }
    return "unhandled script exception in async function";
}

} // namespace

void ExecutionContext::setAsyncRuntime(
    std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime,
    std::shared_ptr<void> resumeEnv) {
    asyncRuntime_ = std::move(asyncRuntime);
    resumeEnv_ = std::move(resumeEnv);
    if (asyncRuntime_ && heap_ && asyncRuntime_->awaitables().heap() == nullptr) {
        asyncRuntime_->setHeap(heap_.get());
    }
}

void ExecutionContext::setAwaitScheduler(
    runtime::IAwaitScheduler* awaitScheduler) noexcept {
    if (options_.awaitScheduler != awaitScheduler) {
        options_.awaitScheduler = awaitScheduler;
        resumeEnv_.reset();
    }
}

void ExecutionContext::ensureResumeEnv() {
    if (!resumeEnv_) {
        auto env = std::make_shared<AotResumeEnv>();
        env->descriptor = program_;
        env->heap = heap_;
        env->bindings = bindings_;
        env->options = options_;
        env->asyncRuntimeWeak = asyncRuntime_;
        if (typesPtr_) env->ownedTypes = *typesPtr_;
        if (functionsPtr_) env->ownedFunctions = *functionsPtr_;
        resumeEnv_ = std::move(env);
        return;
    }
    auto* env = static_cast<AotResumeEnv*>(resumeEnv_.get());
    env->options.awaitScheduler = options_.awaitScheduler;
    if (asyncRuntime_ && asyncRuntime_.use_count() > 0) {
        env->asyncRuntimeWeak = asyncRuntime_;
    }
}

bool ExecutionContext::prepareCoroutineFrame(
    semantic::SymbolId functionSymbolId,
    semantic::SymbolId returnStructTypeId,
    runtime::AwaitableHandle awaitedHandle,
    runtime::CoroutineFrameHandle& ioActiveCoroutine,
    runtime::AwaitableHandle& outCompletionHandle) {
    if (!ioActiveCoroutine.valid()) {
        outCompletionHandle =
            asyncRuntime_->awaitables().allocatePending(runtime::ResumeQueue::Update);
        ioActiveCoroutine = asyncRuntime_->coroutines().allocateFrame(
            functionSymbolId,
            returnStructTypeId,
            outCompletionHandle,
            runtime::ResumeQueue::Update);
        return true;
    }
    const auto* existing = asyncRuntime_->coroutines().resolve(ioActiveCoroutine);
    if (!existing) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT active coroutine frame handle is stale");
    }
    if (existing->invalidated) {
        if (awaitedHandle.valid() && !awaitedHandle.isCompletedVoidSentinel()) {
            (void)asyncRuntime_->awaitables().discard(awaitedHandle);
        }
        return fail(
            runtime::ErrorCode::ExecutionTerminated,
            "AOT coroutine invalidated by hot reload");
    }
    outCompletionHandle = existing->completionHandle;
    return true;
}

runtime::Value ExecutionContext::makePendingAsyncReturnCarrier(
    semantic::SymbolId returnStructTypeId,
    runtime::AwaitableHandle completionHandle) const {
    if (returnStructTypeId != 0 && heap_) {
        if (const TypeDescriptor* ts = findType(returnStructTypeId);
            ts != nullptr && ts->kind == semantic::TypeKind::Class) {
            std::vector<runtime::Value> fields;
            std::vector<std::size_t> refFields;
            fields.reserve(ts->fieldCount);
            for (std::uint32_t idx = 0; idx < ts->fieldCount; ++idx) {
                const auto& f = ts->fields[idx];
                runtime::Value fv{};
                switch (f.type) {
                case semantic::PrimitiveType::Bool: fv = runtime::Value(false); break;
                case semantic::PrimitiveType::Byte:
                case semantic::PrimitiveType::SByte:
                case semantic::PrimitiveType::Short:
                case semantic::PrimitiveType::UShort:
                case semantic::PrimitiveType::Int:
                case semantic::PrimitiveType::UInt: fv = runtime::Value(std::int64_t{0}); break;
                case semantic::PrimitiveType::Long:
                case semantic::PrimitiveType::ULong:
                    fv = idx == 0
                        ? runtime::Value(runtime::LongValue{
                              static_cast<std::int64_t>(completionHandle.pack64())})
                        : runtime::Value(runtime::LongValue{0});
                    break;
                case semantic::PrimitiveType::Char: fv = runtime::Value(runtime::CharValue{0}); break;
                case semantic::PrimitiveType::Float:
                case semantic::PrimitiveType::Double: fv = runtime::Value(0.0); break;
                case semantic::PrimitiveType::String:
                    fv = runtime::Value(runtime::NullString{});
                    refFields.push_back(idx);
                    break;
                case semantic::PrimitiveType::Object:
                    fv = runtime::Value(runtime::NullObject{});
                    refFields.push_back(idx);
                    break;
                case semantic::PrimitiveType::Array:
                    fv = runtime::Value(runtime::NullArray{});
                    refFields.push_back(idx);
                    break;
                default:
                    fv = runtime::Value{};
                    break;
                }
                fields.push_back(std::move(fv));
            }
            runtime::RuntimeError allocErr;
            if (const auto objRef = heap_->allocateObject(
                    returnStructTypeId,
                    std::move(fields),
                    std::move(refFields),
                    &allocErr)) {
                return runtime::Value(*objRef);
            }
            return runtime::Value(runtime::NullObject{});
        }
    }
    return runtime::makeAwaitableStructValue(returnStructTypeId, completionHandle);
}

void ExecutionContext::resumeCoroutineThunk(
    void* runnerContext,
    runtime::CoroutineFrameHandle frameHandle,
    runtime::CoroutineFrame& frame,
    runtime::AsyncRuntimeContext& asyncContext,
    const char* faultFallback) {
    auto* env = static_cast<AotResumeEnv*>(runnerContext);
    runtime::ValuePoolDomain* domainPtr = nullptr;
    if (env && env->heap) {
        domainPtr = env->heap->valuePoolDomain().get();
    } else if (asyncContext.awaitables().heap()) {
        domainPtr = asyncContext.awaitables().heap()->valuePoolDomain().get();
    }
    runtime::ScopedValuePoolDomain domainScope(domainPtr);

    const auto faultUnresolvedFrame = [&](const runtime::RuntimeError& err) {
        if (auto* f = asyncContext.coroutines().resolve(frameHandle)) {
            if (f->invalidated) {
                f->completed = true;
                (void)asyncContext.awaitables().trySetCanceled(f->completionHandle);
                return;
            }
            if (!f->completed && !f->suspended) {
                f->completed = true;
                const runtime::ErrorCode code =
                    (err.code != runtime::ErrorCode::None)
                        ? err.code
                        : runtime::ErrorCode::InvalidProgram;
                const std::string msg = !err.message.empty()
                    ? err.message
                    : std::string(faultFallback);
                (void)asyncContext.awaitables().trySetFaulted(
                    f->completionHandle, code, msg);
            }
        }
    };

    if (void* activeRaw = asyncContext.activeAotContext()) {
        auto* activeCtx = static_cast<ExecutionContext*>(activeRaw);
        if (activeCtx->hasFunction(frame.functionSymbolId)) {
            const runtime::CoroutineFrameHandle savedCoro = activeCtx->resumingCoroutine_;
            runtime::CoroutineFrame* const savedFramePtr = activeCtx->resumingFramePtr_;
            runtime::RuntimeError savedError = std::move(activeCtx->error_);
            std::optional<runtime::Value> savedPendingExc =
                std::move(activeCtx->pendingException_);
            runtime::IAwaitScheduler* const savedScheduler =
                activeCtx->options_.awaitScheduler;

            activeCtx->resumingCoroutine_ = frameHandle;
            activeCtx->resumingFramePtr_ = &frame;
            activeCtx->error_ = runtime::RuntimeError{};
            activeCtx->pendingException_.reset();
            if (env) {
                activeCtx->options_.awaitScheduler = env->options.awaitScheduler;
            }

            runtime::Value ignored{};
            const bool ok = activeCtx->invoke(frame.functionSymbolId, nullptr, 0, ignored);
            if (!ok) {
                faultUnresolvedFrame(activeCtx->error_);
            }

            activeCtx->resumingCoroutine_ = savedCoro;
            activeCtx->resumingFramePtr_ = savedFramePtr;
            activeCtx->error_ = std::move(savedError);
            activeCtx->pendingException_ = std::move(savedPendingExc);
            activeCtx->options_.awaitScheduler = savedScheduler;
            return;
        }
    }
    const std::shared_ptr<const void> keepEnvAlive = frame.runnerOwner;
    if (!env || !env->descriptor) return;
    runtime::ExecutionOptions opts = env->options;
    ExecutionContext freshCtx(
        *env->descriptor,
        env->heap,
        env->bindings,
        std::move(opts),
        &env->ownedTypes,
        &env->ownedFunctions);
    freshCtx.setHotPatchedBodies(&env->hotPatchedBodies);
    std::shared_ptr<runtime::AsyncRuntimeContext> borrowedAsync(
        std::shared_ptr<runtime::AsyncRuntimeContext>{}, &asyncContext);
    freshCtx.setAsyncRuntime(
        std::move(borrowedAsync),
        std::const_pointer_cast<void>(keepEnvAlive));
    freshCtx.resumingCoroutine_ = frameHandle;
    freshCtx.resumingFramePtr_ = &frame;

    void* const prevBridge = asyncContext.activeAotContext();
    asyncContext.setActiveAotContext(&freshCtx);
    struct RestoreBridge {
        runtime::AsyncRuntimeContext& ctx;
        void* prev;
        ~RestoreBridge() noexcept { ctx.setActiveAotContext(prev); }
    } restoreBridge{asyncContext, prevBridge};

    runtime::Value ignored{};
    const bool ok = freshCtx.invoke(frame.functionSymbolId, nullptr, 0, ignored);
    if (!ok) {
        faultUnresolvedFrame(freshCtx.error());
    }
}

bool ExecutionContext::suspendValueCoroutineFrame(
    semantic::SymbolId functionSymbolId,
    semantic::SymbolId returnStructTypeId,
    runtime::AwaitableHandle awaitedHandle,
    std::uint32_t resumeBlockId,
    const std::vector<runtime::Value>& argumentValues,
    const std::vector<runtime::Value>& locals,
    const std::vector<runtime::Value>& registers,
    runtime::CoroutineFrameHandle& ioActiveCoroutine,
    runtime::Value& outResult,
    bool runInlineOnComplete) {
    runtime::AwaitableHandle completionHandle{};
    if (!prepareCoroutineFrame(
            functionSymbolId,
            returnStructTypeId,
            awaitedHandle,
            ioActiveCoroutine,
            completionHandle)) {
        return false;
    }

    struct AotRunnerThunk {
        static void run(
            void* runnerContext,
            runtime::CoroutineFrameHandle frameHandle,
            runtime::CoroutineFrame& frame,
            runtime::AsyncRuntimeContext& asyncContext) {
            ExecutionContext::resumeCoroutineThunk(
                runnerContext,
                frameHandle,
                frame,
                asyncContext,
                "resumed AOT coroutine failed with runtime error");
        }
    };

    ensureResumeEnv();

    std::shared_ptr<const void> ownerCopy{};
    const auto* curFrame = asyncRuntime_->coroutines().resolve(ioActiveCoroutine);
    if (!curFrame || curFrame->runnerOwner.get() != resumeEnv_.get()) {
        ownerCopy = resumeEnv_;
    }

    const auto detachSliceIfNeeded = [](const std::vector<runtime::Value>& src,
                                        std::vector<runtime::Value>& buf) -> const runtime::Value* {
        for (const auto& item : src) {
            if (item.rawTag() == runtime::ValueTag::Struct &&
                item.asStructUnchecked().isFrameSlice()) {
                buf.resize(src.size());
                for (std::size_t i = 0; i < src.size(); ++i) {
                    buf[i] = runtime::detachFrameSliceValue(src[i]);
                }
                return buf.data();
            }
        }
        return src.data();
    };
    std::vector<runtime::Value> detachedArgs, detachedLocals, detachedRegs;
    const runtime::Value* safeArgsPtr = detachSliceIfNeeded(argumentValues, detachedArgs);
    const runtime::Value* safeLocalsPtr = detachSliceIfNeeded(locals, detachedLocals);
    const runtime::Value* safeRegsPtr = detachSliceIfNeeded(registers, detachedRegs);

    if (!asyncRuntime_->coroutines().suspendFrame(
            ioActiveCoroutine,
            safeArgsPtr,
            argumentValues.size(),
            safeLocalsPtr,
            locals.size(),
            safeRegsPtr,
            registers.size(),
            resumeBlockId,
            awaitedHandle,
            &AotRunnerThunk::run,
            resumeEnv_.get(),
            std::move(ownerCopy),
            runInlineOnComplete)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT failed to suspend coroutine onto awaited handle");
    }

    outResult = makePendingAsyncReturnCarrier(returnStructTypeId, completionHandle);
    return true;
}

bool ExecutionContext::tryEnterResumingCoroutine(
    semantic::SymbolId functionSymbolId,
    std::vector<runtime::Value>& argumentValues,
    std::vector<runtime::Value>& locals,
    std::vector<runtime::Value>& registers,
    std::uint32_t& outResumeBlockId,
    runtime::CoroutineFrameHandle& outActiveCoroutine) {
    if (resumingFramePtr_ == nullptr ||
        !resumingCoroutine_.valid() ||
        resumingFramePtr_->functionSymbolId != functionSymbolId ||
        resumingFramePtr_->usesScalarBuffer) {
        return false;
    }
    auto* frame = resumingFramePtr_;
    outActiveCoroutine = resumingCoroutine_;
    resumingCoroutine_ = runtime::CoroutineFrameHandle::invalid();
    resumingFramePtr_ = nullptr;

    argumentValues = frame->savedArguments;
    const std::size_t nLocs = std::min(locals.size(), frame->savedLocals.size());
    for (std::size_t i = 0; i < nLocs; ++i) {
        locals[i] = frame->savedLocals[i];
    }
    const std::size_t nRegs = std::min(registers.size(), frame->savedRegisters.size());
    for (std::size_t i = 0; i < nRegs; ++i) {
        registers[i] = frame->savedRegisters[i];
    }
    outResumeBlockId = frame->resumeBlockId;
    return true;
}

bool ExecutionContext::awaitSuspend(
    semantic::SymbolId functionSymbolId,
    semantic::SymbolId returnStructTypeId,
    const runtime::Value& awaitableValue,
    runtime::Value& awaitLocalSlot,
    std::uint32_t resumeBlockId,
    const std::vector<runtime::Value>& argumentValues,
    const std::vector<runtime::Value>& locals,
    const std::vector<runtime::Value>& registers,
    runtime::CoroutineFrameHandle& ioActiveCoroutine,
    bool& outSuspendedNow,
    runtime::Value& outResult) {
    outSuspendedNow = false;
    awaitLocalSlot = awaitableValue;

    if (options_.awaitScheduler &&
        options_.awaitScheduler->handlesValue(awaitableValue)) {
        if (options_.awaitScheduler->isCompleted(awaitableValue)) {
            return true;
        }
        if (!asyncRuntime_) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT custom await suspension requires an AsyncRuntimeContext");
        }
        const bool hadActiveCoro = ioActiveCoroutine.valid();
        const runtime::AwaitableHandle bridgeHandle =
            asyncRuntime_->awaitables().allocatePending(runtime::ResumeQueue::Update);
        if (!suspendValueCoroutineFrame(
                functionSymbolId,
                returnStructTypeId,
                bridgeHandle,
                resumeBlockId,
                argumentValues,
                locals,
                registers,
                ioActiveCoroutine,
                outResult,
                /*runInlineOnComplete=*/true)) {
            (void)asyncRuntime_->awaitables().discard(bridgeHandle);
            return false;
        }

        const auto* env = static_cast<const AotResumeEnv*>(resumeEnv_.get());
        std::weak_ptr<runtime::AsyncRuntimeContext> weakAsync =
            env ? env->asyncRuntimeWeak : std::weak_ptr<runtime::AsyncRuntimeContext>{};
        const bool hasWeakAsync = !weakAsync.expired();
        std::shared_ptr<const void> envOwner = resumeEnv_;
        auto fired = std::make_shared<std::atomic<bool>>(false);
        runtime::AsyncRuntimeContext* const rawAsyncCtx = asyncRuntime_.get();
        const runtime::CoroutineFrameHandle coroHandle = ioActiveCoroutine;

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
                std::shared_ptr<runtime::AsyncRuntimeContext> lockedAsync;
                runtime::AsyncRuntimeContext* ctx = nullptr;
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

        runtime::RuntimeError schedErr;
        if (!options_.awaitScheduler->onSuspend(
                awaitableValue, std::move(resumeCallback), schedErr)) {
            (void)asyncRuntime_->awaitables().discard(bridgeHandle);
            if (!hadActiveCoro && ioActiveCoroutine.valid()) {
                runtime::AwaitableHandle allocatedCompletion{};
                if (const auto* f = asyncRuntime_->coroutines().resolve(ioActiveCoroutine)) {
                    allocatedCompletion = f->completionHandle;
                }
                asyncRuntime_->coroutines().recycleFrame(ioActiveCoroutine);
                ioActiveCoroutine = runtime::CoroutineFrameHandle{};
                if (allocatedCompletion.valid() &&
                    !allocatedCompletion.isCompletedVoidSentinel()) {
                    (void)asyncRuntime_->awaitables().discard(allocatedCompletion);
                }
            } else if (ioActiveCoroutine.valid()) {
                if (auto* f = asyncRuntime_->coroutines().resolve(ioActiveCoroutine)) {
                    f->suspended = false;
                }
            }
            return fail(
                schedErr.code == runtime::ErrorCode::None
                    ? runtime::ErrorCode::InvalidProgram
                    : schedErr.code,
                schedErr.message.empty()
                    ? std::string("custom await scheduler rejected suspension")
                    : schedErr.message);
        }

        outSuspendedNow = true;
        return true;
    }

    runtime::AwaitableHandle awaitedHandle{};
    if (!asyncRuntime_ ||
        !runtime::tryExtractAwaitableHandle(awaitableValue, awaitedHandle) ||
        !asyncRuntime_->awaitables().isValid(awaitedHandle)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT awaited value does not hold a valid Awaitable handle");
    }
    if (asyncRuntime_->awaitables().isCompleted(awaitedHandle)) {
        return true;
    }

    if (!suspendValueCoroutineFrame(
            functionSymbolId,
            returnStructTypeId,
            awaitedHandle,
            resumeBlockId,
            argumentValues,
            locals,
            registers,
            ioActiveCoroutine,
            outResult,
            /*runInlineOnComplete=*/false)) {
        return false;
    }

    outSuspendedNow = true;
    return true;
}

bool ExecutionContext::awaitResume(
    runtime::Value& awaitLocalSlot,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    runtime::Value* optionalOutResult) {
    const runtime::Value awaitableVal = awaitLocalSlot;
    awaitLocalSlot = runtime::Value{};

    if (options_.awaitScheduler &&
        options_.awaitScheduler->handlesValue(awaitableVal)) {
        runtime::RuntimeError schedErr;
        runtime::Value payload{};
        const bool hasResult = (optionalOutResult != nullptr);
        if (!options_.awaitScheduler->tryGetResult(
                awaitableVal, hasResult, payload, schedErr)) {
            const runtime::ErrorCode effCode =
                schedErr.code == runtime::ErrorCode::None
                    ? runtime::ErrorCode::ScriptException
                    : schedErr.code;
            if (effCode == runtime::ErrorCode::ScriptException && heap_ != nullptr) {
                schedErr.code = effCode;
                if (schedErr.message.empty()) {
                    schedErr.message = "custom awaiter GetResult failed";
                }
                const auto matRef = runtime::materializeAsyncResumeException(
                    heap_.get(), runtime::AwaitStatus::Faulted, schedErr, runtime::Value{});
                if (matRef) {
                    pendingException_ = runtime::Value(*matRef);
                    return false;
                }
            }
            return fail(
                effCode,
                schedErr.message.empty()
                    ? std::string("custom awaiter GetResult failed")
                    : schedErr.message);
        }
        if (optionalOutResult != nullptr) {
            if (!expectSignatureType(
                    payload, expectedType, expectedTypeId, "await result")) {
                return false;
            }
            *optionalOutResult = std::move(payload);
        }
        return true;
    }

    runtime::AwaitableHandle awaitedHandle{};
    if (!asyncRuntime_ ||
        !runtime::tryExtractAwaitableHandle(awaitableVal, awaitedHandle)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT await.resume local does not hold a valid Awaitable");
    }
    runtime::Value payload{};
    runtime::RuntimeError faultErr{};
    runtime::Value excObj{};
    const auto st = asyncRuntime_->awaitables().getResult(
        awaitedHandle, payload, faultErr, &excObj);
    if (st == runtime::AwaitStatus::Succeeded) {
        if (optionalOutResult != nullptr) {
            if (!expectSignatureType(payload, expectedType, expectedTypeId, "await result")) {
                return false;
            }
            *optionalOutResult = std::move(payload);
        }
        return true;
    }
    if (st == runtime::AwaitStatus::Canceled || st == runtime::AwaitStatus::Faulted) {
        const auto matRef = runtime::materializeAsyncResumeException(
            heap_.get(), st, faultErr, excObj);
        if (!matRef) {
            return fail(runtime::ErrorCode::OutOfMemory, "failed to allocate AOT async exception");
        }
        pendingException_ = runtime::Value(*matRef);
        return false;
    }
    return fail(
        runtime::ErrorCode::InvalidProgram,
        "AOT await.resume encountered non-terminal Awaitable");
}

bool ExecutionContext::asyncResolve(
    runtime::CoroutineFrameHandle activeCoroutine,
    semantic::SymbolId returnStructTypeId,
    const runtime::Value* optionalPayload,
    runtime::Value& outWrappedResult) {
    if (activeCoroutine.valid() && asyncRuntime_) {
        auto* frame = asyncRuntime_->coroutines().resolve(activeCoroutine);
        if (!frame) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT async.resolve coroutine frame is invalid");
        }
        frame->completed = true;
        const runtime::Value val = optionalPayload
            ? runtime::detachFrameSliceValue(*optionalPayload)
            : runtime::Value{};
        (void)asyncRuntime_->awaitables().trySetResult(frame->completionHandle, val);
        if (options_.awaitScheduler) {
            runtime::RuntimeError schedErr;
            if (options_.awaitScheduler->tryResolveAsyncReturn(
                    returnStructTypeId,
                    optionalPayload ? &val : nullptr,
                    outWrappedResult,
                    schedErr)) {
                return true;
            }
            if (schedErr.code != runtime::ErrorCode::None) {
                return fail(schedErr.code, schedErr.message);
            }
        }
        outWrappedResult = makePendingAsyncReturnCarrier(
            returnStructTypeId, frame->completionHandle);
        return true;
    }
    if (options_.awaitScheduler) {
        runtime::RuntimeError schedErr;
        const runtime::Value detached = optionalPayload
            ? runtime::detachFrameSliceValue(*optionalPayload)
            : runtime::Value{};
        if (options_.awaitScheduler->tryResolveAsyncReturn(
                returnStructTypeId,
                optionalPayload ? &detached : nullptr,
                outWrappedResult,
                schedErr)) {
            return true;
        }
        if (schedErr.code != runtime::ErrorCode::None) {
            return fail(schedErr.code, schedErr.message);
        }
    }
    if (!asyncRuntime_) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT async.resolve requires an AsyncRuntimeContext");
    }
    const runtime::AwaitableHandle h = optionalPayload
        ? asyncRuntime_->awaitables().fromResult(runtime::detachFrameSliceValue(*optionalPayload))
        : runtime::AwaitableHandle::completedVoid();
    outWrappedResult = makePendingAsyncReturnCarrier(returnStructTypeId, h);
    return true;
}

bool ExecutionContext::finishUncaughtAsyncException(
    runtime::CoroutineFrameHandle activeCoroutine,
    semantic::SymbolId returnStructTypeId,
    runtime::Value& outResult) {
    if (!asyncRuntime_ || !pendingException_.has_value()) {
        return false;
    }
    const runtime::Value excVal = *pendingException_;
    pendingException_.reset();
    error_ = runtime::RuntimeError{};

    runtime::ManagedHeap* excHeap = heap_.get();
    if (const auto* excRef = std::get_if<runtime::ObjectRef>(&excVal)) {
        excHeap = resolveHeap(*excRef);
        if (!excHeap) excHeap = heap_.get();
    }
    const bool isCanceled = runtime::isAsyncCanceledExceptionObject(excHeap, excVal);
    if (activeCoroutine.valid()) {
        auto* frame = asyncRuntime_->coroutines().resolve(activeCoroutine);
        if (!frame) return false;
        frame->completed = true;
        if (isCanceled) {
            (void)asyncRuntime_->awaitables().trySetCanceled(frame->completionHandle);
        } else {
            std::string msg = extractAotExceptionMessage(typesPtr_, excHeap, excVal);
            (void)asyncRuntime_->awaitables().trySetFaulted(
                frame->completionHandle,
                runtime::ErrorCode::ScriptException,
                std::move(msg),
                excVal);
        }
        outResult = makePendingAsyncReturnCarrier(
            returnStructTypeId, frame->completionHandle);
        return true;
    }
    const runtime::AwaitableHandle h = isCanceled
        ? asyncRuntime_->awaitables().fromCanceled()
        : asyncRuntime_->awaitables().fromFaulted(
              runtime::ErrorCode::ScriptException,
              extractAotExceptionMessage(typesPtr_, excHeap, excVal),
              excVal);
    outResult = makePendingAsyncReturnCarrier(returnStructTypeId, h);
    return true;
}

bool ExecutionContext::enterResumingScalarCoroutineSlow(
    semantic::SymbolId functionSymbolId,
    void* outScalarBuffer,
    std::size_t scalarByteSize,
    runtime::Value* outRefSlots,
    std::size_t refCount,
    std::uint32_t& outResumeBlockId,
    runtime::CoroutineFrameHandle& outActiveCoroutine) {
    if (resumingFramePtr_ == nullptr ||
        !resumingCoroutine_.valid() ||
        resumingFramePtr_->functionSymbolId != functionSymbolId ||
        !resumingFramePtr_->usesScalarBuffer) {
        return false;
    }
    auto* frame = resumingFramePtr_;
    outActiveCoroutine = resumingCoroutine_;
    resumingCoroutine_ = runtime::CoroutineFrameHandle::invalid();
    resumingFramePtr_ = nullptr;

    if (scalarByteSize > 0 && outScalarBuffer != nullptr) {
        const std::size_t copyBytes = std::min<std::size_t>(
            scalarByteSize, static_cast<std::size_t>(frame->scalarByteSize));
        if (copyBytes > 0) {
            std::memcpy(outScalarBuffer, frame->scalarBufferData(), copyBytes);
        }
    }
    if (refCount > 0 && outRefSlots != nullptr) {
        const std::size_t nRefs = std::min(refCount, frame->savedLocals.size());
        for (std::size_t i = 0; i < nRefs; ++i) {
            outRefSlots[i] = frame->savedLocals[i];
        }
    }
    outResumeBlockId = frame->resumeBlockId;
    return true;
}

bool ExecutionContext::awaitSuspendScalar(
    semantic::SymbolId functionSymbolId,
    semantic::SymbolId returnStructTypeId,
    std::int64_t packedAwaitable,
    std::uint32_t resumeBlockId,
    const void* scalarData,
    std::size_t scalarByteSize,
    const runtime::Value* refSlots,
    std::size_t refCount,
    runtime::CoroutineFrameHandle& ioActiveCoroutine,
    bool& outSuspendedNow,
    std::int64_t& outPackedResult) {
    outSuspendedNow = false;
    const auto awaitedHandle = runtime::AwaitableHandle::unpack64(
        static_cast<std::uint64_t>(packedAwaitable));
    if (!asyncRuntime_ || !asyncRuntime_->awaitables().isValid(awaitedHandle)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT awaited value does not hold a valid Awaitable handle");
    }
    if (asyncRuntime_->awaitables().isCompleted(awaitedHandle)) {
        return true;
    }

    runtime::AwaitableHandle completionHandle{};
    if (!prepareCoroutineFrame(
            functionSymbolId,
            returnStructTypeId,
            awaitedHandle,
            ioActiveCoroutine,
            completionHandle)) {
        return false;
    }

    struct AotScalarRunnerThunk {
        static void run(
            void* runnerContext,
            runtime::CoroutineFrameHandle frameHandle,
            runtime::CoroutineFrame& frame,
            runtime::AsyncRuntimeContext& asyncContext) {
            ExecutionContext::resumeCoroutineThunk(
                runnerContext,
                frameHandle,
                frame,
                asyncContext,
                "resumed AOT scalar coroutine failed with runtime error");
        }
    };

    ensureResumeEnv();

    std::shared_ptr<const void> ownerCopy{};
    const auto* curFrame = asyncRuntime_->coroutines().resolve(ioActiveCoroutine);
    if (!curFrame || curFrame->runnerOwner.get() != resumeEnv_.get()) {
        ownerCopy = resumeEnv_;
    }

    if (!asyncRuntime_->coroutines().suspendScalarFrame(
            ioActiveCoroutine,
            scalarData,
            scalarByteSize,
            refSlots,
            refCount,
            resumeBlockId,
            awaitedHandle,
            &AotScalarRunnerThunk::run,
            resumeEnv_.get(),
            std::move(ownerCopy))) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT failed to suspend scalar coroutine onto awaited handle");
    }

    outPackedResult = static_cast<std::int64_t>(completionHandle.pack64());
    outSuspendedNow = true;
    return true;
}

bool ExecutionContext::awaitResumeVoidScalarSlow(std::int64_t& ioPackedAwaitable) {
    if (!asyncRuntime_) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume requires AsyncRuntimeContext");
    }
    const auto awaitedHandle = runtime::AwaitableHandle::unpack64(
        static_cast<std::uint64_t>(ioPackedAwaitable));
    ioPackedAwaitable = 0;
    runtime::Value payload{};
    runtime::RuntimeError faultErr{};
    runtime::Value excObj{};
    const auto st = asyncRuntime_->awaitables().getResult(awaitedHandle, payload, faultErr, &excObj);
    if (st == runtime::AwaitStatus::Succeeded) return true;
    if (st == runtime::AwaitStatus::Canceled || st == runtime::AwaitStatus::Faulted) {
        const auto matRef = runtime::materializeAsyncResumeException(heap_.get(), st, faultErr, excObj);
        if (!matRef) return fail(runtime::ErrorCode::OutOfMemory, "failed to allocate AOT async exception");
        pendingException_ = runtime::Value(*matRef);
        return false;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume encountered non-terminal Awaitable");
}

bool ExecutionContext::awaitResumeInt64Scalar(
    std::int64_t& ioPackedAwaitable,
    semantic::PrimitiveType expectedType,
    std::int64_t& outValue) {
    if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume requires AsyncRuntimeContext");
    }
    const auto awaitedHandle = runtime::AwaitableHandle::unpack64(
        static_cast<std::uint64_t>(ioPackedAwaitable));
    ioPackedAwaitable = 0;
    runtime::Value payload{};
    runtime::RuntimeError faultErr{};
    runtime::Value excObj{};
    const auto st = asyncRuntime_->awaitables().getResult(awaitedHandle, payload, faultErr, &excObj);
    if (RS_AOT_LIKELY(st == runtime::AwaitStatus::Succeeded)) {
        if (expectedType == semantic::PrimitiveType::Long) {
            if (const auto* lv = std::get_if<runtime::LongValue>(&payload)) {
                outValue = lv->value;
                return true;
            }
        } else if (const auto* iv = std::get_if<std::int64_t>(&payload)) {
            outValue = *iv;
            return true;
        }
        return fail(runtime::ErrorCode::TypeMismatch, "AOT await result integer type mismatch");
    }
    if (st == runtime::AwaitStatus::Canceled || st == runtime::AwaitStatus::Faulted) {
        const auto matRef = runtime::materializeAsyncResumeException(heap_.get(), st, faultErr, excObj);
        if (!matRef) return fail(runtime::ErrorCode::OutOfMemory, "failed to allocate AOT async exception");
        pendingException_ = runtime::Value(*matRef);
        return false;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume encountered non-terminal Awaitable");
}

bool ExecutionContext::awaitResumeDoubleScalar(
    std::int64_t& ioPackedAwaitable,
    double& outValue) {
    if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume requires AsyncRuntimeContext");
    }
    const auto awaitedHandle = runtime::AwaitableHandle::unpack64(
        static_cast<std::uint64_t>(ioPackedAwaitable));
    ioPackedAwaitable = 0;
    runtime::Value payload{};
    runtime::RuntimeError faultErr{};
    runtime::Value excObj{};
    const auto st = asyncRuntime_->awaitables().getResult(awaitedHandle, payload, faultErr, &excObj);
    if (RS_AOT_LIKELY(st == runtime::AwaitStatus::Succeeded)) {
        if (const auto* dv = std::get_if<double>(&payload)) {
            outValue = *dv;
            return true;
        }
        return fail(runtime::ErrorCode::TypeMismatch, "AOT await result double type mismatch");
    }
    if (st == runtime::AwaitStatus::Canceled || st == runtime::AwaitStatus::Faulted) {
        const auto matRef = runtime::materializeAsyncResumeException(heap_.get(), st, faultErr, excObj);
        if (!matRef) return fail(runtime::ErrorCode::OutOfMemory, "failed to allocate AOT async exception");
        pendingException_ = runtime::Value(*matRef);
        return false;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume encountered non-terminal Awaitable");
}

bool ExecutionContext::awaitResumeBoolScalar(
    std::int64_t& ioPackedAwaitable,
    bool& outValue) {
    if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume requires AsyncRuntimeContext");
    }
    const auto awaitedHandle = runtime::AwaitableHandle::unpack64(
        static_cast<std::uint64_t>(ioPackedAwaitable));
    ioPackedAwaitable = 0;
    runtime::Value payload{};
    runtime::RuntimeError faultErr{};
    runtime::Value excObj{};
    const auto st = asyncRuntime_->awaitables().getResult(awaitedHandle, payload, faultErr, &excObj);
    if (RS_AOT_LIKELY(st == runtime::AwaitStatus::Succeeded)) {
        if (const auto* bv = std::get_if<bool>(&payload)) {
            outValue = *bv;
            return true;
        }
        return fail(runtime::ErrorCode::TypeMismatch, "AOT await result bool type mismatch");
    }
    if (st == runtime::AwaitStatus::Canceled || st == runtime::AwaitStatus::Faulted) {
        const auto matRef = runtime::materializeAsyncResumeException(heap_.get(), st, faultErr, excObj);
        if (!matRef) return fail(runtime::ErrorCode::OutOfMemory, "failed to allocate AOT async exception");
        pendingException_ = runtime::Value(*matRef);
        return false;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume encountered non-terminal Awaitable");
}

bool ExecutionContext::awaitResumeBoxedScalar(
    std::int64_t& ioPackedAwaitable,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    runtime::Value& outValue) {
    if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume requires AsyncRuntimeContext");
    }
    const auto awaitedHandle = runtime::AwaitableHandle::unpack64(
        static_cast<std::uint64_t>(ioPackedAwaitable));
    ioPackedAwaitable = 0;
    runtime::Value payload{};
    runtime::RuntimeError faultErr{};
    runtime::Value excObj{};
    const auto st = asyncRuntime_->awaitables().getResult(awaitedHandle, payload, faultErr, &excObj);
    if (RS_AOT_LIKELY(st == runtime::AwaitStatus::Succeeded)) {
        if (!expectSignatureType(payload, expectedType, expectedTypeId, "await result")) {
            return false;
        }
        outValue = std::move(payload);
        return true;
    }
    if (st == runtime::AwaitStatus::Canceled || st == runtime::AwaitStatus::Faulted) {
        const auto matRef = runtime::materializeAsyncResumeException(heap_.get(), st, faultErr, excObj);
        if (!matRef) return fail(runtime::ErrorCode::OutOfMemory, "failed to allocate AOT async exception");
        pendingException_ = runtime::Value(*matRef);
        return false;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "AOT await.resume encountered non-terminal Awaitable");
}

bool ExecutionContext::asyncResolveVoidScalarSlow(
    runtime::CoroutineFrameHandle activeCoroutine,
    std::int64_t& outPackedResult) {
    if (!asyncRuntime_) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve requires AsyncRuntimeContext");
    }
    if (activeCoroutine.valid()) {
        auto* frame = asyncRuntime_->coroutines().resolve(activeCoroutine);
        if (!frame) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve coroutine frame is invalid");
        }
        frame->completed = true;
        (void)asyncRuntime_->awaitables().trySetResult(frame->completionHandle, runtime::Value{});
        outPackedResult = static_cast<std::int64_t>(frame->completionHandle.pack64());
        return true;
    }
    outPackedResult = static_cast<std::int64_t>(
        runtime::AwaitableHandle::completedVoid().pack64());
    return true;
}

bool ExecutionContext::asyncResolveIntScalar(
    runtime::CoroutineFrameHandle activeCoroutine,
    std::int64_t payload,
    bool isLong,
    std::int64_t& outPackedResult) {
    if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve requires AsyncRuntimeContext");
    }
    const runtime::Value val = isLong
        ? runtime::Value{runtime::LongValue{payload}}
        : runtime::Value{payload};
    if (activeCoroutine.valid()) {
        auto* frame = asyncRuntime_->coroutines().resolve(activeCoroutine);
        if (RS_AOT_UNLIKELY(!frame)) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve coroutine frame is invalid");
        }
        frame->completed = true;
        (void)asyncRuntime_->awaitables().trySetResult(frame->completionHandle, val);
        outPackedResult = static_cast<std::int64_t>(frame->completionHandle.pack64());
        return true;
    }
    const auto h = asyncRuntime_->awaitables().fromResult(val);
    outPackedResult = static_cast<std::int64_t>(h.pack64());
    return true;
}

bool ExecutionContext::asyncResolveDoubleScalar(
    runtime::CoroutineFrameHandle activeCoroutine,
    double payload,
    std::int64_t& outPackedResult) {
    if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve requires AsyncRuntimeContext");
    }
    const runtime::Value val{payload};
    if (activeCoroutine.valid()) {
        auto* frame = asyncRuntime_->coroutines().resolve(activeCoroutine);
        if (RS_AOT_UNLIKELY(!frame)) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve coroutine frame is invalid");
        }
        frame->completed = true;
        (void)asyncRuntime_->awaitables().trySetResult(frame->completionHandle, val);
        outPackedResult = static_cast<std::int64_t>(frame->completionHandle.pack64());
        return true;
    }
    const auto h = asyncRuntime_->awaitables().fromResult(val);
    outPackedResult = static_cast<std::int64_t>(h.pack64());
    return true;
}

bool ExecutionContext::asyncResolveBoolScalar(
    runtime::CoroutineFrameHandle activeCoroutine,
    bool payload,
    std::int64_t& outPackedResult) {
    if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve requires AsyncRuntimeContext");
    }
    const runtime::Value val{payload};
    if (activeCoroutine.valid()) {
        auto* frame = asyncRuntime_->coroutines().resolve(activeCoroutine);
        if (RS_AOT_UNLIKELY(!frame)) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve coroutine frame is invalid");
        }
        frame->completed = true;
        (void)asyncRuntime_->awaitables().trySetResult(frame->completionHandle, val);
        outPackedResult = static_cast<std::int64_t>(frame->completionHandle.pack64());
        return true;
    }
    const auto h = asyncRuntime_->awaitables().fromResult(val);
    outPackedResult = static_cast<std::int64_t>(h.pack64());
    return true;
}

bool ExecutionContext::asyncResolveBoxedScalar(
    runtime::CoroutineFrameHandle activeCoroutine,
    const runtime::Value& payload,
    std::int64_t& outPackedResult) {
    if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
        return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve requires AsyncRuntimeContext");
    }
    if (activeCoroutine.valid()) {
        auto* frame = asyncRuntime_->coroutines().resolve(activeCoroutine);
        if (RS_AOT_UNLIKELY(!frame)) {
            return fail(runtime::ErrorCode::InvalidProgram, "AOT async.resolve coroutine frame is invalid");
        }
        frame->completed = true;
        (void)asyncRuntime_->awaitables().trySetResult(frame->completionHandle, payload);
        outPackedResult = static_cast<std::int64_t>(frame->completionHandle.pack64());
        return true;
    }
    const auto h = asyncRuntime_->awaitables().fromResult(payload);
    outPackedResult = static_cast<std::int64_t>(h.pack64());
    return true;
}

bool ExecutionContext::finishUncaughtAsyncExceptionScalar(
    runtime::CoroutineFrameHandle activeCoroutine,
    semantic::SymbolId returnStructTypeId,
    std::int64_t& outPackedResult) {
    (void)returnStructTypeId;
    if (!asyncRuntime_ || !pendingException_.has_value()) {
        return false;
    }
    const runtime::Value excVal = *pendingException_;
    pendingException_.reset();
    error_ = runtime::RuntimeError{};

    runtime::ManagedHeap* excHeap = heap_.get();
    if (const auto* excRef = std::get_if<runtime::ObjectRef>(&excVal)) {
        excHeap = resolveHeap(*excRef);
        if (!excHeap) excHeap = heap_.get();
    }
    const bool isCanceled = runtime::isAsyncCanceledExceptionObject(excHeap, excVal);
    if (activeCoroutine.valid()) {
        auto* frame = asyncRuntime_->coroutines().resolve(activeCoroutine);
        if (!frame) return false;
        frame->completed = true;
        if (isCanceled) {
            (void)asyncRuntime_->awaitables().trySetCanceled(frame->completionHandle);
        } else {
            std::string msg = extractAotExceptionMessage(typesPtr_, excHeap, excVal);
            (void)asyncRuntime_->awaitables().trySetFaulted(
                frame->completionHandle,
                runtime::ErrorCode::ScriptException,
                std::move(msg),
                excVal);
        }
        outPackedResult = static_cast<std::int64_t>(frame->completionHandle.pack64());
        return true;
    }
    const runtime::AwaitableHandle h = isCanceled
        ? asyncRuntime_->awaitables().fromCanceled()
        : asyncRuntime_->awaitables().fromFaulted(
              runtime::ErrorCode::ScriptException,
              extractAotExceptionMessage(typesPtr_, excHeap, excVal),
              excVal);
    outPackedResult = static_cast<std::int64_t>(h.pack64());
    return true;
}

void Program::setBindings(
    std::shared_ptr<const runtime::BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
    resumeEnv_.reset();
}

void Program::setHeap(std::shared_ptr<runtime::ManagedHeap> heap) {
    if (lifecycleTarget_) {
        std::shared_ptr<runtime::ManagedHeap> regHeap;
        std::uint64_t token = 0;
        {
            std::lock_guard<std::mutex> lock(lifecycleTarget_->mutex);
            regHeap = lifecycleTarget_->registeredHeap.lock();
            token = lifecycleTarget_->sweepToken;
            lifecycleTarget_->registeredHeap.reset();
            lifecycleTarget_->sweepToken = 0;
        }
        if (regHeap && token != 0) {
            regHeap->removeValueLifecycleSweepHandler(token);
        }
    }
    heap_ = heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>();
    if (asyncRuntime_) asyncRuntime_->setHeap(heap_.get());
    resumeEnv_.reset();
    registerValueLifecycleHooks();
}

void Program::registerValueLifecycleHooks() {
    if (lifecycleTarget_) {
        std::shared_ptr<runtime::ManagedHeap> regHeap;
        std::uint64_t token = 0;
        {
            std::lock_guard<std::mutex> lock(lifecycleTarget_->mutex);
            regHeap = lifecycleTarget_->registeredHeap.lock();
            token = lifecycleTarget_->sweepToken;
            lifecycleTarget_->registeredHeap.reset();
            lifecycleTarget_->sweepToken = 0;
        }
        if (regHeap && token != 0) {
            regHeap->removeValueLifecycleSweepHandler(token);
        }
    }
    if (!heap_) return;
    std::unordered_map<semantic::SymbolId, semantic::SymbolId> disposeMap;
    const auto canonicalTypeName = [](const TypeDescriptor& td) -> std::string {
        if (td.moduleName && td.moduleName[0] != '\0') {
            return std::string(td.moduleName) + "::" + (td.name ? td.name : "");
        }
        return td.name ? std::string(td.name) : std::string();
    };
    for (const auto& [typeId, tdPtr] : types_) {
        if (!tdPtr || tdPtr->kind != semantic::TypeKind::Struct) continue;
        const std::string canonical = canonicalTypeName(*tdPtr);
        const bool isLifecycle = tdPtr->hasValueLifecycle ||
            canonical == "Core::ResourceHandle" ||
            canonical.rfind("Core::ResourceHandle<", 0) == 0;
        if (!isLifecycle) continue;
        heap_->registerValueLifecycleType(typeId);
        const std::string disposeName = canonical + ".Dispose";
        semantic::SymbolId disposeId = 0;
        if (const auto found = names_.find(disposeName); found != names_.end()) {
            const auto fnIt = functions_.find(found->second);
            if (fnIt != functions_.end() && fnIt->second &&
                fnIt->second->parameterCount <= 1) {
                disposeId = found->second;
            }
        }
        if (disposeId == 0 && descriptor_ && descriptor_->functions) {
            for (std::uint32_t fi = 0; fi < descriptor_->functionCount; ++fi) {
                const auto& fn = descriptor_->functions[fi];
                if (fn.name && disposeName == fn.name && fn.parameterCount <= 1) {
                    disposeId = fn.symbolId;
                    break;
                }
            }
        }
        if (disposeId != 0) {
            disposeMap.emplace(typeId, disposeId);
        }
    }
    if (disposeMap.empty()) {
        return;
    }
    if (!lifecycleTarget_) {
        lifecycleTarget_ = std::make_shared<LifecycleTarget>();
        lifecycleTarget_->program = this;
        lifecycleTarget_->alive = true;
    }
    const std::uint64_t sweepToken = heap_->addValueLifecycleSweepHandler(
        [weakTarget = std::weak_ptr<LifecycleTarget>(lifecycleTarget_),
         disposeMap = std::move(disposeMap),
         heapPtr = heap_.get()](const runtime::Value& sweptValue) -> bool {
            if (tlsInAotValueLifecycleDispose) return true;
            if (sweptValue.rawTag() != runtime::ValueTag::Struct) return false;
            const auto locked = weakTarget.lock();
            if (!locked) return false;
            std::lock_guard<std::mutex> lock(locked->mutex);
            const Program* self = locked->program;
            if (!locked->alive || !self || !self->heap_ || self->heap_.get() != heapPtr) {
                return false;
            }
            const auto& st = sweptValue.asStructUnchecked();
            const auto it = disposeMap.find(st.typeId);
            if (it == disposeMap.end() || it->second == 0) return false;
            const auto fnIt = self->functions_.find(it->second);
            if (fnIt == self->functions_.end() || !fnIt->second) return false;
            const FunctionDescriptor* disposeFn = fnIt->second;

            struct DisposeGuard {
                bool& flag;
                explicit DisposeGuard(bool& f) noexcept : flag(f) { flag = true; }
                ~DisposeGuard() noexcept { flag = false; }
            } guard(tlsInAotValueLifecycleDispose);

            runtime::ScopedValuePoolDomain domainScope(
                self->heap_->valuePoolDomain().get());
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            try {
#endif
                std::vector<runtime::Value> args;
                if (disposeFn->parameterCount > 0 && disposeFn->parameterTypes) {
                    if (disposeFn->parameterTypes[0] == semantic::PrimitiveType::Object &&
                        disposeFn->parameterTypeIds) {
                        const semantic::SymbolId wrapperTypeId =
                            disposeFn->parameterTypeIds[0];
                        if (wrapperTypeId == 0) return true;
                        const auto wrapped = self->heap_->allocateObject(
                            wrapperTypeId,
                            std::vector<runtime::Value>{sweptValue},
                            std::vector<std::size_t>{});
                        if (!wrapped) return true;
                        args.emplace_back(*wrapped);
                    } else {
                        args.push_back(sweptValue);
                    }
                }
                runtime::ExecutionOptions opts;
                opts.limits.instructionBudget = 1'000'000;
                opts.limits.recursionLimit = 128;
                (void)self->invoke(disposeFn->symbolId, args, std::move(opts));
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            } catch (...) {
            }
#endif
            return true;
        });

    bool stillAlive = true;
    {
        std::lock_guard<std::mutex> lock(lifecycleTarget_->mutex);
        stillAlive = lifecycleTarget_->alive;
        if (stillAlive) {
            lifecycleTarget_->registeredHeap = heap_;
            lifecycleTarget_->sweepToken = sweepToken;
        }
    }
    if (!stillAlive && sweepToken != 0) {
        heap_->removeValueLifecycleSweepHandler(sweepToken);
    }
}

std::shared_ptr<runtime::ManagedHeap> Program::heap() const noexcept {
    return heap_;
}

void Program::setExecutionPolicy(runtime::IExecutionPolicy* policy) noexcept {
    executionPolicy_ = policy;
    resumeEnv_.reset();
}

runtime::IExecutionPolicy* Program::executionPolicy() const noexcept {
    return executionPolicy_;
}

void Program::setAsyncRuntime(
    std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime) {
    asyncRuntime_ = asyncRuntime
        ? std::move(asyncRuntime)
        : std::make_shared<runtime::AsyncRuntimeContext>(heap_.get());
    if (heap_ && asyncRuntime_->awaitables().heap() == nullptr) {
        asyncRuntime_->setHeap(heap_.get());
    }
    resumeEnv_.reset();
}

std::shared_ptr<runtime::AsyncRuntimeContext> Program::asyncRuntime() const noexcept {
    return asyncRuntime_;
}

const ProgramDescriptor& Program::descriptor() const noexcept {
    return *descriptor_;
}

namespace {

bool validateMethodBodyHotPatchPlan(
    const hot_reload::ReloadPlan& plan,
    std::string& outError) {
    if (plan.structuralMigrationRequired) {
        outError =
            "AOT method-body hot-patch rejected: structuralMigrationRequired is true";
        return false;
    }
    if (!plan.layoutChangedTypes.empty()) {
        outError =
            "AOT method-body hot-patch rejected: layoutChangedTypes is non-empty";
        return false;
    }
    if (!plan.typeMigrations.empty()) {
        outError =
            "AOT method-body hot-patch rejected: typeMigrations is non-empty";
        return false;
    }
    if (!plan.addedTypes.empty() || !plan.removedTypes.empty()) {
        outError =
            "AOT method-body hot-patch rejected: type additions/removals are not supported";
        return false;
    }
    if (!plan.issues.empty()) {
        outError =
            "AOT method-body hot-patch rejected: " + plan.issues.front().message;
        return false;
    }
    if (!plan.compatible) {
        outError =
            "AOT method-body hot-patch rejected: ReloadPlan is marked incompatible";
        return false;
    }
    return true;
}

bool cstringsEqual(const char* a, const char* b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    return std::strcmp(a, b) == 0;
}

bool typeDescriptorsLayoutEquivalent(
    const TypeDescriptor& existing,
    const TypeDescriptor& replacement) noexcept {
    if (existing.id != replacement.id ||
        existing.kind != replacement.kind ||
        existing.baseTypeId != replacement.baseTypeId ||
        existing.fieldCount != replacement.fieldCount ||
        existing.virtualSlotCount != replacement.virtualSlotCount ||
        existing.interfaceType != replacement.interfaceType ||
        existing.delegateType != replacement.delegateType ||
        existing.hasValueLifecycle != replacement.hasValueLifecycle) {
        return false;
    }
    if (existing.fieldCount > 0 && (!existing.fields || !replacement.fields)) {
        return false;
    }
    for (std::uint32_t i = 0; i < existing.fieldCount; ++i) {
        const auto& ef = existing.fields[i];
        const auto& rf = replacement.fields[i];
        if (ef.type != rf.type ||
            ef.typeId != rf.typeId ||
            ef.index != rf.index ||
            ef.staticField != rf.staticField ||
            ef.constField != rf.constField ||
            !cstringsEqual(ef.name, rf.name)) {
            return false;
        }
    }
    return true;
}

bool functionSignaturesEquivalent(
    const FunctionDescriptor& existing,
    const FunctionDescriptor& replacement) noexcept {
    if (existing.symbolId != replacement.symbolId ||
        existing.returnType != replacement.returnType ||
        existing.returnTypeId != replacement.returnTypeId ||
        existing.parameterCount != replacement.parameterCount) {
        return false;
    }
    if (existing.parameterCount > 0 &&
        (!existing.parameterTypes || !existing.parameterTypeIds ||
         !replacement.parameterTypes || !replacement.parameterTypeIds)) {
        return false;
    }
    for (std::uint32_t p = 0; p < existing.parameterCount; ++p) {
        if (existing.parameterTypes[p] != replacement.parameterTypes[p] ||
            existing.parameterTypeIds[p] != replacement.parameterTypeIds[p]) {
            return false;
        }
    }
    return true;
}

} // namespace

HotPatchResult Program::applyMethodBodyHotPatch(
    const hot_reload::ReloadPlan& plan,
    const std::unordered_map<semantic::SymbolId, FunctionImplementation>& patchedBodies) {
    HotPatchResult result{};
    result.revision = hotPatchRevision_;
    if (!descriptor_) {
        result.message = "AOT program descriptor is null";
        return result;
    }
    if (!validateMethodBodyHotPatchPlan(plan, result.message)) {
        return result;
    }

    // Build validated list of (symbolId, targetImplementation) atomically before mutating anything.
    std::vector<std::pair<semantic::SymbolId, FunctionImplementation>> pendingUpdates;
    std::unordered_set<semantic::SymbolId> queuedIds;

    auto enqueueChecked = [&](semantic::SymbolId symId, FunctionImplementation impl) -> bool {
        if (symId == 0 || !impl) {
            result.message = "AOT method-body hot-patch rejected: null function implementation";
            return false;
        }
        if (functions_.find(symId) == functions_.end()) {
            result.message =
                "AOT method-body hot-patch rejected: target symbol is not present in loaded Program";
            return false;
        }
        if (queuedIds.insert(symId).second) {
            pendingUpdates.emplace_back(symId, impl);
        }
        return true;
    };

    if (!plan.changedFunctions.empty()) {
        for (const semantic::SymbolId symId : plan.changedFunctions) {
            const auto found = patchedBodies.find(symId);
            if (found == patchedBodies.end()) {
                result.message =
                    "AOT method-body hot-patch rejected: missing replacement body for changed function";
                return result;
            }
            if (!enqueueChecked(symId, found->second)) {
                return result;
            }
        }
        for (const auto& [symId, impl] : patchedBodies) {
            if (!enqueueChecked(symId, impl)) {
                return result;
            }
        }
    } else {
        for (const auto& [symId, impl] : patchedBodies) {
            if (!enqueueChecked(symId, impl)) {
                return result;
            }
        }
    }

    std::unordered_map<semantic::SymbolId, HotPatchSlot*> slotMap;
    if (descriptor_->hotPatchSlots != nullptr && descriptor_->hotPatchSlotCount > 0) {
        slotMap.reserve(descriptor_->hotPatchSlotCount);
        for (std::uint32_t i = 0; i < descriptor_->hotPatchSlotCount; ++i) {
            HotPatchSlot* slot = &descriptor_->hotPatchSlots[i];
            if (slot->symbolId != 0) {
                slotMap[slot->symbolId] = slot;
            }
        }
        for (const auto& [symId, impl] : pendingUpdates) {
            (void)impl;
            if (slotMap.find(symId) == slotMap.end()) {
                result.message =
                    "AOT method-body hot-patch rejected: target function has no emitted hot-patch slot";
                return result;
            }
        }
    }

    const std::uint32_t nextRevision = hotPatchRevision_ + 1u;
    for (const auto& [symId, impl] : pendingUpdates) {
        hotPatchedBodies_[symId] = impl;
        if (!slotMap.empty()) {
            HotPatchSlot* slot = slotMap.at(symId);
            bool alreadyTracked = false;
            for (const auto& rec : modifiedHotPatchSlots_) {
                if (rec.slot == slot) {
                    alreadyTracked = true;
                    break;
                }
            }
            if (!alreadyTracked) {
                modifiedHotPatchSlots_.push_back(ModifiedHotPatchSlotRecord{
                    slot,
                    slot->originalImplementation});
            }
            slot->patchedImplementation = impl;
            slot->revision = nextRevision;
        }
        result.patchedSymbolIds.push_back(symId);
    }

    hotPatchRevision_ = nextRevision;
    if (resumeEnv_) {
        static_cast<AotResumeEnv*>(resumeEnv_.get())->hotPatchedBodies = hotPatchedBodies_;
    }

    result.applied = true;
    result.revision = hotPatchRevision_;
    result.patchedMethodCount = result.patchedSymbolIds.size();
    result.message = "AOT method-body hot-patch applied";
    return result;
}

HotPatchResult Program::applyMethodBodyHotPatch(
    const hot_reload::ReloadPlan& plan,
    const ProgramDescriptor& replacementProgram) {
    HotPatchResult result{};
    result.revision = hotPatchRevision_;
    if (!descriptor_) {
        result.message = "AOT program descriptor is null";
        return result;
    }
    if (!validateMethodBodyHotPatchPlan(plan, result.message)) {
        return result;
    }
    if (replacementProgram.abiMajor != RuntimeAbiMajor) {
        result.message =
            "AOT method-body hot-patch rejected: incompatible replacement ABI major version";
        return result;
    }
    if (replacementProgram.typeCount > 0) {
        if (!replacementProgram.types) {
            result.message =
                "AOT method-body hot-patch rejected: replacement descriptor has null types array";
            return result;
        }
        if (replacementProgram.typeCount != types_.size()) {
            result.message =
                "AOT method-body hot-patch rejected: type table count differs from loaded Program";
            return result;
        }
        for (std::uint32_t i = 0; i < replacementProgram.typeCount; ++i) {
            const auto& repType = replacementProgram.types[i];
            const auto found = types_.find(repType.id);
            if (found == types_.end() ||
                !typeDescriptorsLayoutEquivalent(*found->second, repType)) {
                result.message =
                    "AOT method-body hot-patch rejected: struct or class type layout changed";
                return result;
            }
        }
    }
    if (replacementProgram.functionCount > 0 && !replacementProgram.functions) {
        result.message =
            "AOT method-body hot-patch rejected: replacement descriptor has null functions array";
        return result;
    }

    std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> repFunctions;
    repFunctions.reserve(replacementProgram.functionCount);
    for (std::uint32_t i = 0; i < replacementProgram.functionCount; ++i) {
        const auto& fn = replacementProgram.functions[i];
        if (fn.symbolId != 0) {
            repFunctions[fn.symbolId] = &fn;
        }
    }

    // Prefer raw `_body` entry pointer (`originalImplementation`) from replacement's hotPatchSlots
    // so patching never indirectly bounces back through an identically named thunk!
    std::unordered_map<semantic::SymbolId, FunctionImplementation> repRawBodies;
    if (replacementProgram.hotPatchSlots != nullptr &&
        replacementProgram.hotPatchSlotCount > 0) {
        repRawBodies.reserve(replacementProgram.hotPatchSlotCount);
        for (std::uint32_t i = 0; i < replacementProgram.hotPatchSlotCount; ++i) {
            const auto& slot = replacementProgram.hotPatchSlots[i];
            if (slot.symbolId != 0 && slot.originalImplementation != nullptr) {
                repRawBodies[slot.symbolId] = slot.originalImplementation;
            }
        }
    }

    std::unordered_map<semantic::SymbolId, FunctionImplementation> patchedBodies;
    auto collectTarget = [&](semantic::SymbolId symId) -> bool {
        const auto origIt = functions_.find(symId);
        const auto repIt = repFunctions.find(symId);
        if (origIt == functions_.end() || repIt == repFunctions.end()) {
            result.message =
                "AOT method-body hot-patch rejected: target symbol missing in replacement descriptor";
            return false;
        }
        if (!functionSignaturesEquivalent(*origIt->second, *repIt->second)) {
            result.message =
                "AOT method-body hot-patch rejected: function signature changed";
            return false;
        }
        const auto rawIt = repRawBodies.find(symId);
        const FunctionImplementation impl = (rawIt != repRawBodies.end())
            ? rawIt->second
            : repIt->second->implementation;
        if (!impl) {
            result.message =
                "AOT method-body hot-patch rejected: null replacement implementation";
            return false;
        }
        patchedBodies[symId] = impl;
        return true;
    };

    if (!plan.changedFunctions.empty()) {
        for (const semantic::SymbolId symId : plan.changedFunctions) {
            if (!collectTarget(symId)) return result;
        }
        for (const semantic::SymbolId symId : plan.inlineInvalidatedFunctions) {
            if (repFunctions.find(symId) != repFunctions.end() &&
                functions_.find(symId) != functions_.end()) {
                if (!collectTarget(symId)) return result;
            }
        }
    } else {
        for (const auto& [symId, repFn] : repFunctions) {
            (void)repFn;
            if (functions_.find(symId) != functions_.end()) {
                if (!collectTarget(symId)) return result;
            }
        }
    }

    return applyMethodBodyHotPatch(plan, patchedBodies);
}

runtime::ExecutionResult Program::invoke(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    runtime::ScopedValuePoolDomain domainScope(
        heap_ ? heap_->valuePoolDomain().get() : nullptr);
    if (!options.executionPolicy) {
        options.executionPolicy = executionPolicy_;
    }
    const runtime::ExecutionOptions invokeOptionsCopy = options;
    ExecutionContext context(
        *descriptor_,
        heap_,
        bindings_,
        std::move(options),
        &types_,
        &functions_);
    if (!resumeEnv_ ||
        static_cast<const AotResumeEnv*>(resumeEnv_.get())->options.awaitScheduler !=
            invokeOptionsCopy.awaitScheduler ||
        static_cast<const AotResumeEnv*>(resumeEnv_.get())->options.executionPolicy !=
            invokeOptionsCopy.executionPolicy) {
        auto env = std::make_shared<AotResumeEnv>();
        env->descriptor = descriptor_;
        env->heap = heap_;
        env->bindings = bindings_;
        env->options = invokeOptionsCopy;
        env->asyncRuntimeWeak = asyncRuntime_;
        env->ownedTypes = types_;
        env->ownedFunctions = functions_;
        env->hotPatchedBodies = hotPatchedBodies_;
        resumeEnv_ = std::move(env);
    } else {
        auto* existingEnv = static_cast<AotResumeEnv*>(resumeEnv_.get());
        existingEnv->options = invokeOptionsCopy;
        existingEnv->asyncRuntimeWeak = asyncRuntime_;
        existingEnv->hotPatchedBodies = hotPatchedBodies_;
    }
    context.setHotPatchedBodies(&hotPatchedBodies_);
    context.setAsyncRuntime(asyncRuntime_, resumeEnv_);
    void* const prevBridge = asyncRuntime_
        ? asyncRuntime_->activeAotContext()
        : nullptr;
    if (asyncRuntime_) {
        asyncRuntime_->setActiveAotContext(&context);
    }
    struct ActiveAotBridgeScope {
        runtime::AsyncRuntimeContext* ctx;
        void* prev;
        ~ActiveAotBridgeScope() noexcept {
            if (ctx) ctx->setActiveAotContext(prev);
        }
    } activeAotBridgeScope{asyncRuntime_.get(), prevBridge};
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
    if (execution.succeeded) {
        value = runtime::detachFrameSliceValue(value);
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

runtime::ParallelNativeBatchKernel Program::buildParallelBatchKernel(
    semantic::SymbolId symbolId,
    runtime::ParallelKernelKind kernelKind,
    const std::vector<runtime::ParallelNativeColumnBinding>& paramColumns,
    const runtime::ParallelJobConfig& config,
    bool usesParallelPrng) const {
    (void)kernelKind;
    (void)paramColumns;
    if (!descriptor_ || usesParallelPrng) {
        return {};
    }
    const auto& execOpts = config.executionOptions;
    if (execOpts.determinism.mode != runtime::DeterminismMode::Off ||
        execOpts.profile != nullptr ||
        static_cast<bool>(execOpts.trace) ||
        execOpts.executionPolicy != nullptr ||
        executionPolicy_ != nullptr ||
        execOpts.debugger != nullptr ||
        execOpts.limits.recursionLimit == 0) {
        return {};
    }
    const auto fnIt = functions_.find(symbolId);
    if (fnIt == functions_.end() || !fnIt->second || !fnIt->second->implementation) {
        return {};
    }
    const FunctionDescriptor* fnDesc = fnIt->second;
    const bool hasHotPatch = hotPatchedBodies_.find(symbolId) != hotPatchedBodies_.end();
    const ParallelBatchImplementation batchKernel = hasHotPatch
        ? nullptr
        : fnDesc->parallelBatchKernel;

    struct AotParallelBatchSnapshot {
        const ProgramDescriptor* descriptor = nullptr;
        std::unordered_map<semantic::SymbolId, const TypeDescriptor*> types;
        std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> functions;
        std::unordered_map<semantic::SymbolId, FunctionImplementation> hotPatchedBodies;
        std::shared_ptr<const runtime::BindingRegistry> bindings;
        std::shared_ptr<std::atomic<std::size_t>> batchCounter;
    };
    auto snapshot = std::make_shared<AotParallelBatchSnapshot>();
    snapshot->descriptor = descriptor_;
    snapshot->types = types_;
    snapshot->functions = functions_;
    snapshot->hotPatchedBodies = hotPatchedBodies_;
    snapshot->bindings = bindings_;
    snapshot->batchCounter = parallelBatchExecutions_;

    return [snapshot, symbolId, batchKernel](runtime::ParallelNativeBatchContext& ctx) -> bool {
        if (!snapshot || !snapshot->descriptor) {
            ctx.error.code = runtime::ErrorCode::InvalidProgram;
            ctx.error.message = "AOT program descriptor is not available";
            return false;
        }
        std::shared_ptr<runtime::ManagedHeap> batchHeap;
        if (runtime::ManagedHeap* rawHeap = ctx.mutableHeap
                ? ctx.mutableHeap
                : const_cast<runtime::ManagedHeap*>(ctx.heap)) {
            batchHeap = std::shared_ptr<runtime::ManagedHeap>(
                rawHeap,
                [](runtime::ManagedHeap*) {});
        }
        std::shared_ptr<const runtime::BindingRegistry> batchBindings = snapshot->bindings;
        if (ctx.bindings != nullptr) {
            batchBindings = std::shared_ptr<const runtime::BindingRegistry>(
                ctx.bindings,
                [](const runtime::BindingRegistry*) {});
        }
        runtime::ExecutionOptions batchOpts = ctx.config
            ? ctx.config->executionOptions
            : runtime::ExecutionOptions{};
        batchOpts.limits.gcWorkBudget = 0;
        batchOpts.debugger.reset();

        runtime::ScopedValuePoolDomain domainScope(
            batchHeap ? batchHeap->valuePoolDomain().get() : nullptr);
        ExecutionContext context(
            *snapshot->descriptor,
            std::move(batchHeap),
            std::move(batchBindings),
            std::move(batchOpts),
            &snapshot->types,
            &snapshot->functions);
        context.setHotPatchedBodies(&snapshot->hotPatchedBodies);
        if (snapshot->batchCounter) {
            snapshot->batchCounter->fetch_add(1, std::memory_order_relaxed);
        }
        return context.invokeParallelBatch(symbolId, batchKernel, ctx);
    };
}

runtime::ParallelNativeBatchKernel Program::buildParallelBatchKernel(
    const std::string& qualifiedName,
    runtime::ParallelKernelKind kernelKind,
    const std::vector<runtime::ParallelNativeColumnBinding>& paramColumns,
    const runtime::ParallelJobConfig& config,
    bool usesParallelPrng) const {
    const auto found = names_.find(qualifiedName);
    if (found == names_.end()) {
        return {};
    }
    return buildParallelBatchKernel(
        found->second,
        kernelKind,
        paramColumns,
        config,
        usesParallelPrng);
}

void Program::attachToProgramImage(const runtime::ProgramImage& program) const {
    if (!lifecycleTarget_) {
        const Program* selfPtr = this;
        program.setParallelBatchKernelFactory(
            [selfPtr](
                semantic::SymbolId symId,
                runtime::ParallelKernelKind kind,
                const std::vector<runtime::ParallelNativeColumnBinding>& cols,
                const runtime::ParallelJobConfig& cfg,
                bool usesPrng) -> runtime::ParallelNativeBatchKernel {
                return selfPtr->buildParallelBatchKernel(symId, kind, cols, cfg, usesPrng);
            });
        return;
    }
    std::weak_ptr<LifecycleTarget> weakTarget = lifecycleTarget_;
    program.setParallelBatchKernelFactory(
        [weakTarget](
            semantic::SymbolId symId,
            runtime::ParallelKernelKind kind,
            const std::vector<runtime::ParallelNativeColumnBinding>& cols,
            const runtime::ParallelJobConfig& cfg,
            bool usesPrng) -> runtime::ParallelNativeBatchKernel {
            const auto locked = weakTarget.lock();
            if (!locked) {
                return {};
            }
            const Program* prog = nullptr;
            {
                std::lock_guard<std::mutex> lock(locked->mutex);
                if (!locked->alive || !locked->program) {
                    return {};
                }
                prog = locked->program;
            }
            return prog->buildParallelBatchKernel(symId, kind, cols, cfg, usesPrng);
        });
}

std::size_t Program::parallelBatchExecutionCount() const noexcept {
    return parallelBatchExecutions_
        ? parallelBatchExecutions_->load(std::memory_order_relaxed)
        : 0u;
}

void Program::resetParallelBatchExecutionCount() const noexcept {
    if (parallelBatchExecutions_) {
        parallelBatchExecutions_->store(0u, std::memory_order_relaxed);
    }
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
