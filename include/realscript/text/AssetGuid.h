#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace realscript::text {

// 128-bit stable content-addressed identifier (RFC-4122 UUID v5 over canonical names/paths).
struct AssetGuid {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    constexpr AssetGuid() noexcept = default;
    constexpr AssetGuid(std::uint64_t hi, std::uint64_t lo) noexcept
        : high(hi), low(lo) {}

    [[nodiscard]] static constexpr AssetGuid nil() noexcept { return {}; }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return high != 0 || low != 0;
    }

    [[nodiscard]] static AssetGuid fromCanonicalName(
        std::string_view domain,
        std::string_view canonicalName);
    [[nodiscard]] static AssetGuid fromTypeName(std::string_view canonicalTypeName);
    [[nodiscard]] static AssetGuid fromAssetPath(std::string_view assetPath);

    [[nodiscard]] static std::optional<AssetGuid> parse(std::string_view uuidText) noexcept;
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] static std::string normalizeAssetPath(std::string_view rawPath);

    friend constexpr bool operator==(AssetGuid a, AssetGuid b) noexcept {
        return a.high == b.high && a.low == b.low;
    }
    friend constexpr bool operator!=(AssetGuid a, AssetGuid b) noexcept {
        return !(a == b);
    }
    friend constexpr bool operator<(AssetGuid a, AssetGuid b) noexcept {
        return a.high != b.high ? (a.high < b.high) : (a.low < b.low);
    }
    friend constexpr bool operator<=(AssetGuid a, AssetGuid b) noexcept {
        return !(b < a);
    }
    friend constexpr bool operator>(AssetGuid a, AssetGuid b) noexcept {
        return b < a;
    }
    friend constexpr bool operator>=(AssetGuid a, AssetGuid b) noexcept {
        return !(a < b);
    }
};

static_assert(sizeof(AssetGuid) == 16, "AssetGuid must be 128 bits (16 bytes)");

} // namespace realscript::text

namespace std {
template <>
struct hash<realscript::text::AssetGuid> {
    std::size_t operator()(realscript::text::AssetGuid g) const noexcept {
        const std::uint64_t x =
            g.high ^ (g.low + 0x9e3779b97f4a7c15ULL + (g.high << 6u) + (g.high >> 2u));
        return static_cast<std::size_t>(x);
    }
};
} // namespace std
