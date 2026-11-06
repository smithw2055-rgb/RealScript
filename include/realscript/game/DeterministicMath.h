#pragma once

#include "realscript/runtime/FiberScheduler.h"

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

    [[nodiscard]] static constexpr Vec2 lerp(Vec2 a, Vec2 b, Fixed t) noexcept {
        return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
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

    [[nodiscard]] static constexpr Vec3 lerp(Vec3 a, Vec3 b, Fixed t) noexcept {
        return Vec3{
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t};
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

    [[nodiscard]] constexpr Vec3 normalized() const noexcept {
        const Fixed len = length();
        if (len.raw == 0) return Vec3{};
        return Vec3{x / len, y / len, z / len};
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

[[nodiscard]] constexpr Fixed fixedPi() noexcept { return Fixed::fromRaw(804); }
[[nodiscard]] constexpr Fixed fixedHalfPi() noexcept { return Fixed::fromRaw(402); }
[[nodiscard]] constexpr Fixed fixedTwoPi() noexcept { return Fixed::fromRaw(1608); }

[[nodiscard]] constexpr Fixed deterministicSin(Fixed radians) noexcept {
    std::int32_t r = radians.raw % 1608;
    if (r > 804) r -= 1608;
    if (r < -804) r += 1608;
    const std::int32_t absR = r < 0 ? -r : r;
    const std::int64_t y0 =
        (static_cast<std::int64_t>(r) * static_cast<std::int64_t>(804 - absR) * 262144LL) /
        (804LL * 804LL);
    const std::int64_t absY0 = y0 < 0 ? -y0 : y0;
    const std::int64_t y = (y0 * 775LL + ((y0 * absY0) / 65536LL) * 225LL) / 1000LL;
    return Fixed::fromRaw(static_cast<std::int32_t>(y / 256LL));
}

[[nodiscard]] constexpr Fixed deterministicCos(Fixed radians) noexcept {
    return deterministicSin(Fixed::fromRaw(radians.raw + 402));
}

[[nodiscard]] constexpr Fixed deterministicAtan2(Fixed y, Fixed x) noexcept {
    if (x.raw == 0 && y.raw == 0) return Fixed::fromRaw(0);
    const std::int64_t ax = x.raw < 0 ? -static_cast<std::int64_t>(x.raw) : static_cast<std::int64_t>(x.raw);
    const std::int64_t ay = y.raw < 0 ? -static_cast<std::int64_t>(y.raw) : static_cast<std::int64_t>(y.raw);
    const std::int64_t mn = ax < ay ? ax : ay;
    const std::int64_t mx = ax > ay ? ax : ay;
    const std::int64_t z = (mn * 65536LL) / mx;
    std::int64_t angle = (201LL * z + (70LL * z * (65536LL - z)) / 65536LL) / 65536LL;
    if (ay > ax) angle = 402LL - angle;
    if (x.raw < 0) angle = 804LL - angle;
    if (y.raw < 0) angle = -angle;
    return Fixed::fromRaw(static_cast<std::int32_t>(angle));
}

[[nodiscard]] constexpr Fixed deterministicLerp(Fixed a, Fixed b, Fixed t) noexcept {
    return a + (b - a) * t;
}

struct Quat {
    Fixed x{};
    Fixed y{};
    Fixed z{};
    Fixed w{Fixed::fromInt(1)};

    constexpr Quat() noexcept = default;
    constexpr Quat(Fixed xVal, Fixed yVal, Fixed zVal, Fixed wVal) noexcept
        : x(xVal), y(yVal), z(zVal), w(wVal) {}

    [[nodiscard]] static constexpr Quat identity() noexcept {
        return Quat{Fixed::fromInt(0), Fixed::fromInt(0), Fixed::fromInt(0), Fixed::fromInt(1)};
    }

    [[nodiscard]] static constexpr Quat fromRaw(
        std::int32_t xRaw, std::int32_t yRaw, std::int32_t zRaw, std::int32_t wRaw) noexcept {
        return Quat{
            Fixed::fromRaw(xRaw),
            Fixed::fromRaw(yRaw),
            Fixed::fromRaw(zRaw),
            Fixed::fromRaw(wRaw)};
    }

    [[nodiscard]] static constexpr Quat fromAxisAngle(Vec3 axis, Fixed angle) noexcept {
        const Vec3 n = axis.normalized();
        const Fixed half = angle / Fixed::fromInt(2);
        const Fixed s = deterministicSin(half);
        const Fixed c = deterministicCos(half);
        return Quat{n.x * s, n.y * s, n.z * s, c};
    }

    [[nodiscard]] static constexpr Fixed dot(Quat a, Quat b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    [[nodiscard]] constexpr Fixed lengthSq() const noexcept {
        return x * x + y * y + z * z + w * w;
    }

    [[nodiscard]] constexpr Fixed length() const noexcept {
        const std::int64_t r2 =
            static_cast<std::int64_t>(x.raw) * static_cast<std::int64_t>(x.raw) +
            static_cast<std::int64_t>(y.raw) * static_cast<std::int64_t>(y.raw) +
            static_cast<std::int64_t>(z.raw) * static_cast<std::int64_t>(z.raw) +
            static_cast<std::int64_t>(w.raw) * static_cast<std::int64_t>(w.raw);
        return Fixed::fromRaw(static_cast<std::int32_t>(deterministicSqrt64(r2)));
    }

    [[nodiscard]] constexpr Quat normalized() const noexcept {
        const Fixed len = length();
        if (len.raw == 0) return identity();
        return Quat{x / len, y / len, z / len, w / len};
    }

    [[nodiscard]] constexpr Quat conjugate() const noexcept {
        return Quat{-x, -y, -z, w};
    }

    [[nodiscard]] constexpr Vec3 rotate(Vec3 v) const noexcept {
        const Vec3 u{x, y, z};
        const Vec3 uv = Vec3::cross(u, v);
        const Vec3 uuv = Vec3::cross(u, uv);
        const Fixed two = Fixed::fromInt(2);
        return v + (uv * w + uuv) * two;
    }

    [[nodiscard]] static constexpr Quat lerp(Quat a, Quat b, Fixed t) noexcept {
        Quat target = b;
        if (dot(a, b).raw < 0) {
            target = Quat{-b.x, -b.y, -b.z, -b.w};
        }
        return Quat{
            a.x + (target.x - a.x) * t,
            a.y + (target.y - a.y) * t,
            a.z + (target.z - a.z) * t,
            a.w + (target.w - a.w) * t}.normalized();
    }

    [[nodiscard]] static constexpr Quat slerp(Quat a, Quat b, Fixed t) noexcept {
        return lerp(a, b, t);
    }

    friend constexpr Quat operator*(Quat a, Quat b) noexcept {
        return Quat{
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    friend constexpr bool operator==(Quat a, Quat b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    friend constexpr bool operator!=(Quat a, Quat b) noexcept {
        return !(a == b);
    }
};

using FixedQuat = Quat;

} // namespace realscript::game


