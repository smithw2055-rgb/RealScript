#include "RuntimeInternal.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

namespace realscript::runtime {
namespace {

std::uint64_t nextBindingRegistryId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1u, std::memory_order_relaxed);
}

std::mutex& bindingCacheMutexFor(const BindingRegistry* reg) noexcept {
    static auto* stripes = new std::array<std::mutex, 64>();
    const auto idx = (reinterpret_cast<std::uintptr_t>(reg) >> 4u) & 63u;
    return (*stripes)[idx];
}

} // namespace

BindingRegistry::BindingRegistry() noexcept
    : instanceId_(nextBindingRegistryId()) {}

BindingRegistry::BindingRegistry(const BindingRegistry& other)
    : bySymbol_(other.bySymbol_),
      byName_(other.byName_),
      entryCount_(other.entryCount_),
      instanceId_(nextBindingRegistryId()),
      mutationEpoch_(1) {}

BindingRegistry& BindingRegistry::operator=(const BindingRegistry& other) {
    if (this == &other) return *this;
    {
        std::lock_guard<std::mutex> cacheLock(bindingCacheMutexFor(this));
        resolvedBySymbolCache_.clear();
    }
    bySymbol_ = other.bySymbol_;
    byName_ = other.byName_;
    entryCount_ = other.entryCount_;
    ++mutationEpoch_;
    return *this;
}

BindingRegistry::BindingRegistry(BindingRegistry&& other) noexcept
    : bySymbol_(std::move(other.bySymbol_)),
      byName_(std::move(other.byName_)),
      entryCount_(other.entryCount_),
      instanceId_(nextBindingRegistryId()),
      mutationEpoch_(1) {
    std::lock_guard<std::mutex> cacheLock(bindingCacheMutexFor(&other));
    other.resolvedBySymbolCache_.clear();
    other.entryCount_ = 0;
    ++other.mutationEpoch_;
}

BindingRegistry& BindingRegistry::operator=(BindingRegistry&& other) noexcept {
    if (this == &other) return *this;
    {
        std::lock_guard<std::mutex> cacheLock(bindingCacheMutexFor(this));
        resolvedBySymbolCache_.clear();
    }
    {
        std::lock_guard<std::mutex> otherCacheLock(bindingCacheMutexFor(&other));
        other.resolvedBySymbolCache_.clear();
    }
    bySymbol_ = std::move(other.bySymbol_);
    byName_ = std::move(other.byName_);
    entryCount_ = other.entryCount_;
    other.entryCount_ = 0;
    ++mutationEpoch_;
    ++other.mutationEpoch_;
    return *this;
}

bool BindingRegistry::bind(
    semantic::SymbolId symbolId,
    ExternalFunction function,
    BindingDeterminism determinism,
    std::string requiredCapability) {
    if (symbolId == 0 || !function) return false;
    const bool inserted = bySymbol_.emplace(
        symbolId,
        Entry{std::move(function), determinism, std::move(requiredCapability)}).second;
    if (inserted) {
        ++entryCount_;
        ++mutationEpoch_;
    }
    return inserted;
}

bool BindingRegistry::bind(
    const std::string& canonicalName,
    ExternalFunction function,
    BindingDeterminism determinism,
    std::string requiredCapability) {
    if (canonicalName.empty() || !function) return false;
    if (byName_.find(canonicalName) != byName_.end()) return false;
    Entry entry{std::move(function), determinism, std::move(requiredCapability)};
    const auto symId = semantic::stableTypeId(canonicalName);
    bySymbol_.try_emplace(symId, entry);
    const bool inserted = byName_.emplace(canonicalName, std::move(entry)).second;
    if (inserted) {
        ++entryCount_;
        ++mutationEpoch_;
    }
    return inserted;
}

std::optional<ResolvedBinding> BindingRegistry::resolve(
    const bytecode::FunctionReference& reference) const {
    if (reference.symbolId != 0) {
        const auto bySymbol = bySymbol_.find(reference.symbolId);
        if (bySymbol != bySymbol_.end()) {
            return ResolvedBinding{
                &bySymbol->second.function,
                bySymbol->second.determinism,
                bySymbol->second.requiredCapability};
        }
        {
            std::lock_guard<std::mutex> cacheLock(bindingCacheMutexFor(this));
            const auto cached = resolvedBySymbolCache_.find(reference.symbolId);
            if (cached != resolvedBySymbolCache_.end() && cached->second) {
                return ResolvedBinding{
                    &cached->second->function,
                    cached->second->determinism,
                    cached->second->requiredCapability};
            }
        }
    }
    const auto byName = byName_.find(reference.name);
    if (byName != byName_.end()) {
        if (reference.symbolId != 0) {
            std::lock_guard<std::mutex> cacheLock(bindingCacheMutexFor(this));
            resolvedBySymbolCache_.emplace(reference.symbolId, &byName->second);
        }
        return ResolvedBinding{
            &byName->second.function,
            byName->second.determinism,
            byName->second.requiredCapability};
    }
    return std::nullopt;
}

std::optional<Value> BindingRegistry::invoke(
    const bytecode::FunctionReference& reference,
    ValueSpan arguments,
    RuntimeError& error) const {
    const auto resolved = resolve(reference);
    if (resolved && resolved->function) {
        return (*resolved->function)(reference, arguments, error);
    }
    error.code = ErrorCode::ExternalFunctionUnresolved;
    error.message = "external function '" + reference.name + "' is not registered";
    return std::nullopt;
}

std::size_t BindingRegistry::size() const noexcept {
    return entryCount_;
}

std::size_t BindingRegistry::cachedSymbolCount() const noexcept {
    std::lock_guard<std::mutex> cacheLock(bindingCacheMutexFor(this));
    return bySymbol_.size() + resolvedBySymbolCache_.size();
}

} // namespace realscript::runtime
