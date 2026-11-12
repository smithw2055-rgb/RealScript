#pragma once

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/runtime/Runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realscript::game {

class ResourceRegistry;

// Godot-style reference-counted host asset base class unified with RealScript `Resource`.
class HostResource {
public:
    HostResource() = default;
    explicit HostResource(std::string path, std::string guid = {})
        : path_(std::move(path)), guid_(std::move(guid)) {}
    virtual ~HostResource() = default;

    HostResource(const HostResource&) = delete;
    HostResource& operator=(const HostResource&) = delete;

    [[nodiscard]] std::int64_t resourceId() const noexcept { return resourceId_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& guid() const noexcept { return guid_; }
    [[nodiscard]] int useCount() const noexcept {
        if (disposed_.load(std::memory_order_acquire)) {
            return 0;
        }
        const int c = useCount_.load(std::memory_order_acquire);
        return c > 0 ? c : 0;
    }
    [[nodiscard]] bool isDisposed() const noexcept {
        return disposed_.load(std::memory_order_acquire);
    }

    void setPath(std::string nextPath) { path_ = std::move(nextPath); }
    void setGuid(std::string nextGuid) { guid_ = std::move(nextGuid); }

    int retain() noexcept {
        if (disposed_.load(std::memory_order_acquire)) {
            return 0;
        }
        int cur = useCount_.load(std::memory_order_relaxed);
        while (cur > 0) {
            if (useCount_.compare_exchange_weak(
                    cur,
                    cur + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                if (disposed_.load(std::memory_order_acquire)) {
                    useCount_.fetch_sub(1, std::memory_order_acq_rel);
                    return 0;
                }
                return cur + 1;
            }
        }
        return 0;
    }

    int release() {
        if (disposed_.load(std::memory_order_acquire)) {
            return 0;
        }
        int cur = useCount_.load(std::memory_order_relaxed);
        while (cur > 0) {
            if (useCount_.compare_exchange_weak(
                    cur,
                    cur - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                const int remaining = cur - 1;
                if (remaining == 0) {
                    if (!disposed_.exchange(true, std::memory_order_acq_rel)) {
                        onDispose();
                        std::function<void(std::int64_t)> hook;
                        {
                            std::lock_guard<std::mutex> lock(hookMutex_);
                            hook = std::move(zeroRefHook_);
                            zeroRefHook_ = nullptr;
                        }
                        if (hook) {
                            hook(resourceId_);
                        }
                    }
                }
                return remaining;
            }
        }
        return 0;
    }

    void dispose() {
        (void)release();
    }

protected:
    virtual void onDispose() {}

private:
    friend class ResourceRegistry;
    template <typename T>
    friend class ResourceHandle;

    std::int64_t resourceId_ = 0;
    std::string path_;
    std::string guid_;
    std::atomic<int> useCount_{1};
    std::atomic<bool> disposed_{false};
    std::atomic<bool> adopted_{false};
    std::mutex hookMutex_;
    std::function<void(std::int64_t)> zeroRefHook_;
};

// Godot `Ref<T>`-style deterministic reference-counted handle for host C++ code.
template <typename T = HostResource>
class ResourceHandle {
    static_assert(
        std::is_base_of_v<HostResource, T>,
        "ResourceHandle<T> requires T to derive from realscript::game::HostResource");

public:
    ResourceHandle() noexcept = default;
    ResourceHandle(std::nullptr_t) noexcept {}

    static ResourceHandle adopt(std::shared_ptr<T> resource) noexcept {
        if (!resource || resource->isDisposed()) {
            return {};
        }
        if (resource->adopted_.exchange(true, std::memory_order_acq_rel)) {
            if (resource->retain() <= 0) {
                return {};
            }
        }
        return ResourceHandle(std::move(resource), AdoptTag{});
    }

    static ResourceHandle retainFrom(std::shared_ptr<T> resource) noexcept {
        if (!resource || resource->isDisposed()) {
            return {};
        }
        if (!resource->adopted_.exchange(true, std::memory_order_acq_rel)) {
            return ResourceHandle(std::move(resource), AdoptTag{});
        }
        if (resource->retain() <= 0) {
            return {};
        }
        return ResourceHandle(std::move(resource), AdoptTag{});
    }

    ResourceHandle(const ResourceHandle& other) noexcept {
        if (other.ptr_ && other.active_ && !other.ptr_->isDisposed()) {
            if (other.ptr_->retain() > 0) {
                ptr_ = other.ptr_;
                active_ = true;
            }
        }
    }

    ResourceHandle(ResourceHandle&& other) noexcept
        : ptr_(std::move(other.ptr_)), active_(other.active_) {
        other.active_ = false;
    }

    template <
        typename U,
        typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    ResourceHandle(const ResourceHandle<U>& other) noexcept {
        if (other.ptr_ && other.active_ && !other.ptr_->isDisposed()) {
            if (other.ptr_->retain() > 0) {
                ptr_ = other.ptr_;
                active_ = true;
            }
        }
    }

    template <
        typename U,
        typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    ResourceHandle(ResourceHandle<U>&& other) noexcept
        : ptr_(std::move(other.ptr_)), active_(other.active_) {
        other.active_ = false;
    }

    ResourceHandle& operator=(const ResourceHandle& other) {
        if (this == &other) {
            return *this;
        }
        dispose();
        if (other.ptr_ && other.active_ && !other.ptr_->isDisposed()) {
            if (other.ptr_->retain() > 0) {
                ptr_ = other.ptr_;
                active_ = true;
            }
        }
        return *this;
    }

    ResourceHandle& operator=(ResourceHandle&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        dispose();
        ptr_ = std::move(other.ptr_);
        active_ = other.active_;
        other.active_ = false;
        return *this;
    }

    ~ResourceHandle() {
        dispose();
    }

    [[nodiscard]] ResourceHandle retain() const noexcept {
        return retainFrom(ptr_);
    }

    int release() {
        if (!active_ || !ptr_) {
            active_ = false;
            return ptr_ ? ptr_->useCount() : 0;
        }
        active_ = false;
        auto local = std::move(ptr_);
        return local->release();
    }

    void dispose() {
        (void)release();
    }

    [[nodiscard]] int useCount() const noexcept {
        return ptr_ ? ptr_->useCount() : 0;
    }

    [[nodiscard]] bool isDisposed() const noexcept {
        return !ptr_ || ptr_->isDisposed();
    }

    [[nodiscard]] bool isValid() const noexcept {
        return active_ && ptr_ != nullptr && !ptr_->isDisposed();
    }

    [[nodiscard]] std::int64_t resourceId() const noexcept {
        return ptr_ ? ptr_->resourceId() : 0;
    }

    [[nodiscard]] std::string path() const {
        return ptr_ ? ptr_->path() : std::string{};
    }

    [[nodiscard]] std::string guid() const {
        return ptr_ ? ptr_->guid() : std::string{};
    }

    [[nodiscard]] T* get() const noexcept {
        return isValid() ? ptr_.get() : nullptr;
    }

    [[nodiscard]] std::shared_ptr<T> shared() const noexcept {
        return isValid() ? ptr_ : nullptr;
    }

    T* operator->() const noexcept { return get(); }
    T& operator*() const noexcept { return *get(); }
    explicit operator bool() const noexcept { return isValid(); }

private:
    template <typename U>
    friend class ResourceHandle;
    friend class ResourceRegistry;

    struct AdoptTag {};

    ResourceHandle(std::shared_ptr<T> ptr, AdoptTag) noexcept
        : ptr_(std::move(ptr)), active_(true) {}

    std::shared_ptr<T> ptr_;
    bool active_ = false;
};

// Unified Host/Script Resource Registry supporting path/GUID deduplication,
// deterministic zero-refcount cleanup, and seamless RealScript `Resource` / `ResourceHandle` bindings.
class ResourceRegistry {
public:
    ResourceRegistry() : state_(std::make_shared<RegistryState>()) {}

    void clear() {
        if (!state_) {
            state_ = std::make_shared<RegistryState>();
            return;
        }
        // Clear registered lookup slots without ever resetting `nextId_` back to 1,
        // preventing resource ID collisions between pre-clear handles and post-clear resources.
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->clearEpoch;
        state_->entries.clear();
        state_->byPath.clear();
        state_->byGuid.clear();
        state_->disposedCount.store(0, std::memory_order_relaxed);
    }

    template <typename T, typename... Args>
    ResourceHandle<T> emplace(std::string path, std::string guid, Args&&... args) {
        static_assert(std::is_base_of_v<HostResource, T>);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (auto existing = findLiveHandleLocked<T>(*state_, path, guid)) {
                return existing;
            }
            if (hasConflictingLiveIdentityLocked<T>(*state_, path, guid, nullptr)) {
                return {};
            }
        }
        auto created = std::make_shared<T>(std::forward<Args>(args)...);
        if (!created || created->isDisposed()) {
            return {};
        }
        created->setPath(std::move(path));
        created->setGuid(std::move(guid));
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (auto existing = findLiveHandleLocked<T>(*state_, created->path(), created->guid())) {
                return existing;
            }
            if (hasConflictingLiveIdentityLocked<T>(*state_, created->path(), created->guid(), created.get())) {
                return {};
            }
            attachEntryLocked(state_, created);
        }
        return ResourceHandle<T>::adopt(std::move(created));
    }

    template <typename T>
    ResourceHandle<T> registerResource(std::shared_ptr<T> resource) {
        static_assert(std::is_base_of_v<HostResource, T>);
        if (!resource || resource->isDisposed()) {
            return {};
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (resource->resourceId() > 0) {
                const auto selfIt = state_->entries.find(resource->resourceId());
                if (selfIt != state_->entries.end() && selfIt->second.get() == resource.get() &&
                    !resource->isDisposed()) {
                    return ResourceHandle<T>::adopt(std::move(resource));
                }
            }
            if (auto existing = findLiveHandleLocked<T>(*state_, resource->path(), resource->guid())) {
                return existing;
            }
            if (hasConflictingLiveIdentityLocked<T>(*state_, resource->path(), resource->guid(), resource.get())) {
                return {};
            }
            attachEntryLocked(state_, resource);
        }
        return ResourceHandle<T>::adopt(std::move(resource));
    }

    ResourceHandle<HostResource> loadOrCreate(
        const std::string& path,
        const std::string& guid = {},
        const std::function<std::shared_ptr<HostResource>(const std::string&, const std::string&)>& factory = {}) {
        return loadOrCreateInState(state_, path, guid, factory);
    }

    std::int64_t acquireForScript(const std::string& path, const std::string& guid) {
        return acquireForScriptInState(state_, path, guid);
    }

    int retainById(std::int64_t resourceId) noexcept {
        return retainByIdInState(*state_, resourceId);
    }

    int releaseById(std::int64_t resourceId) {
        return releaseByIdInState(*state_, resourceId);
    }

    [[nodiscard]] int useCountById(std::int64_t resourceId) const noexcept {
        return useCountByIdInState(*state_, resourceId);
    }

    [[nodiscard]] bool isDisposedById(std::int64_t resourceId) const noexcept {
        return isDisposedByIdInState(*state_, resourceId);
    }

    [[nodiscard]] std::string pathById(std::int64_t resourceId) const {
        return pathByIdInState(*state_, resourceId);
    }

    [[nodiscard]] std::string guidById(std::int64_t resourceId) const {
        return guidByIdInState(*state_, resourceId);
    }

    [[nodiscard]] std::int64_t findIdByPath(std::string_view path) const {
        return findIdByPathInState(*state_, path);
    }

    [[nodiscard]] std::int64_t findIdByGuid(std::string_view guid) const {
        return findIdByGuidInState(*state_, guid);
    }

    [[nodiscard]] std::shared_ptr<HostResource> findResource(std::int64_t resourceId) const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto it = state_->entries.find(resourceId);
        if (it == state_->entries.end() || !it->second || it->second->isDisposed()) {
            return nullptr;
        }
        return it->second;
    }

    [[nodiscard]] std::size_t liveCount() const noexcept {
        return liveCountInState(*state_);
    }

    [[nodiscard]] std::size_t entryCount() const noexcept {
        if (!state_) return 0;
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->entries.size();
    }

    [[nodiscard]] std::size_t disposedCount() const noexcept {
        return state_->disposedCount.load(std::memory_order_acquire);
    }

    void setDefaultFactory(
        std::function<std::shared_ptr<HostResource>(const std::string&, const std::string&)> factory) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->defaultFactory = std::move(factory);
    }

    void setZeroRefCallback(std::function<void(std::int64_t, const HostResource&)> callback) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->onZeroRef = std::move(callback);
    }

    // Bind `RealScript.Resource` host bridge intrinsics to `BindingRegistry` (works for both Interpreter & AOT).
    void bindTo(runtime::BindingRegistry& bindings, runtime::ManagedHeap* heap = nullptr) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->boundHeap = heap;
        }
        const std::weak_ptr<RegistryState> weakState = state_;

        bindings.bind(
            "RealScript.Resource::__RsHostResAcquire",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                const auto state = weakState.lock();
                if (!state) {
                    return runtime::Value{runtime::LongValue{0}};
                }
                if (args.size() != 2) {
                    error.code = runtime::ErrorCode::InvalidArguments;
                    error.message = "__RsHostResAcquire expects 2 arguments";
                    return std::nullopt;
                }
                const auto path = extractString(*state, args[0]);
                const auto guid = extractString(*state, args[1]);
                if (!path || !guid) {
                    error.code = runtime::ErrorCode::TypeMismatch;
                    error.message = "__RsHostResAcquire expects string arguments";
                    return std::nullopt;
                }
                return runtime::Value{runtime::LongValue{acquireForScriptInState(state, *path, *guid)}};
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResRetain",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                const auto id = extractLong(args, error);
                if (!id) return std::nullopt;
                const auto state = weakState.lock();
                if (!state) return runtime::Value{std::int64_t{0}};
                return runtime::Value{static_cast<std::int64_t>(retainByIdInState(*state, *id))};
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResRelease",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                const auto id = extractLong(args, error);
                if (!id) return std::nullopt;
                const auto state = weakState.lock();
                if (!state) return runtime::Value{std::int64_t{0}};
                return runtime::Value{static_cast<std::int64_t>(releaseByIdInState(*state, *id))};
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResUseCount",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                const auto id = extractLong(args, error);
                if (!id) return std::nullopt;
                const auto state = weakState.lock();
                if (!state) return runtime::Value{std::int64_t{0}};
                return runtime::Value{static_cast<std::int64_t>(useCountByIdInState(*state, *id))};
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResIsDisposed",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                const auto id = extractLong(args, error);
                if (!id) return std::nullopt;
                const auto state = weakState.lock();
                if (!state) return runtime::Value{true};
                return runtime::Value{isDisposedByIdInState(*state, *id)};
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResGetPath",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                const auto id = extractLong(args, error);
                if (!id) return std::nullopt;
                const auto state = weakState.lock();
                if (!state) return runtime::Value{std::string{}};
                return makeStringValue(*state, pathByIdInState(*state, *id));
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResGetGuid",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                const auto id = extractLong(args, error);
                if (!id) return std::nullopt;
                const auto state = weakState.lock();
                if (!state) return runtime::Value{std::string{}};
                return makeStringValue(*state, guidByIdInState(*state, *id));
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResFindByPath",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                if (args.size() != 1) {
                    error.code = runtime::ErrorCode::InvalidArguments;
                    error.message = "__RsHostResFindByPath expects 1 argument";
                    return std::nullopt;
                }
                const auto state = weakState.lock();
                if (!state) return runtime::Value{runtime::LongValue{0}};
                const auto path = extractString(*state, args[0]);
                if (!path) {
                    error.code = runtime::ErrorCode::TypeMismatch;
                    error.message = "expected string path";
                    return std::nullopt;
                }
                return runtime::Value{runtime::LongValue{findIdByPathInState(*state, *path)}};
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResFindByGuid",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                if (args.size() != 1) {
                    error.code = runtime::ErrorCode::InvalidArguments;
                    error.message = "__RsHostResFindByGuid expects 1 argument";
                    return std::nullopt;
                }
                const auto state = weakState.lock();
                if (!state) return runtime::Value{runtime::LongValue{0}};
                const auto guid = extractString(*state, args[0]);
                if (!guid) {
                    error.code = runtime::ErrorCode::TypeMismatch;
                    error.message = "expected string guid";
                    return std::nullopt;
                }
                return runtime::Value{runtime::LongValue{findIdByGuidInState(*state, *guid)}};
            });

        bindings.bind(
            "RealScript.Resource::__RsHostResLiveCount",
            [weakState](const bytecode::FunctionReference&,
                        runtime::ValueSpan args,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
                if (!args.empty()) {
                    error.code = runtime::ErrorCode::InvalidArguments;
                    error.message = "__RsHostResLiveCount expects 0 arguments";
                    return std::nullopt;
                }
                const auto state = weakState.lock();
                if (!state) return runtime::Value{std::int64_t{0}};
                return runtime::Value{static_cast<std::int64_t>(liveCountInState(*state))};
            });
    }

    static bool enableInInterpreter(runtime::Interpreter& interpreter, bool enabled = true) {
        const std::vector<runtime::Value> args{runtime::Value{enabled}};
        const auto res = interpreter.invoke(
            "RealScript.Resource::ResourceServer.EnableHostBridge", args);
        return res.succeeded;
    }

    static bool enableInAot(aot::Program& program, bool enabled = true) {
        const std::vector<runtime::Value> args{runtime::Value{enabled}};
        const auto res = program.invoke(
            "RealScript.Resource::ResourceServer.EnableHostBridge", args);
        return res.succeeded;
    }

private:
    struct RegistryState {
        mutable std::mutex mutex;
        std::uint64_t clearEpoch = 0;
        std::unordered_map<std::int64_t, std::shared_ptr<HostResource>> entries;
        std::unordered_map<std::string, std::int64_t> byPath;
        std::unordered_map<std::string, std::int64_t> byGuid;
        std::atomic<std::size_t> disposedCount{0};
        runtime::ManagedHeap* boundHeap = nullptr;
        std::function<std::shared_ptr<HostResource>(const std::string&, const std::string&)> defaultFactory;
        std::function<void(std::int64_t, const HostResource&)> onZeroRef;
    };

    static std::int64_t allocateNextId() noexcept {
        return nextId_.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename T>
    static bool hasConflictingLiveIdentityLocked(
        const RegistryState& state,
        const std::string& path,
        const std::string& guid,
        const HostResource* selfPtr) {
        if (!guid.empty()) {
            const auto git = state.byGuid.find(guid);
            if (git != state.byGuid.end()) {
                const auto eit = state.entries.find(git->second);
                if (eit != state.entries.end() && eit->second && !eit->second->isDisposed() &&
                    eit->second.get() != selfPtr &&
                    std::dynamic_pointer_cast<T>(eit->second) == nullptr) {
                    return true;
                }
            }
        }
        if (!path.empty()) {
            const auto pit = state.byPath.find(path);
            if (pit != state.byPath.end()) {
                const auto eit = state.entries.find(pit->second);
                if (eit != state.entries.end() && eit->second && !eit->second->isDisposed() &&
                    eit->second.get() != selfPtr &&
                    std::dynamic_pointer_cast<T>(eit->second) == nullptr) {
                    return true;
                }
            }
        }
        return false;
    }

    template <typename T>
    static ResourceHandle<T> findLiveHandleLocked(
        RegistryState& state,
        const std::string& path,
        const std::string& guid) {
        if (!guid.empty()) {
            const auto git = state.byGuid.find(guid);
            if (git != state.byGuid.end()) {
                const auto eit = state.entries.find(git->second);
                if (eit != state.entries.end() && eit->second && !eit->second->isDisposed()) {
                    if (auto casted = std::dynamic_pointer_cast<T>(eit->second)) {
                        return ResourceHandle<T>::retainFrom(std::move(casted));
                    }
                }
            }
        }
        if (!path.empty()) {
            const auto pit = state.byPath.find(path);
            if (pit != state.byPath.end()) {
                const auto eit = state.entries.find(pit->second);
                if (eit != state.entries.end() && eit->second && !eit->second->isDisposed()) {
                    if (auto casted = std::dynamic_pointer_cast<T>(eit->second)) {
                        return ResourceHandle<T>::retainFrom(std::move(casted));
                    }
                }
            }
        }
        return {};
    }

    static void attachEntryLocked(
        const std::shared_ptr<RegistryState>& state,
        const std::shared_ptr<HostResource>& resource) {
        const std::int64_t id = allocateNextId();
        const std::uint64_t entryEpoch = state->clearEpoch;
        resource->resourceId_ = id;
        if (!resource->path().empty()) {
            state->byPath[resource->path()] = id;
        }
        if (!resource->guid().empty()) {
            state->byGuid[resource->guid()] = id;
        }
        state->entries[id] = resource;
        const std::weak_ptr<RegistryState> weakState = state;
        std::lock_guard<std::mutex> hookLock(resource->hookMutex_);
        resource->zeroRefHook_ = [weakState, entryEpoch](std::int64_t zeroId) {
            const auto locked = weakState.lock();
            if (!locked) {
                return;
            }
            std::shared_ptr<HostResource> keepAlive;
            std::function<void(std::int64_t, const HostResource&)> callback;
            {
                std::lock_guard<std::mutex> lock(locked->mutex);
                if (locked->clearEpoch != entryEpoch) {
                    return;
                }
                locked->disposedCount.fetch_add(1, std::memory_order_acq_rel);
                const auto it = locked->entries.find(zeroId);
                if (it != locked->entries.end()) {
                    keepAlive = std::move(it->second);
                    locked->entries.erase(it);
                    if (keepAlive) {
                        if (!keepAlive->path().empty()) {
                            const auto pit = locked->byPath.find(keepAlive->path());
                            if (pit != locked->byPath.end() && pit->second == zeroId) {
                                locked->byPath.erase(pit);
                            }
                        }
                        if (!keepAlive->guid().empty()) {
                            const auto git = locked->byGuid.find(keepAlive->guid());
                            if (git != locked->byGuid.end() && git->second == zeroId) {
                                locked->byGuid.erase(git);
                            }
                        }
                        callback = locked->onZeroRef;
                    }
                }
            }
            if (callback && keepAlive) {
                callback(zeroId, *keepAlive);
            }
        };
    }

    static ResourceHandle<HostResource> loadOrCreateInState(
        const std::shared_ptr<RegistryState>& state,
        const std::string& path,
        const std::string& guid,
        const std::function<std::shared_ptr<HostResource>(const std::string&, const std::string&)>& factory) {
        if (!state) {
            return {};
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (auto existing = findLiveHandleLocked<HostResource>(*state, path, guid)) {
                return existing;
            }
        }
        std::shared_ptr<HostResource> created = factory
            ? factory(path, guid)
            : std::make_shared<HostResource>(path, guid);
        if (!created || created->isDisposed()) {
            return {};
        }
        if (created->path().empty() && !path.empty()) {
            created->setPath(path);
        }
        if (created->guid().empty() && !guid.empty()) {
            created->setGuid(guid);
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (auto existing = findLiveHandleLocked<HostResource>(*state, created->path(), created->guid())) {
                return existing;
            }
            attachEntryLocked(state, created);
        }
        return ResourceHandle<HostResource>::adopt(std::move(created));
    }

    static std::int64_t acquireForScriptInState(
        const std::shared_ptr<RegistryState>& state,
        const std::string& path,
        const std::string& guid) {
        if (!state) {
            return 0;
        }
        std::function<std::shared_ptr<HostResource>(const std::string&, const std::string&)> factory;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            factory = state->defaultFactory;
        }
        auto handle = loadOrCreateInState(state, path, guid, factory);
        if (!handle.isValid()) {
            return 0;
        }
        const auto id = handle.resourceId();
        // Transfer the retained reference count (+1) to the script caller without releasing on C++ scope exit.
        handle.active_ = false;
        return id;
    }

    static int retainByIdInState(RegistryState& state, std::int64_t resourceId) noexcept {
        std::shared_ptr<HostResource> keepAlive;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            const auto found = state.entries.find(resourceId);
            if (found == state.entries.end() || !found->second || found->second->isDisposed()) {
                return 0;
            }
            keepAlive = found->second;
        }
        return keepAlive->retain();
    }

    static int releaseByIdInState(RegistryState& state, std::int64_t resourceId) {
        std::shared_ptr<HostResource> keepAlive;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            const auto found = state.entries.find(resourceId);
            if (found == state.entries.end() || !found->second || found->second->isDisposed()) {
                return 0;
            }
            keepAlive = found->second;
        }
        return keepAlive->release();
    }

    [[nodiscard]] static int useCountByIdInState(
        const RegistryState& state,
        std::int64_t resourceId) noexcept {
        std::shared_ptr<HostResource> keepAlive;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            const auto found = state.entries.find(resourceId);
            if (found == state.entries.end() || !found->second) {
                return 0;
            }
            keepAlive = found->second;
        }
        return keepAlive->useCount();
    }

    [[nodiscard]] static bool isDisposedByIdInState(
        const RegistryState& state,
        std::int64_t resourceId) noexcept {
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.entries.find(resourceId);
        if (found == state.entries.end() || !found->second) {
            return true;
        }
        return found->second->isDisposed();
    }

    [[nodiscard]] static std::string pathByIdInState(
        const RegistryState& state,
        std::int64_t resourceId) {
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.entries.find(resourceId);
        return (found != state.entries.end() && found->second)
            ? found->second->path()
            : std::string{};
    }

    [[nodiscard]] static std::string guidByIdInState(
        const RegistryState& state,
        std::int64_t resourceId) {
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto found = state.entries.find(resourceId);
        return (found != state.entries.end() && found->second)
            ? found->second->guid()
            : std::string{};
    }

    [[nodiscard]] static std::int64_t findIdByPathInState(
        const RegistryState& state,
        std::string_view path) {
        if (path.empty()) return 0;
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto it = state.byPath.find(std::string(path));
        if (it == state.byPath.end()) return 0;
        const auto found = state.entries.find(it->second);
        return (found == state.entries.end() || !found->second || found->second->isDisposed()) ? 0 : it->second;
    }

    [[nodiscard]] static std::int64_t findIdByGuidInState(
        const RegistryState& state,
        std::string_view guid) {
        if (guid.empty()) return 0;
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto it = state.byGuid.find(std::string(guid));
        if (it == state.byGuid.end()) return 0;
        const auto found = state.entries.find(it->second);
        return (found == state.entries.end() || !found->second || found->second->isDisposed()) ? 0 : it->second;
    }

    [[nodiscard]] static std::size_t liveCountInState(const RegistryState& state) noexcept {
        std::lock_guard<std::mutex> lock(state.mutex);
        std::size_t count = 0;
        for (const auto& [id, res] : state.entries) {
            (void)id;
            if (res && !res->isDisposed()) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] static std::optional<std::string> extractString(
        const RegistryState& state,
        const runtime::Value& val) {
        if (const auto* s = std::get_if<std::string>(&val)) {
            return *s;
        }
        if (std::holds_alternative<runtime::NullString>(val) ||
            std::holds_alternative<std::monostate>(val)) {
            return std::string{};
        }
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&val)) {
            runtime::ManagedHeap* heap = nullptr;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                heap = state.boundHeap;
            }
            if (heap) {
                if (const auto sv = heap->stringView(*ref)) {
                    return std::string(*sv);
                }
            }
            if (const auto globalStr = runtime::ManagedHeap::tryCopyStringGlobal(*ref)) {
                return *globalStr;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static runtime::Value makeStringValue(
        const RegistryState& state,
        const std::string& text) {
        runtime::ManagedHeap* heap = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            heap = state.boundHeap;
        }
        if (heap) {
            if (const auto ref = heap->allocateString(text)) {
                return runtime::Value{*ref};
            }
        }
        return runtime::Value{text};
    }

    [[nodiscard]] static std::optional<std::int64_t> extractLong(
        runtime::ValueSpan args,
        runtime::RuntimeError& error) {
        if (args.size() != 1) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "expected 1 resourceId argument";
            return std::nullopt;
        }
        if (const auto* lv = std::get_if<runtime::LongValue>(&args[0])) {
            return lv->value;
        }
        if (const auto* iv = std::get_if<std::int64_t>(&args[0])) {
            return *iv;
        }
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "expected long resourceId";
        return std::nullopt;
    }

    static inline std::atomic<std::int64_t> nextId_{1};
    std::shared_ptr<RegistryState> state_;
};

} // namespace realscript::game
