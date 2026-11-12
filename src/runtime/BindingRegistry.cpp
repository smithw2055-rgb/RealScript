#include "RuntimeInternal.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <utility>

namespace realscript::runtime {
namespace {

std::mutex& bindingCacheMutexFor(const BindingRegistry* reg) noexcept {
    static auto* stripes = new std::array<std::mutex, 64>();
    const auto idx = (reinterpret_cast<std::uintptr_t>(reg) >> 4u) & 63u;
    return (*stripes)[idx];
}

} // namespace

bool BindingRegistry::bind(
    semantic::SymbolId symbolId,
    ExternalFunction function,
    BindingDeterminism determinism,
    std::string requiredCapability) {
    if (symbolId == 0 || !function) return false;
    const bool inserted = bySymbol_.emplace(
        symbolId,
        Entry{std::move(function), determinism, std::move(requiredCapability)}).second;
    if (inserted) ++entryCount_;
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
    if (inserted) ++entryCount_;
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
