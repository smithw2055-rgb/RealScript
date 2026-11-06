#pragma once

#include "realscript/runtime/RTSFiberScheduler.h"

#include <cstdint>
#include <string>

namespace realscript::game {

using Fixed = runtime::Fixed24_8;
using Fixed16 = runtime::Fixed16_16;
using Fixed24_8 = runtime::Fixed24_8;
using Fixed16_16 = runtime::Fixed16_16;

[[nodiscard]] constexpr std::int64_t deterministicSqrt64(std::int64_t n) noexcept {
    if (n <= 0) return 0;
    std::int64_t lo = 1;
    std::int64_t hi = n < 3037000499LL ? n : 3037000499LL;
    std::int64_t ans = 1;
    while (lo <= hi) {
        const std::int64_t mid = lo + (hi - lo) / 2;
        if (mid <= n / mid) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

struct Vec2 {
    Fixed x{};
    Fixed y{};

    constexpr Vec2() noexcept = default;
    constexpr Vec2(Fixed xVal, Fixed yVal) noexcept : x(xVal), y(yVal) {}
    constexpr Vec2(std::int32_t xInt, std::int32_t yInt) noexcept
        : x(Fixed::fromInt(xInt)), y(Fixed::fromInt(yInt)) {}

    [[nodiscard]] static constexpr Vec2 of(std::int32_t xInt, std::int32_t yInt) noexcept {
        return Vec2{Fixed::fromInt(xInt), Fixed::fromInt(yInt)};
    }

    [[nodiscard]] static constexpr Vec2 fromInts(std::int32_t xInt, std::int32_t yInt) noexcept {
        return Vec2{Fixed::fromInt(xInt), Fixed::fromInt(yInt)};
    }

    [[nodiscard]] static constexpr Vec2 raw(std::int32_t xRaw, std::int32_t yRaw) noexcept {
        return Vec2{Fixed::fromRaw(xRaw), Fixed::fromRaw(yRaw)};
    }

    [[nodiscard]] static constexpr Vec2 fromRaw(std::int32_t xRaw, std::int32_t yRaw) noexcept {
        return Vec2{Fixed::fromRaw(xRaw), Fixed::fromRaw(yRaw)};
    }

    [[nodiscard]] static constexpr Fixed dot(Vec2 a, Vec2 b) noexcept {
        return a.x * b.x + a.y * b.y;
    }

    [[nodiscard]] static constexpr Fixed cross(Vec2 a, Vec2 b) noexcept {
        return a.x * b.y - a.y * b.x;
    }

    [[nodiscard]] constexpr Fixed lengthSq() const noexcept {
        return x * x + y * y;
    }

    [[nodiscard]] constexpr Fixed length() const noexcept {
        const std::int64_t r2 =
            static_cast<std::int64_t>(x.raw) * static_cast<std::int64_t>(x.raw) +
            static_cast<std::int64_t>(y.raw) * static_cast<std::int64_t>(y.raw);
        return Fixed::fromRaw(static_cast<std::int32_t>(deterministicSqrt64(r2)));
    }

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept {
        return Vec2{a.x + b.x, a.y + b.y};
    }

    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept {
        return Vec2{a.x - b.x, a.y - b.y};
    }

    friend constexpr Vec2 operator-(Vec2 a) noexcept {
        return Vec2{-a.x, -a.y};
    }

    friend constexpr Vec2 operator*(Vec2 v, Fixed s) noexcept {
        return Vec2{v.x * s, v.y * s};
    }

    friend constexpr Vec2 operator*(Fixed s, Vec2 v) noexcept {
        return Vec2{v.x * s, v.y * s};
    }

    friend constexpr Vec2 operator/(Vec2 v, Fixed s) noexcept {
        return Vec2{v.x / s, v.y / s};
    }

    friend constexpr bool operator==(Vec2 a, Vec2 b) noexcept {
        return a.x == b.x && a.y == b.y;
    }

    friend constexpr bool operator!=(Vec2 a, Vec2 b) noexcept {
        return !(a == b);
    }
};

using FixedVec2 = Vec2;

struct Vec3 {
    Fixed x{};
    Fixed y{};
    Fixed z{};

    constexpr Vec3() noexcept = default;
    constexpr Vec3(Fixed xVal, Fixed yVal, Fixed zVal) noexcept
        : x(xVal), y(yVal), z(zVal) {}
    constexpr Vec3(std::int32_t xInt, std::int32_t yInt, std::int32_t zInt) noexcept
        : x(Fixed::fromInt(xInt)), y(Fixed::fromInt(yInt)), z(Fixed::fromInt(zInt)) {}

    [[nodiscard]] static constexpr Vec3 of(
        std::int32_t xInt, std::int32_t yInt, std::int32_t zInt) noexcept {
        return Vec3{
            Fixed::fromInt(xInt),
            Fixed::fromInt(yInt),
            Fixed::fromInt(zInt)};
    }

    [[nodiscard]] static constexpr Vec3 fromInts(
        std::int32_t xInt, std::int32_t yInt, std::int32_t zInt) noexcept {
        return Vec3{
            Fixed::fromInt(xInt),
            Fixed::fromInt(yInt),
            Fixed::fromInt(zInt)};
    }

    [[nodiscard]] static constexpr Fixed dot(Vec3 a, Vec3 b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    [[nodiscard]] static constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
        return Vec3{
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
    }

    [[nodiscard]] constexpr Fixed lengthSq() const noexcept {
        return x * x + y * y + z * z;
    }

    [[nodiscard]] constexpr Fixed length() const noexcept {
        const std::int64_t r2 =
            static_cast<std::int64_t>(x.raw) * static_cast<std::int64_t>(x.raw) +
            static_cast<std::int64_t>(y.raw) * static_cast<std::int64_t>(y.raw) +
            static_cast<std::int64_t>(z.raw) * static_cast<std::int64_t>(z.raw);
        return Fixed::fromRaw(static_cast<std::int32_t>(deterministicSqrt64(r2)));
    }

    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
        return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
    }

    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
        return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
    }

    friend constexpr Vec3 operator-(Vec3 a) noexcept {
        return Vec3{-a.x, -a.y, -a.z};
    }

    friend constexpr Vec3 operator*(Vec3 v, Fixed s) noexcept {
        return Vec3{v.x * s, v.y * s, v.z * s};
    }

    friend constexpr Vec3 operator*(Fixed s, Vec3 v) noexcept {
        return Vec3{v.x * s, v.y * s, v.z * s};
    }

    friend constexpr Vec3 operator/(Vec3 v, Fixed s) noexcept {
        return Vec3{v.x / s, v.y / s, v.z / s};
    }

    friend constexpr bool operator==(Vec3 a, Vec3 b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    friend constexpr bool operator!=(Vec3 a, Vec3 b) noexcept {
        return !(a == b);
    }
};

using FixedVec3 = Vec3;

} // namespace realscript::game

