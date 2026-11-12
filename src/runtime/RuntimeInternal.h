#pragma once

#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace realscript::runtime::detail {

[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Shared Value / Struct / String operations (implemented in ValueOperations.cpp)
[[nodiscard]] std::uint64_t computeStructFieldsHash(
    const std::vector<Value>& fields) noexcept;
[[nodiscard]] bool isStringLikeValue(const Value& val) noexcept;
[[nodiscard]] std::string extractIntrinsicString(
    const Value& val,
    const ManagedHeap* heap);
[[nodiscard]] bool intrinsicValuesEqual(
    const Value& a,
    const Value& b,
    const ManagedHeap* heap);
[[nodiscard]] bool intrinsicValueLess(
    const Value& a,
    const Value& b,
    const ManagedHeap* heap);

// Shared sharded value-pool service accessors (implemented in RuntimeServices.cpp)
[[nodiscard]] const std::string& emptyRuntimePoolString() noexcept;
[[nodiscard]] StringValue internRuntimeString(
    std::string_view text,
    bool pinEntry = false);
[[nodiscard]] StringValue lookupRuntimeStringById(std::uint32_t id) noexcept;

[[nodiscard]] std::uint32_t allocateInlineArenaEntry(
    const InlineStructArenaEntry& entry) noexcept;
[[nodiscard]] const InlineStructArenaEntry& lookupInlineArenaEntry(
    std::uint32_t idx) noexcept;

[[nodiscard]] std::uint32_t registerHeapStructStorage(
    std::shared_ptr<const StructStorage> store,
    bool pinEntry = false);
[[nodiscard]] const StructStorage* lookupHeapStructStorage(
    std::uint32_t id) noexcept;

} // namespace realscript::runtime::detail
