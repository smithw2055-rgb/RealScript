#include "InterpreterInternal.h"

namespace realscript::runtime {
namespace interp_detail {

namespace {
thread_local ThreadInvokeScratch tlsInvokeScratch;
} // namespace

void resumeInterpreterCoroutineCallback(
    void* runnerContext,
    CoroutineFrameHandle frameHandle,
    CoroutineFrame& frame,
    AsyncRuntimeContext& asyncContext) {
    ManagedHeap* resumeHeap = nullptr;
    if (auto* envPtr = static_cast<InterpreterResumeEnv*>(runnerContext)) {
        resumeHeap = envPtr->heap.get();
    }
    if (!resumeHeap) {
        if (void* activeRaw = asyncContext.activeInterpreterState()) {
            resumeHeap = static_cast<State*>(activeRaw)->heap;
        }
    }
    if (!resumeHeap) {
        resumeHeap = asyncContext.awaitables().heap();
    }
    ScopedValuePoolDomain domainScope(
        resumeHeap ? resumeHeap->valuePoolDomain().get() : nullptr);
    detail::ActiveMutatorScope activeMutatorScope;
    const auto faultUnresolvedFrame = [&](const RuntimeError& err) {
        if (auto* f = asyncContext.coroutines().resolve(frameHandle)) {
            if (f->invalidated) {
                f->completed = true;
                (void)asyncContext.awaitables().trySetCanceled(f->completionHandle);
                return;
            }
            if (!f->completed && !f->suspended) {
                f->completed = true;
                const ErrorCode code = (err.code != ErrorCode::None)
                    ? err.code
                    : ErrorCode::InvalidProgram;
                const std::string msg = !err.message.empty()
                    ? err.message
                    : std::string("resumed coroutine failed with runtime error");
                (void)asyncContext.awaitables().trySetFaulted(
                    f->completionHandle, code, msg);
            }
        }
    };

    if (void* activeRaw = asyncContext.activeInterpreterState()) {
        auto* activeState = static_cast<State*>(activeRaw);
        if (activeState->program != nullptr) {
            const auto* loc = activeState->program->resolveFunction(frame.functionSymbolId);
            if (loc) {
                const CoroutineFrameHandle savedCoro = activeState->resumingCoroutine;
                CoroutineFrame* const savedFramePtr = activeState->resumingFramePtr;
                const bool savedSuspendedNow = activeState->coroutineSuspendedNow;
                Value savedSuspendedRet = std::move(activeState->suspendedReturnValue);
                RuntimeError savedError = std::move(activeState->error);
                std::optional<Value> savedPendingExc = std::move(activeState->pendingException);
                IAwaitScheduler* const savedAwaitScheduler = activeState->awaitScheduler;

                if (auto* envPtr = static_cast<InterpreterResumeEnv*>(runnerContext)) {
                    activeState->awaitScheduler = envPtr->options.awaitScheduler;
                }
                activeState->resumingCoroutine = frameHandle;
                activeState->resumingFramePtr = &frame;
                activeState->coroutineSuspendedNow = false;
                activeState->suspendedReturnValue = Value{};
                activeState->error = RuntimeError{};
                activeState->pendingException.reset();

                Value ignoredResult{};
                const ArgumentSource emptyArgs{};
                const bool ok = executeFunction(*activeState, *loc, emptyArgs, ignoredResult);
                if (!ok) {
                    faultUnresolvedFrame(activeState->error);
                }

                activeState->awaitScheduler = savedAwaitScheduler;
                activeState->resumingCoroutine = savedCoro;
                activeState->resumingFramePtr = savedFramePtr;
                activeState->coroutineSuspendedNow = savedSuspendedNow;
                activeState->suspendedReturnValue = std::move(savedSuspendedRet);
                activeState->error = std::move(savedError);
                activeState->pendingException = std::move(savedPendingExc);
                return;
            }
        }
    }

    const std::shared_ptr<const void> keepEnvAlive = frame.runnerOwner;
    auto* env = static_cast<InterpreterResumeEnv*>(runnerContext);
    if (!env || !env->program) return;
    const auto* found = env->program->resolveFunction(frame.functionSymbolId);
    if (!found) return;

    std::array<InlineCallCacheEntry, 64> fallbackInlineCache;
    std::vector<PolymorphicPicSite> fallbackPolyCache;
    std::vector<DelegateCallSiteCache> fallbackDelCache;
    const auto& activeMirror = detail::executionMirror(*env->program);

    State state;
    state.mirror = &activeMirror;
    const std::uint64_t mirrorId = activeMirror.instanceId;
    const std::size_t polyCount = activeMirror.polyCallSites.size();
    const std::size_t delCount = activeMirror.invokeDelegateSites.size();
    const bool useTls = (tlsInvokeScratch.depth < ThreadInvokeScratch::MaxDepth);
    if (useTls) {
        auto& buf = tlsInvokeScratch.pool[tlsInvokeScratch.depth++];
        if (buf.values.size() < 256) buf.values.resize(256);
        if (buf.stack.capacity() < 32) buf.stack.reserve(32);
        if (buf.lastMirrorId != mirrorId ||
            buf.polyCallCache.size() != polyCount ||
            buf.delegateCallCache.size() != delCount) {
            buf.inlineCallCache.fill(InlineCallCacheEntry{});
            buf.polyCallCache.assign(polyCount, PolymorphicPicSite{});
            buf.delegateCallCache.assign(delCount, DelegateCallSiteCache{});
            buf.lastMirrorId = mirrorId;
        }
        state.values = std::move(buf.values);
        state.stack = std::move(buf.stack);
        state.inlineCallCache = &buf.inlineCallCache;
        state.polyCallCache = &buf.polyCallCache;
        state.delegateCallCache = &buf.delegateCallCache;
    } else {
        fallbackInlineCache.fill(InlineCallCacheEntry{});
        fallbackPolyCache.assign(polyCount, PolymorphicPicSite{});
        fallbackDelCache.assign(delCount, DelegateCallSiteCache{});
        state.inlineCallCache = &fallbackInlineCache;
        state.polyCallCache = &fallbackPolyCache;
        state.delegateCallCache = &fallbackDelCache;
    }

    struct TlsReleaseGuard {
        State& st;
        bool active;
        ~TlsReleaseGuard() noexcept {
            if (active) {
                if (st.values.size() > 4096) st.values.resize(256);
                st.stack.clear();
                auto& b = tlsInvokeScratch.pool[--tlsInvokeScratch.depth];
                b.values = std::move(st.values);
                b.stack = std::move(st.stack);
            }
        }
    } tlsGuard{state, useTls};

    ExecutionOptions resumeOpts = env->options;
    state.limits = resumeOpts.limits;
    state.program = env->program.get();
    state.externalResolver = &env->externalResolver;
    state.bindings = env->bindings.get();
    state.trace = &env->options.trace;
    state.profile = resumeOpts.profile.get();
    state.determinismEventsEnabled =
        resumeOpts.determinism.mode != DeterminismMode::Off;
    state.diagnosticEventsEnabled =
        state.trace && static_cast<bool>(*state.trace);
    state.determinismOnly = state.determinismEventsEnabled &&
        state.profile == nullptr && !state.diagnosticEventsEnabled;
    state.traceEventsEnabled =
        state.determinismEventsEnabled || state.profile != nullptr ||
        state.diagnosticEventsEnabled;
    state.fastAccounting = !state.traceEventsEnabled &&
        resumeOpts.debugger == nullptr;
    state.determinism = DeterminismSession(std::move(resumeOpts.determinism));
    state.heap = env->heap.get();
    state.shadowStack.setDomainId(
        (state.heap && state.heap->valuePoolDomain())
            ? state.heap->valuePoolDomain()->domainId()
            : 0u);
    state.debugger = resumeOpts.debugger.get();
    state.asyncRuntime = &asyncContext;
    state.awaitScheduler = resumeOpts.awaitScheduler;
    state.crossHeapPolicy = resumeOpts.crossHeapPolicy;
    const std::shared_ptr<const void> keepAlive = frame.runnerOwner;
    state.resumeEnvRaw = env;
    state.resumeEnvOwner = &keepAlive;
    state.resumingCoroutine = frameHandle;
    state.resumingFramePtr = &frame;

    void* const prevBridge = asyncContext.activeInterpreterState();
    asyncContext.setActiveInterpreterState(&state);
    struct RestoreBridge {
        AsyncRuntimeContext& ctx;
        void* prev;
        ~RestoreBridge() noexcept { ctx.setActiveInterpreterState(prev); }
    } restoreBridge{asyncContext, prevBridge};

    FlatStackBankScope flatBankScope(state);
    ManagedHeap::ActiveShadowStackScope activeHeapRoots(state.heap, &state.shadowStack);
    ShadowFrameScope roots(
        state.shadowStack, nullptr, nullptr, &state.values, &state.valueTop);
    if (state.traceEventsEnabled && state.heap && state.heap->isWallClockCycleActive()) {
        state.heap->finishWallClockCycleForDeterminism(state.shadowStack);
    }
    Value ignoredResult{};
    const ArgumentSource emptyArgs{};
    const bool ok = executeFunction(state, *found, emptyArgs, ignoredResult);
    ignoredResult = detachFrameSliceValue(ignoredResult);
    if (!ok) {
        faultUnresolvedFrame(state.error);
    }
}

} // namespace interp_detail

using namespace interp_detail;

namespace {

thread_local bool tlsInValueLifecycleDispose = false;

[[nodiscard]] bool endsWithSv(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] bool isValueLifecycleAttrName(std::string_view attrName) noexcept {
    if (attrName == "ValueLifecycle" || attrName == "ValueLifecycleAttribute") {
        return true;
    }
    return endsWithSv(attrName, "::ValueLifecycle") ||
        endsWithSv(attrName, ".ValueLifecycle") ||
        endsWithSv(attrName, "::ValueLifecycleAttribute") ||
        endsWithSv(attrName, ".ValueLifecycleAttribute");
}

[[nodiscard]] bool targetMatchesLifecycleStruct(
    std::string_view attrTarget,
    std::string_view candidate) noexcept {
    if (attrTarget.empty() || candidate.empty()) {
        return false;
    }
    if (attrTarget == candidate) {
        return true;
    }
    return candidate.size() > attrTarget.size() + 2u &&
        candidate.compare(0, attrTarget.size(), attrTarget) == 0 &&
        candidate[attrTarget.size()] == '_' &&
        candidate[attrTarget.size() + 1u] == '_';
}

} // namespace

void Interpreter::installValueLifecycleHooks(
    const std::shared_ptr<const ProgramImage>& program,
    const std::shared_ptr<ManagedHeap>& heap,
    std::shared_ptr<DispatchTarget> target) {
    if (!program || !heap || !target) {
        return;
    }
    std::vector<std::string> attrTargets;
    for (const auto& mod : program->modules()) {
        for (const auto& attr : mod.languageMetadata.attributes) {
            if (isValueLifecycleAttrName(attr.name) && !attr.target.empty()) {
                attrTargets.push_back(attr.target);
            }
        }
    }

    std::unordered_map<semantic::SymbolId, semantic::SymbolId> disposeByTypeId;
    for (const auto& mod : program->modules()) {
        for (const auto& ts : mod.types) {
            if (ts.kind != semantic::TypeKind::Struct || ts.id == 0) {
                continue;
            }
            const std::string canonical = semantic::canonicalTypeName(ts);
            bool hasLifecycle = ts.hasValueLifecycle;
            if (!hasLifecycle) {
                for (const auto& targetName : attrTargets) {
                    if (targetMatchesLifecycleStruct(targetName, canonical) ||
                        targetMatchesLifecycleStruct(targetName, ts.name)) {
                        hasLifecycle = true;
                        break;
                    }
                }
            }
            if (!hasLifecycle) {
                continue;
            }

            heap->registerValueLifecycleType(ts.id);

            semantic::SymbolId disposeId = 0;
            for (const auto& m : ts.methods) {
                // Non-static struct methods may include an implicit `this` entry in `m.parameters`.
                if (m.staticMethod || m.parameters.size() > 1u || m.id == 0) {
                    continue;
                }
                if (m.name == "Dispose" ||
                    endsWithSv(m.name, ".Dispose") ||
                    endsWithSv(m.name, "::Dispose")) {
                    if (program->resolveFunction(m.id) != nullptr) {
                        disposeId = m.id;
                        break;
                    }
                }
            }
            if (disposeId == 0) {
                if (const auto found = program->findFunction(canonical + ".Dispose")) {
                    disposeId = *found;
                } else if (const auto foundScope = program->findFunction(canonical + "::Dispose")) {
                    disposeId = *foundScope;
                }
            }
            if (disposeId != 0) {
                disposeByTypeId.emplace(ts.id, disposeId);
            }
        }
    }

    if (disposeByTypeId.empty()) {
        return;
    }

    heap->setValueLifecycleSweepCallback(
        [weakTarget = std::weak_ptr<DispatchTarget>(target),
         disposeMap = std::move(disposeByTypeId)](const Value& sweptValue) {
            if (tlsInValueLifecycleDispose || sweptValue.rawTag() != ValueTag::Struct) {
                return;
            }
            const auto locked = weakTarget.lock();
            if (!locked) {
                return;
            }
            Interpreter* rawInterp = nullptr;
            std::shared_ptr<const ProgramImage> prog;
            std::shared_ptr<ManagedHeap> targetHeap;
            std::shared_ptr<const BindingRegistry> bindings;
            std::shared_ptr<AsyncRuntimeContext> asyncRt;
            {
                std::lock_guard<std::mutex> guardLock(locked->mutex);
                if (!locked->alive) {
                    return;
                }
                rawInterp = locked->interpreter;
                if (rawInterp != nullptr) {
                    prog = rawInterp->program_;
                    targetHeap = rawInterp->heap_;
                } else {
                    prog = locked->program;
                    targetHeap = locked->heap;
                    bindings = locked->bindings;
                    asyncRt = locked->asyncRuntime;
                }
            }
            if (!prog || !targetHeap) {
                return;
            }
            const auto& st = sweptValue.asStructUnchecked();
            const auto it = disposeMap.find(st.typeId);
            if (it == disposeMap.end() || it->second == 0) {
                return;
            }
            const auto* loc = prog->resolveFunction(it->second);
            if (!loc || !loc->function) {
                return;
            }
            struct ReentrancyGuard {
                bool& flag;
                explicit ReentrancyGuard(bool& f) noexcept : flag(f) { flag = true; }
                ~ReentrancyGuard() noexcept { flag = false; }
                ReentrancyGuard(const ReentrancyGuard&) = delete;
                ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;
            } guard(tlsInValueLifecycleDispose);

            try {
                ExecutionOptions opts;
                opts.limits.instructionBudget = 1'000'000;
                opts.limits.recursionLimit = 128;
                std::vector<Value> args;
                if (!loc->function->parameterTypes.empty()) {
                    if (loc->function->parameterTypes[0] == semantic::PrimitiveType::Object) {
                        // Mutable struct methods lower `this` as a single-field reference-wrapper class.
                        const semantic::SymbolId wrapperTypeId =
                            loc->function->parameterTypeIds.empty()
                                ? 0
                                : loc->function->parameterTypeIds[0];
                        if (wrapperTypeId == 0) {
                            return;
                        }
                        std::vector<std::size_t> refFields;
                        if (sweptValue.isTraceable()) {
                            refFields.push_back(0);
                        }
                        RuntimeError allocErr;
                        const auto wrappedThis = targetHeap->allocateObject(
                            wrapperTypeId,
                            std::vector<Value>{sweptValue},
                            std::move(refFields),
                            &allocErr);
                        if (!wrappedThis) {
                            return;
                        }
                        args.push_back(*wrappedThis);
                    } else {
                        args.push_back(sweptValue);
                    }
                }
                if (rawInterp != nullptr) {
                    (void)rawInterp->invoke(it->second, args, std::move(opts));
                } else {
                    Interpreter transientInvoker(
                        EngineInvokeTag{},
                        std::move(prog),
                        std::move(targetHeap),
                        std::move(bindings),
                        std::move(asyncRt));
                    (void)transientInvoker.invoke(it->second, args, std::move(opts));
                }
            } catch (...) {
                // Sweep finalizers must never propagate errors out of GC cycles.
            }
        });
}

void Interpreter::registerValueLifecycleHooks() {
    if (!program_ || !heap_) {
        return;
    }
    if (!dispatchTarget_) {
        dispatchTarget_ = std::make_shared<DispatchTarget>();
    }
    {
        std::lock_guard<std::mutex> lock(dispatchTarget_->mutex);
        dispatchTarget_->interpreter = this;
        dispatchTarget_->alive = true;
    }
    installValueLifecycleHooks(program_, heap_, dispatchTarget_);
}

Interpreter::Interpreter(
    EngineInvokeTag,
    std::shared_ptr<const ProgramImage> program,
    std::shared_ptr<ManagedHeap> heap,
    std::shared_ptr<const BindingRegistry> bindings,
    std::shared_ptr<AsyncRuntimeContext> asyncRuntime)
    : program_(std::move(program)),
      bindings_(std::move(bindings)),
      heap_(heap ? std::move(heap) : std::make_shared<ManagedHeap>()),
      asyncRuntime_(std::move(asyncRuntime)) {
    if (!asyncRuntime_) {
        asyncRuntime_ = std::make_shared<AsyncRuntimeContext>(heap_.get());
    }
    if (heap_ && asyncRuntime_->awaitables().heap() == nullptr) {
        asyncRuntime_->setHeap(heap_.get());
    }
}

Interpreter::Interpreter(std::vector<bytecode::Module> modules)
    : heap_(std::make_shared<ManagedHeap>()),
      asyncRuntime_(std::make_shared<AsyncRuntimeContext>(heap_.get())),
      dispatchTarget_(std::make_shared<DispatchTarget>(this)) {
    auto linked = ProgramImage::link(std::move(modules), programError_);
    if (linked) {
        program_ = std::make_shared<ProgramImage>(std::move(*linked));
        if (heap_) heap_->registerTypeHierarchy(*program_);
        registerValueLifecycleHooks();
    }
}

Interpreter::Interpreter(std::shared_ptr<const ProgramImage> program)
    : Interpreter(std::move(program), std::make_shared<ManagedHeap>()) {}

Interpreter::Interpreter(
    std::shared_ptr<const ProgramImage> program,
    std::shared_ptr<ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<ManagedHeap>()),
      asyncRuntime_(std::make_shared<AsyncRuntimeContext>(heap_.get())),
      dispatchTarget_(std::make_shared<DispatchTarget>(this)) {
    if (!program_) {
        programError_.code = ErrorCode::InvalidProgram;
        programError_.message = "interpreter has no linked program image";
    } else if (heap_) {
        heap_->registerTypeHierarchy(*program_);
        registerValueLifecycleHooks();
    }
}

Interpreter::~Interpreter() {
    if (dispatchTarget_) {
        std::lock_guard<std::mutex> lock(dispatchTarget_->mutex);
        dispatchTarget_->interpreter = nullptr;
        dispatchTarget_->alive = false;
    }
}

Interpreter::Interpreter(const Interpreter& other)
    : program_(other.program_),
      programError_(other.programError_),
      externalResolver_(other.externalResolver_),
      bindings_(other.bindings_),
      heap_(other.heap_),
      asyncRuntime_(other.asyncRuntime_),
      dispatchTarget_(std::make_shared<DispatchTarget>(this)) {
    registerValueLifecycleHooks();
}

Interpreter& Interpreter::operator=(const Interpreter& other) {
    if (this == &other) {
        return *this;
    }
    if (dispatchTarget_) {
        std::lock_guard<std::mutex> lock(dispatchTarget_->mutex);
        dispatchTarget_->interpreter = nullptr;
        dispatchTarget_->alive = false;
    }
    program_ = other.program_;
    programError_ = other.programError_;
    externalResolver_ = other.externalResolver_;
    bindings_ = other.bindings_;
    heap_ = other.heap_;
    asyncRuntime_ = other.asyncRuntime_;
    resumeEnv_.reset();
    dispatchTarget_ = std::make_shared<DispatchTarget>(this);
    registerValueLifecycleHooks();
    return *this;
}

Interpreter::Interpreter(Interpreter&& other) noexcept
    : program_(std::move(other.program_)),
      programError_(std::move(other.programError_)),
      externalResolver_(std::move(other.externalResolver_)),
      bindings_(std::move(other.bindings_)),
      heap_(std::move(other.heap_)),
      asyncRuntime_(std::move(other.asyncRuntime_)),
      resumeEnv_(std::move(other.resumeEnv_)),
      dispatchTarget_(std::move(other.dispatchTarget_)) {
    if (dispatchTarget_) {
        std::lock_guard<std::mutex> lock(dispatchTarget_->mutex);
        dispatchTarget_->interpreter = this;
    }
}

Interpreter& Interpreter::operator=(Interpreter&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (dispatchTarget_) {
        std::lock_guard<std::mutex> lock(dispatchTarget_->mutex);
        dispatchTarget_->interpreter = nullptr;
        dispatchTarget_->alive = false;
    }
    program_ = std::move(other.program_);
    programError_ = std::move(other.programError_);
    externalResolver_ = std::move(other.externalResolver_);
    bindings_ = std::move(other.bindings_);
    heap_ = std::move(other.heap_);
    asyncRuntime_ = std::move(other.asyncRuntime_);
    resumeEnv_ = std::move(other.resumeEnv_);
    dispatchTarget_ = std::move(other.dispatchTarget_);
    if (dispatchTarget_) {
        std::lock_guard<std::mutex> lock(dispatchTarget_->mutex);
        dispatchTarget_->interpreter = this;
    }
    return *this;
}

void Interpreter::setExternalResolver(ExternalFunction resolver) {
    externalResolver_ = std::move(resolver);
    resumeEnv_.reset();
}

void Interpreter::setHeap(std::shared_ptr<ManagedHeap> heap) {
    heap_ = heap ? std::move(heap) : std::make_shared<ManagedHeap>();
    if (program_ && heap_) {
        heap_->registerTypeHierarchy(*program_);
        registerValueLifecycleHooks();
    }
    if (asyncRuntime_) asyncRuntime_->setHeap(heap_.get());
    resumeEnv_.reset();
}

std::shared_ptr<ManagedHeap> Interpreter::heap() const noexcept { return heap_; }

void Interpreter::setAsyncRuntime(std::shared_ptr<AsyncRuntimeContext> asyncRuntime) {
    asyncRuntime_ = asyncRuntime
        ? std::move(asyncRuntime)
        : std::make_shared<AsyncRuntimeContext>(heap_.get());
    if (heap_ && asyncRuntime_->awaitables().heap() == nullptr) {
        asyncRuntime_->setHeap(heap_.get());
    }
}

std::shared_ptr<AsyncRuntimeContext> Interpreter::asyncRuntime() const noexcept {
    return asyncRuntime_;
}

void Interpreter::setBindingRegistry(std::shared_ptr<const BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
    resumeEnv_.reset();
}

ExecutionResult Interpreter::invoke(
    semantic::SymbolId symbolId,
    const std::vector<Value>& arguments,
    Limits limits) const {
    ExecutionOptions options;
    options.limits = limits;
    return invoke(symbolId, arguments, std::move(options));
}

ExecutionResult Interpreter::invoke(
    semantic::SymbolId symbolId,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    if (!program_) {
        ExecutionResult invalid;
        invalid.error = programError_;
        if (invalid.error.code == ErrorCode::None) {
            invalid.error.code = ErrorCode::InvalidProgram;
            invalid.error.message = "interpreter has no linked program image";
        }
        return invalid;
    }
    const auto* found = program_->resolveFunction(symbolId);
    if (!found) {
        ExecutionResult missing;
        missing.error.code = ErrorCode::FunctionNotFound;
        missing.error.message = "entry function was not found";
        return missing;
    }
    ScopedValuePoolDomain domainScope(heap_ ? heap_->valuePoolDomain().get() : nullptr);
    detail::ActiveMutatorScope activeMutatorScope;
    auto& tlsScratch = tlsInvokeScratch;
    std::array<InlineCallCacheEntry, 64> fallbackInlineCache;
    std::vector<PolymorphicPicSite> fallbackPolyCache;
    std::vector<DelegateCallSiteCache> fallbackDelCache;
    const auto& activeMirror = detail::executionMirror(*program_);
    struct ScratchGuard {
        ThreadInvokeScratch& scratch;
        State& state;
        bool active = false;
        ScratchGuard(
            ThreadInvokeScratch& s,
            State& st,
            const detail::ExecutionMirror& mirror,
            std::array<InlineCallCacheEntry, 64>& fallback,
            std::vector<PolymorphicPicSite>& fallbackPoly,
            std::vector<DelegateCallSiteCache>& fallbackDel)
            : scratch(s), state(st) {
            state.mirror = &mirror;
            const std::uint64_t mirrorId = mirror.instanceId;
            const std::size_t polyCount = mirror.polyCallSites.size();
            const std::size_t delCount = mirror.invokeDelegateSites.size();
            if (scratch.depth < ThreadInvokeScratch::MaxDepth) {
                auto& buf = scratch.pool[scratch.depth++];
                if (buf.values.size() < 256) buf.values.resize(256);
                if (buf.stack.capacity() < 32) buf.stack.reserve(32);
                if (buf.lastMirrorId != mirrorId ||
                    buf.polyCallCache.size() != polyCount ||
                    buf.delegateCallCache.size() != delCount) {
                    buf.inlineCallCache.fill(InlineCallCacheEntry{});
                    buf.polyCallCache.assign(polyCount, PolymorphicPicSite{});
                    buf.delegateCallCache.assign(delCount, DelegateCallSiteCache{});
                    buf.lastMirrorId = mirrorId;
                }
                state.values = std::move(buf.values);
                state.stack = std::move(buf.stack);
                state.inlineCallCache = &buf.inlineCallCache;
                state.polyCallCache = &buf.polyCallCache;
                state.delegateCallCache = &buf.delegateCallCache;
                active = true;
            } else {
                fallback.fill(InlineCallCacheEntry{});
                fallbackPoly.assign(polyCount, PolymorphicPicSite{});
                fallbackDel.assign(delCount, DelegateCallSiteCache{});
                state.inlineCallCache = &fallback;
                state.polyCallCache = &fallbackPoly;
                state.delegateCallCache = &fallbackDel;
            }
        }
        ~ScratchGuard() noexcept {
            if (active) {
                if (state.values.size() > 4096) state.values.resize(256);
                state.stack.clear();
                auto& buf = scratch.pool[--scratch.depth];
                buf.values = std::move(state.values);
                buf.stack = std::move(state.stack);
            }
        }
    };

    State state;
    ScratchGuard scratchGuard(
        tlsScratch,
        state,
        activeMirror,
        fallbackInlineCache,
        fallbackPolyCache,
        fallbackDelCache);
    state.limits = options.limits;
    state.program = program_.get();
    state.externalResolver = &externalResolver_;
    state.bindings = bindings_.get();
    state.trace = &options.trace;
    state.profile = options.profile.get();
    state.determinismEventsEnabled =
        options.determinism.mode != DeterminismMode::Off;
    state.diagnosticEventsEnabled =
        state.trace && static_cast<bool>(*state.trace);
    state.determinismOnly = state.determinismEventsEnabled &&
        state.profile == nullptr && !state.diagnosticEventsEnabled;
    state.traceEventsEnabled =
        state.determinismEventsEnabled || state.profile != nullptr ||
        state.diagnosticEventsEnabled;
    state.fastAccounting = !state.traceEventsEnabled &&
        options.debugger == nullptr;
    if (options.enforceCapabilities) {
        options.determinism.enforceCapabilities = true;
        if (options.determinism.grantedCapabilities.empty()) {
            options.determinism.grantedCapabilities = std::move(options.grantedCapabilities);
        } else {
            options.determinism.grantedCapabilities.insert(
                options.grantedCapabilities.begin(),
                options.grantedCapabilities.end());
        }
    }
    if (!resumeEnv_ ||
        static_cast<const InterpreterResumeEnv*>(resumeEnv_.get())->options.awaitScheduler !=
            options.awaitScheduler) {
        auto env = std::make_shared<InterpreterResumeEnv>();
        env->program = program_;
        env->heap = heap_;
        env->bindings = bindings_;
        env->externalResolver = externalResolver_;
        env->options = options;
        env->asyncRuntimeWeak = asyncRuntime_;
        resumeEnv_ = env;
    } else {
        auto* env = static_cast<InterpreterResumeEnv*>(resumeEnv_.get());
        env->options = options;
        env->asyncRuntimeWeak = asyncRuntime_;
    }
    state.determinism = DeterminismSession(std::move(options.determinism));
    state.heap = heap_.get();
    state.shadowStack.setDomainId(
        (state.heap && state.heap->valuePoolDomain())
            ? state.heap->valuePoolDomain()->domainId()
            : 0u);
    state.debugger = options.debugger.get();
    state.asyncRuntime = asyncRuntime_.get();
    state.awaitScheduler = options.awaitScheduler;
    state.crossHeapPolicy = options.crossHeapPolicy;
    if (state.asyncRuntime && state.heap && state.asyncRuntime->awaitables().heap() == nullptr) {
        state.asyncRuntime->setHeap(state.heap);
    }
    const std::shared_ptr<const void> resumeOwnerRef = resumeEnv_;
    state.resumeEnvRaw = resumeEnv_.get();
    state.resumeEnvOwner = &resumeOwnerRef;
    void* const prevAsyncBridge = state.asyncRuntime
        ? state.asyncRuntime->activeInterpreterState()
        : nullptr;
    if (state.asyncRuntime) {
        state.asyncRuntime->setActiveInterpreterState(&state);
    }
    struct ActiveStateBridgeScope {
        AsyncRuntimeContext* ctx;
        void* prev;
        ~ActiveStateBridgeScope() noexcept {
            if (ctx) ctx->setActiveInterpreterState(prev);
        }
    } activeStateBridgeScope{state.asyncRuntime, prevAsyncBridge};
    struct DebugExecutionScope {
        debug::RuntimeDebugHook* debugger = nullptr;
        explicit DebugExecutionScope(debug::RuntimeDebugHook* d) : debugger(d) {
            if (debugger) debugger->beginExecution();
        }
        ~DebugExecutionScope() {
            if (debugger) debugger->endExecution();
        }
        DebugExecutionScope(const DebugExecutionScope&) = delete;
        DebugExecutionScope& operator=(const DebugExecutionScope&) = delete;
    } debugExecutionScope(state.debugger);
    if (state.debugger) {
        state.debugger->bindProgram(*program_);
    }
    Value value;
    ExecutionResult execution;
    {
        FlatStackBankScope flatBankScope(state);
        ManagedHeap::ActiveShadowStackScope activeHeapRoots(state.heap, &state.shadowStack);
        ShadowFrameScope roots(
            state.shadowStack, nullptr, nullptr, &state.values, &state.valueTop);
        if (state.traceEventsEnabled && state.heap && state.heap->isWallClockCycleActive()) {
            state.heap->finishWallClockCycleForDeterminism(state.shadowStack);
        }
        execution.succeeded = executeFunction(state, *found, arguments, value);
        value = detachFrameSliceValue(value);
    }
    state.statistics.instructionsExecuted = state.executed;
    if (!execution.succeeded && state.pendingException &&
        state.error.code == ErrorCode::None) {
        state.error.code = ErrorCode::ScriptException;
        state.error.message = "unhandled script exception";
        state.error.stackTrace = materializeStack(state.stack);
        populateInterpreterSourceLocations(state);
    }
    if (state.determinismEventsEnabled) {
        state.determinism.mergeEventDigest(
            state.determinismEventDigest,
            state.determinismEventCount);
    }
    execution.succeeded = state.determinism.finish(
        execution.succeeded,
        value,
        state.error,
        state.statistics);
    if (!execution.succeeded && state.heap && program_) {
        for (const auto& mod : program_->modules()) {
            for (const auto& ts : mod.types) {
                if (!ts.staticConstructor.has_value()) continue;
                const auto guardId = semantic::stableTypeId(
                    semantic::canonicalTypeName(ts) + "::cctor_guard");
                if (const auto guardVal = state.heap->getStaticField(guardId)) {
                    bool initialized = false;
                    if (guardVal->tryGetBool(initialized) && !initialized) {
                        (void)state.heap->removeStaticField(guardId);
                    }
                }
            }
        }
    }
    mergeProfile(state);
    execution.value = std::move(value);
    execution.error = std::move(state.error);
    execution.instructionsExecuted = state.executed;
    execution.statistics = state.statistics;
    execution.determinismDigest = state.determinism.digest();
    execution.replayEntriesConsumed = state.determinism.replayEntriesConsumed();
    return execution;
}

ExecutionResult Interpreter::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    Limits limits) const {
    ExecutionOptions options;
    options.limits = limits;
    return invoke(qualifiedName, arguments, std::move(options));
}

ExecutionResult Interpreter::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    if (!program_) {
        ExecutionResult invalid;
        invalid.error = programError_;
        if (invalid.error.code == ErrorCode::None) {
            invalid.error.code = ErrorCode::InvalidProgram;
            invalid.error.message = "interpreter has no linked program image";
        }
        return invalid;
    }
    if (const auto symbolId = program_->findFunction(qualifiedName)) {
        return invoke(*symbolId, arguments, std::move(options));
    }
    ExecutionResult missing;
    missing.error.code = ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

const char* errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None: return "none";
    case ErrorCode::FunctionNotFound: return "function-not-found";
    case ErrorCode::InvalidArguments: return "invalid-arguments";
    case ErrorCode::TypeMismatch: return "type-mismatch";
    case ErrorCode::DivisionByZero: return "division-by-zero";
    case ErrorCode::IntegerOverflow: return "integer-overflow";
    case ErrorCode::InstructionBudgetExceeded: return "instruction-budget-exceeded";
    case ErrorCode::RecursionLimitExceeded: return "recursion-limit-exceeded";
    case ErrorCode::ExternalFunctionUnresolved: return "external-function-unresolved";
    case ErrorCode::DuplicateSymbol: return "duplicate-symbol";
    case ErrorCode::InvalidObjectReference: return "invalid-object-reference";
    case ErrorCode::InvalidNativeHandle: return "invalid-native-handle";
    case ErrorCode::CrossHeapAccessViolation: return "cross-heap-access-violation";
    case ErrorCode::NullReference: return "null-reference";
    case ErrorCode::IndexOutOfRange: return "index-out-of-range";
    case ErrorCode::OutOfMemory: return "out-of-memory";
    case ErrorCode::InvalidProgram: return "invalid-program";
    case ErrorCode::ExecutionTerminated: return "execution-terminated";
    case ErrorCode::DeterminismViolation: return "determinism-violation";
    case ErrorCode::ReplayMismatch: return "replay-mismatch";
    case ErrorCode::ScriptException: return "script-exception";
    case ErrorCode::CapabilityDenied: return "capability-denied";
    }
    return "unknown";
}

bool tryConvertNumeric(
    const Value& value,
    semantic::PrimitiveType target,
    Value& result,
    bool checkedArithmetic) noexcept {
    long double number = 0.0L;
    std::uint64_t integerBits = 0;
    bool integerSource = true;
    if (const auto* byteValue = std::get_if<ByteValue>(&value)) {
        number = byteValue->value; integerBits = byteValue->value;
    } else if (const auto* sbyteValue = std::get_if<SByteValue>(&value)) {
        number = sbyteValue->value; integerBits = static_cast<std::uint64_t>(sbyteValue->value);
    } else if (const auto* shortValue = std::get_if<ShortValue>(&value)) {
        number = shortValue->value; integerBits = static_cast<std::uint64_t>(shortValue->value);
    } else if (const auto* ushortValue = std::get_if<UShortValue>(&value)) {
        number = ushortValue->value; integerBits = ushortValue->value;
    } else if (const auto* intValue = std::get_if<std::int64_t>(&value)) {
        number = static_cast<long double>(*intValue); integerBits = static_cast<std::uint64_t>(*intValue);
    } else if (const auto* uintValue = std::get_if<UIntValue>(&value)) {
        number = uintValue->value; integerBits = uintValue->value;
    } else if (const auto* longValue = std::get_if<LongValue>(&value)) {
        number = static_cast<long double>(longValue->value); integerBits = static_cast<std::uint64_t>(longValue->value);
    } else if (const auto* ulongValue = std::get_if<ULongValue>(&value)) {
        number = static_cast<long double>(ulongValue->value); integerBits = ulongValue->value;
    }
    else if (const auto* floatValue = std::get_if<FloatValue>(&value)) number = floatValue->value;
    else if (const auto* doubleValue = std::get_if<double>(&value)) number = *doubleValue;
    else if (const auto* charValue = std::get_if<CharValue>(&value)) {
        number = charValue->value; integerBits = charValue->value;
    }
    else return false;
    if (std::holds_alternative<FloatValue>(value) ||
        std::holds_alternative<double>(value)) integerSource = false;

    if (!checkedArithmetic && semantic::isIntegralType(target)) {
        if (!integerSource) {
            if (!std::isfinite(number)) integerBits = 0;
            else {
                const auto modulus = std::ldexp(1.0L,
                    target == semantic::PrimitiveType::Byte ||
                    target == semantic::PrimitiveType::SByte ? 8 :
                    target == semantic::PrimitiveType::Short ||
                    target == semantic::PrimitiveType::UShort ||
                    target == semantic::PrimitiveType::Char ? 16 :
                    target == semantic::PrimitiveType::Int ||
                    target == semantic::PrimitiveType::UInt ? 32 : 64);
                auto wrapped = std::fmod(std::trunc(number), modulus);
                if (wrapped < 0) wrapped += modulus;
                integerBits = static_cast<std::uint64_t>(wrapped);
            }
        }
        switch (target) {
        case semantic::PrimitiveType::Byte:
            result = ByteValue{static_cast<std::uint8_t>(integerBits)}; return true;
        case semantic::PrimitiveType::SByte: {
            const auto bits = static_cast<std::uint8_t>(integerBits);
            const auto signedValue = bits <= 0x7fU
                ? static_cast<std::int16_t>(bits)
                : static_cast<std::int16_t>(bits) - 0x100;
            result = SByteValue{static_cast<std::int8_t>(signedValue)}; return true;
        }
        case semantic::PrimitiveType::Short: {
            const auto bits = static_cast<std::uint16_t>(integerBits);
            const auto signedValue = bits <= 0x7fffU
                ? static_cast<std::int32_t>(bits)
                : static_cast<std::int32_t>(bits) - 0x10000;
            result = ShortValue{static_cast<std::int16_t>(signedValue)}; return true;
        }
        case semantic::PrimitiveType::UShort:
            result = UShortValue{static_cast<std::uint16_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Char:
            result = CharValue{static_cast<char16_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Int:
            result = wrapInt32(static_cast<std::int64_t>(integerBits)); return true;
        case semantic::PrimitiveType::UInt:
            result = UIntValue{static_cast<std::uint32_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Long:
            result = LongValue{signedFromBits(integerBits)}; return true;
        case semantic::PrimitiveType::ULong:
            result = ULongValue{integerBits}; return true;
        default:
            break;
        }
    }

    if (checkedArithmetic) {
        const auto inRange = [&](long double minimum, long double maximum) {
            return std::isfinite(number) && number >= minimum && number <= maximum;
        };
        switch (target) {
        case semantic::PrimitiveType::Byte:
            if (!inRange(0, 255)) return false;
            break;
        case semantic::PrimitiveType::SByte:
            if (!inRange(-128, 127)) return false;
            break;
        case semantic::PrimitiveType::Short:
            if (!inRange(-32768, 32767)) return false;
            break;
        case semantic::PrimitiveType::UShort:
        case semantic::PrimitiveType::Char:
            if (!inRange(0, 65535)) return false;
            break;
        case semantic::PrimitiveType::Int:
            if (!inRange(
                    std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max())) return false;
            break;
        case semantic::PrimitiveType::UInt:
            if (!inRange(0, std::numeric_limits<std::uint32_t>::max())) return false;
            break;
        case semantic::PrimitiveType::Long:
            if (!inRange(
                    static_cast<long double>(std::numeric_limits<std::int64_t>::min()),
                    static_cast<long double>(std::numeric_limits<std::int64_t>::max()))) return false;
            break;
        case semantic::PrimitiveType::ULong:
            if (!inRange(0, static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max()))) return false;
            break;
        case semantic::PrimitiveType::Float:
            if (!inRange(
                    -std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max())) return false;
            break;
        default:
            break;
        }
    }

    switch (target) {
    case semantic::PrimitiveType::Byte:
        result = ByteValue{static_cast<std::uint8_t>(number)}; return true;
    case semantic::PrimitiveType::SByte:
        result = SByteValue{static_cast<std::int8_t>(number)}; return true;
    case semantic::PrimitiveType::Short:
        result = ShortValue{static_cast<std::int16_t>(number)}; return true;
    case semantic::PrimitiveType::UShort:
        result = UShortValue{static_cast<std::uint16_t>(number)}; return true;
    case semantic::PrimitiveType::Int:
        result = static_cast<std::int64_t>(number); return true;
    case semantic::PrimitiveType::UInt:
        result = UIntValue{static_cast<std::uint32_t>(number)}; return true;
    case semantic::PrimitiveType::Long:
        result = LongValue{static_cast<std::int64_t>(number)}; return true;
    case semantic::PrimitiveType::ULong:
        result = ULongValue{static_cast<std::uint64_t>(number)}; return true;
    case semantic::PrimitiveType::Float:
        result = FloatValue{static_cast<float>(number)}; return true;
    case semantic::PrimitiveType::Double:
        result = static_cast<double>(number); return true;
    case semantic::PrimitiveType::Char:
        result = CharValue{static_cast<char16_t>(number)}; return true;
    default:
        return false;
    }
}

semantic::PrimitiveType valueType(const Value& value) noexcept {
    static constexpr semantic::PrimitiveType kTagPrimitive[33] = {
        semantic::PrimitiveType::Null,   // 0: Monostate
        semantic::PrimitiveType::String, // 1: NullString
        semantic::PrimitiveType::Object, // 2: NullObject
        semantic::PrimitiveType::Array,  // 3: NullArray
        semantic::PrimitiveType::Bool,   // 4: Bool
        semantic::PrimitiveType::Byte,   // 5: Byte
        semantic::PrimitiveType::SByte,  // 6: SByte
        semantic::PrimitiveType::Short,  // 7: Short
        semantic::PrimitiveType::UShort, // 8: UShort
        semantic::PrimitiveType::Int,    // 9: Int64
        semantic::PrimitiveType::UInt,   // 10: UInt
        semantic::PrimitiveType::Long,   // 11: Long
        semantic::PrimitiveType::ULong,  // 12: ULong
        semantic::PrimitiveType::Float,  // 13: Float
        semantic::PrimitiveType::Double, // 14: Double
        semantic::PrimitiveType::Char,   // 15: Char
        semantic::PrimitiveType::Enum,   // 16: EnumInline
        semantic::PrimitiveType::Struct, // 17: Fixed
        semantic::PrimitiveType::Struct, // 18: Fixed16
        semantic::PrimitiveType::Struct, // 19: Vec2
        semantic::PrimitiveType::Struct, // 20: Vec3
        semantic::PrimitiveType::Struct, // 21: QuatInline
        semantic::PrimitiveType::Handle, // 22: NativeHandle
        semantic::PrimitiveType::Enum,   // 23: EnumArena
        semantic::PrimitiveType::Struct, // 24: QuatArena
        semantic::PrimitiveType::Struct, // 25: Struct
        semantic::PrimitiveType::String, // 26: String
        semantic::PrimitiveType::Object, // 27: ObjectRef (refined below)
        semantic::PrimitiveType::Struct, // 28: Fixed64
        semantic::PrimitiveType::Struct, // 29: Vec2f
        semantic::PrimitiveType::Struct, // 30: Vec3f
        semantic::PrimitiveType::Struct, // 31: QuatfInline
        semantic::PrimitiveType::Struct, // 32: QuatfArena
    };
    const auto t = value.rawTag();
    if (t == ValueTag::ObjectRef) {
        const auto kind = value.asObjectRefUnchecked().kind;
        if (kind == ObjectKind::String) return semantic::PrimitiveType::String;
        if (kind == ObjectKind::Array) return semantic::PrimitiveType::Array;
        return semantic::PrimitiveType::Object;
    }
    const auto idx = static_cast<std::uint8_t>(t);
    return idx < 33u ? kTagPrimitive[idx] : semantic::PrimitiveType::Null;
}

std::string valueToString(const Value& value) {
    return valueToString(value, nullptr);
}

std::string valueToString(const Value& value, const ManagedHeap* heap) {
    if (std::holds_alternative<std::monostate>(value)) return "null";
    if (std::holds_alternative<NullString>(value)) return "null";
    if (std::holds_alternative<NullObject>(value)) return "null";
    if (std::holds_alternative<NullArray>(value)) return "null";
    if (std::holds_alternative<bool>(value)) return std::get<bool>(value) ? "true" : "false";
    if (const auto* number = std::get_if<ByteValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<SByteValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<ShortValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<UShortValue>(&value)) return std::to_string(number->value);
    if (std::holds_alternative<std::int64_t>(value)) return std::to_string(std::get<std::int64_t>(value));
    if (const auto* number = std::get_if<UIntValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<LongValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<ULongValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<FloatValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<double>(&value)) return std::to_string(*number);
    if (const auto* character = std::get_if<CharValue>(&value)) return std::to_string(character->value);
    if (EnumValue enumeration{}; value.tryGetEnum(enumeration)) {
        return "<enum:type=0x" + std::to_string(enumeration.typeId) + ":" +
            std::to_string(enumeration.value) + ">";
    }
    if (const auto* fixed = std::get_if<FixedValue>(&value)) {
        return "<Fixed:raw=" + std::to_string(fixed->raw) + ">";
    }
    if (const auto* fixed16 = std::get_if<Fixed16Value>(&value)) {
        return "<Fixed16:raw=" + std::to_string(fixed16->raw) + ">";
    }
    if (const auto* v2 = std::get_if<Vec2Value>(&value)) {
        return "<Vec2:" + std::to_string(v2->x) + "," + std::to_string(v2->y) + ">";
    }
    if (const auto* v3 = std::get_if<Vec3Value>(&value)) {
        return "<Vec3:" + std::to_string(v3->x) + "," + std::to_string(v3->y) + "," +
            std::to_string(v3->z) + ">";
    }
    if (QuatValue q{}; value.tryGetQuat(q)) {
        return "<Quat:" + std::to_string(q.x) + "," + std::to_string(q.y) + "," +
            std::to_string(q.z) + "," + std::to_string(q.w) + ">";
    }
    if (const auto* f64 = std::get_if<Fixed64Value>(&value)) {
        return "<Fixed64:raw=" + std::to_string(f64->raw) + ">";
    }
    if (const auto* v2f = std::get_if<Vec2fValue>(&value)) {
        return "<Vec2f:" + std::to_string(v2f->x) + "," + std::to_string(v2f->y) + ">";
    }
    if (const auto* v3f = std::get_if<Vec3fValue>(&value)) {
        return "<Vec3f:" + std::to_string(v3f->x) + "," + std::to_string(v3f->y) + "," +
            std::to_string(v3f->z) + ">";
    }
    if (QuatfValue qf{}; value.tryGetQuatf(qf)) {
        return "<Quatf:" + std::to_string(qf.x) + "," + std::to_string(qf.y) + "," +
            std::to_string(qf.z) + "," + std::to_string(qf.w) + ">";
    }
    if (const auto* structure = std::get_if<StructValue>(&value)) {
        return "<struct:type=0x" + std::to_string(structure->typeId) + ":fields=" +
            std::to_string(structure->fieldCount()) + ">";
    }
    if (std::holds_alternative<std::string>(value)) return std::get<std::string>(value);
    if (NativeHandle handle{}; value.tryGetNativeHandle(handle)) {
        return handle.valid()
            ? "<handle:" + std::to_string(handle.slot) + ":" +
                std::to_string(handle.generation) + ":registry=" +
                std::to_string(handle.registryId) + ":type=0x" +
                std::to_string(handle.typeId) + ">"
            : "null";
    }
    const auto reference = std::get<ObjectRef>(value);
    if (heap) {
        const auto text = heap->stringView(reference);
        if (text) return std::string(*text);
    }
    const auto kind = reference.kind == ObjectKind::Array ? "array" : "object";
    return "<" + std::string(kind) + ":" + std::to_string(reference.slot) + ":" +
        std::to_string(reference.generation) + ":heap=" +
        std::to_string(reference.heapId) + ">";
}

} // namespace realscript::runtime

            #undef RS_TRUSTED_SPAN_CHECK
