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
        return Vec2{Fixed(xRaw), Fixed(yRaw)};
    }

    [[nodiscard]] static constexpr Vec2 fromRaw(std::int32_t xRaw, std::int32_t yRaw) noexcept {
        return Vec2{Fixed(xRaw), Fixed(yRaw)};
    }

    [[nodiscard]] static constexpr std::int64_t dotRaw64(Vec2 a, Vec2 b) noexcept {
        return (static_cast<std::int64_t>(a.x.raw) * static_cast<std::int64_t>(b.x.raw) +
                static_cast<std::int64_t>(a.y.raw) * static_cast<std::int64_t>(b.y.raw)) >> 8;
    }

    [[nodiscard]] static constexpr Fixed dot(Vec2 a, Vec2 b) noexcept {
        return Fixed::fromRaw(Fixed::clampRaw64(dotRaw64(a, b)));
    }

    [[nodiscard]] static constexpr std::int64_t crossRaw64(Vec2 a, Vec2 b) noexcept {
        return (static_cast<std::int64_t>(a.x.raw) * static_cast<std::int64_t>(b.y.raw) -
                static_cast<std::int64_t>(a.y.raw) * static_cast<std::int64_t>(b.x.raw)) >> 8;
    }

    [[nodiscard]] static constexpr Fixed cross(Vec2 a, Vec2 b) noexcept {
        return Fixed::fromRaw(Fixed::clampRaw64(crossRaw64(a, b)));
    }

    [[nodiscard]] constexpr std::int64_t lengthSqRaw64() const noexcept {
        return dotRaw64(*this, *this);
    }

    [[nodiscard]] constexpr Fixed lengthSq() const noexcept {
        return Fixed::fromRaw(Fixed::clampRaw64(lengthSqRaw64()));
    }

    [[nodiscard]] constexpr Fixed length() const noexcept {
        const std::int64_t r2 =
            static_cast<std::int64_t>(x.raw) * static_cast<std::int64_t>(x.raw) +
            static_cast<std::int64_t>(y.raw) * static_cast<std::int64_t>(y.raw);
        return Fixed(static_cast<std::int32_t>(deterministicSqrt64(r2)));
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
        return Vec2{Fixed(-a.x.raw), Fixed(-a.y.raw)};
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

    [[nodiscard]] static constexpr std::int64_t dotRaw64(Vec3 a, Vec3 b) noexcept {
        return (static_cast<std::int64_t>(a.x.raw) * static_cast<std::int64_t>(b.x.raw) +
                static_cast<std::int64_t>(a.y.raw) * static_cast<std::int64_t>(b.y.raw) +
                static_cast<std::int64_t>(a.z.raw) * static_cast<std::int64_t>(b.z.raw)) >> 8;
    }

    [[nodiscard]] static constexpr Fixed dot(Vec3 a, Vec3 b) noexcept {
        return Fixed::fromRaw(Fixed::clampRaw64(dotRaw64(a, b)));
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

    [[nodiscard]] constexpr std::int64_t lengthSqRaw64() const noexcept {
        return dotRaw64(*this, *this);
    }

    [[nodiscard]] constexpr Fixed lengthSq() const noexcept {
        return Fixed::fromRaw(Fixed::clampRaw64(lengthSqRaw64()));
    }

    [[nodiscard]] constexpr Fixed length() const noexcept {
        const std::int64_t r2 =
            static_cast<std::int64_t>(x.raw) * static_cast<std::int64_t>(x.raw) +
            static_cast<std::int64_t>(y.raw) * static_cast<std::int64_t>(y.raw) +
            static_cast<std::int64_t>(z.raw) * static_cast<std::int64_t>(z.raw);
        return Fixed(static_cast<std::int32_t>(deterministicSqrt64(r2)));
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
        return Vec3{Fixed(-a.x.raw), Fixed(-a.y.raw), Fixed(-a.z.raw)};
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
        Quat qa = a.normalized();
        Quat qb = b.normalized();
        Fixed cosTheta = dot(qa, qb);
        if (cosTheta.raw < 0) {
            qb = Quat{-qb.x, -qb.y, -qb.z, -qb.w};
            cosTheta = -cosTheta;
        }
        if (cosTheta.raw >= 250) {
            return lerp(qa, qb, t);
        }
        const std::int64_t sin2Raw = 65536LL -
            static_cast<std::int64_t>(cosTheta.raw) * static_cast<std::int64_t>(cosTheta.raw);
        const Fixed sinTheta = Fixed::fromRaw(
            static_cast<std::int32_t>(deterministicSqrt64(sin2Raw > 0 ? sin2Raw : 0)));
        if (sinTheta.raw <= 4) {
            return lerp(qa, qb, t);
        }
        const Fixed theta = deterministicAtan2(sinTheta, cosTheta);
        const Fixed s0 = deterministicSin((Fixed::fromInt(1) - t) * theta) / sinTheta;
        const Fixed s1 = deterministicSin(t * theta) / sinTheta;
        return Quat{
            qa.x * s0 + qb.x * s1,
            qa.y * s0 + qb.y * s1,
            qa.z * s0 + qb.z * s1,
            qa.w * s0 + qb.w * s1}.normalized();
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

/**
 * 64-Bit Deterministic Fixed-Point Type (`Fixed64`, Q32.32: 32-bit signed integer + 32-bit fraction).
 * Supports large-world coordinates (+/- 2.14 billion units) with sub-nanometer precision.
 */
struct Fixed64 {
    static constexpr std::uint32_t FRAC_BITS = 32;
    static constexpr std::int64_t SCALE = (1LL << 32);
    std::int64_t raw{0};

    constexpr Fixed64() noexcept = default;
    constexpr explicit Fixed64(std::int64_t rawValue) noexcept : raw(rawValue) {}

    [[nodiscard]] static constexpr Fixed64 fromRaw(std::int64_t rawValue) noexcept {
        return Fixed64(rawValue);
    }
    [[nodiscard]] static constexpr Fixed64 fromInt(std::int64_t whole) noexcept {
        return Fixed64(whole * SCALE);
    }
    [[nodiscard]] static constexpr Fixed64 fromFixed(Fixed f) noexcept {
        return Fixed64(static_cast<std::int64_t>(f.raw) << 24);
    }
    [[nodiscard]] constexpr std::int64_t toIntFloor() const noexcept {
        return raw >> 32;
    }
    [[nodiscard]] constexpr Fixed toFixed() const noexcept {
        return Fixed::fromRaw(static_cast<std::int32_t>(raw >> 24));
    }
    [[nodiscard]] constexpr double toDouble() const noexcept {
        return static_cast<double>(raw) / static_cast<double>(SCALE);
    }

    constexpr Fixed64 operator-() const noexcept { return Fixed64(-raw); }
    constexpr Fixed64 operator+(Fixed64 rhs) const noexcept { return Fixed64(raw + rhs.raw); }
    constexpr Fixed64 operator-(Fixed64 rhs) const noexcept { return Fixed64(raw - rhs.raw); }
    constexpr Fixed64 operator*(Fixed64 rhs) const noexcept {
        const std::int64_t aHi = raw >> 32;
        const std::uint64_t aLo = static_cast<std::uint64_t>(raw) & 0xFFFFFFFFULL;
        const std::int64_t bHi = rhs.raw >> 32;
        const std::uint64_t bLo = static_cast<std::uint64_t>(rhs.raw) & 0xFFFFFFFFULL;
        const std::uint64_t hiPart =
            static_cast<std::uint64_t>(aHi * bHi) << 32;
        const std::uint64_t midPart = static_cast<std::uint64_t>(
            aHi * static_cast<std::int64_t>(bLo) +
            bHi * static_cast<std::int64_t>(aLo));
        const std::uint64_t loPart = (aLo * bLo) >> 32;
        return Fixed64(static_cast<std::int64_t>(hiPart + midPart + loPart));
    }
    constexpr Fixed64 operator/(Fixed64 rhs) const noexcept {
        if (rhs.raw == 0) return Fixed64(0);
        const bool neg = (raw < 0) != (rhs.raw < 0);
        const std::uint64_t ua = raw < 0
            ? (0ULL - static_cast<std::uint64_t>(raw))
            : static_cast<std::uint64_t>(raw);
        const std::uint64_t ub = rhs.raw < 0
            ? (0ULL - static_cast<std::uint64_t>(rhs.raw))
            : static_cast<std::uint64_t>(rhs.raw);
        const std::uint64_t qHi = ua / ub;
        std::uint64_t rem = ua % ub;
        std::uint64_t qLo = 0;
        for (int bit = 31; bit >= 0; --bit) {
            if (rem >= ub - rem) {
                rem = rem - (ub - rem);
                qLo |= (1ULL << bit);
            } else {
                rem <<= 1;
            }
        }
        const std::uint64_t q = (qHi << 32) | qLo;
        return Fixed64(neg
            ? static_cast<std::int64_t>(0ULL - q)
            : static_cast<std::int64_t>(q));
    }
    constexpr bool operator==(Fixed64 rhs) const noexcept { return raw == rhs.raw; }
    constexpr bool operator!=(Fixed64 rhs) const noexcept { return raw != rhs.raw; }
    constexpr bool operator<(Fixed64 rhs) const noexcept { return raw < rhs.raw; }
    constexpr bool operator<=(Fixed64 rhs) const noexcept { return raw <= rhs.raw; }
    constexpr bool operator>(Fixed64 rhs) const noexcept { return raw > rhs.raw; }
    constexpr bool operator>=(Fixed64 rhs) const noexcept { return raw >= rhs.raw; }
};

struct Vec4 {
    Fixed x{};
    Fixed y{};
    Fixed z{};
    Fixed w{};

    constexpr Vec4() noexcept = default;
    constexpr Vec4(Fixed xVal, Fixed yVal, Fixed zVal, Fixed wVal) noexcept
        : x(xVal), y(yVal), z(zVal), w(wVal) {}
    constexpr Vec4(std::int32_t xInt, std::int32_t yInt, std::int32_t zInt, std::int32_t wInt) noexcept
        : x(Fixed::fromInt(xInt)), y(Fixed::fromInt(yInt)), z(Fixed::fromInt(zInt)), w(Fixed::fromInt(wInt)) {}

    [[nodiscard]] static constexpr Vec4 of(
        std::int32_t xInt, std::int32_t yInt, std::int32_t zInt, std::int32_t wInt) noexcept {
        return Vec4{Fixed::fromInt(xInt), Fixed::fromInt(yInt), Fixed::fromInt(zInt), Fixed::fromInt(wInt)};
    }
    [[nodiscard]] static constexpr Fixed dot(Vec4 a, Vec4 b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }
    [[nodiscard]] constexpr Vec3 xyz() const noexcept { return Vec3{x, y, z}; }

    friend constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept {
        return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    friend constexpr Vec4 operator-(Vec4 a, Vec4 b) noexcept {
        return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    friend constexpr Vec4 operator*(Vec4 v, Fixed s) noexcept {
        return Vec4{v.x * s, v.y * s, v.z * s, v.w * s};
    }
    friend constexpr bool operator==(Vec4 a, Vec4 b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
    friend constexpr bool operator!=(Vec4 a, Vec4 b) noexcept { return !(a == b); }
};

struct Mat4 {
    std::array<Fixed, 16> m{};

    [[nodiscard]] static constexpr Mat4 identity() noexcept {
        Mat4 res{};
        res.m[0] = Fixed::fromInt(1);
        res.m[5] = Fixed::fromInt(1);
        res.m[10] = Fixed::fromInt(1);
        res.m[15] = Fixed::fromInt(1);
        return res;
    }

    [[nodiscard]] static constexpr Mat4 trs(Vec3 translation, Quat rotation, Vec3 scale) noexcept {
        const Quat q = rotation.normalized();
        const Fixed two = Fixed::fromInt(2);
        const Fixed one = Fixed::fromInt(1);
        const Fixed xx = q.x * q.x * two;
        const Fixed yy = q.y * q.y * two;
        const Fixed zz = q.z * q.z * two;
        const Fixed xy = q.x * q.y * two;
        const Fixed xz = q.x * q.z * two;
        const Fixed yz = q.y * q.z * two;
        const Fixed wx = q.w * q.x * two;
        const Fixed wy = q.w * q.y * two;
        const Fixed wz = q.w * q.z * two;

        Mat4 res{};
        res.m[0] = (one - (yy + zz)) * scale.x;
        res.m[1] = (xy - wz) * scale.y;
        res.m[2] = (xz + wy) * scale.z;
        res.m[3] = translation.x;

        res.m[4] = (xy + wz) * scale.x;
        res.m[5] = (one - (xx + zz)) * scale.y;
        res.m[6] = (yz - wx) * scale.z;
        res.m[7] = translation.y;

        res.m[8] = (xz - wy) * scale.x;
        res.m[9] = (yz + wx) * scale.y;
        res.m[10] = (one - (xx + yy)) * scale.z;
        res.m[11] = translation.z;

        res.m[12] = Fixed::fromInt(0);
        res.m[13] = Fixed::fromInt(0);
        res.m[14] = Fixed::fromInt(0);
        res.m[15] = one;
        return res;
    }

    [[nodiscard]] constexpr Vec3 transformPoint(Vec3 p) const noexcept {
        return Vec3{
            m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
            m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    }

    [[nodiscard]] constexpr Vec3 transformVector(Vec3 v) const noexcept {
        return Vec3{
            m[0] * v.x + m[1] * v.y + m[2] * v.z,
            m[4] * v.x + m[5] * v.y + m[6] * v.z,
            m[8] * v.x + m[9] * v.y + m[10] * v.z};
    }

    friend constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
        Mat4 res{};
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                Fixed sum{};
                for (std::size_t k = 0; k < 4; ++k) {
                    sum = sum + a.m[r * 4 + k] * b.m[k * 4 + c];
                }
                res.m[r * 4 + c] = sum;
            }
        }
        return res;
    }
};

struct Transform3D {
    Vec3 position{};
    Quat rotation{Quat::identity()};
    Vec3 scale{Fixed::fromInt(1), Fixed::fromInt(1), Fixed::fromInt(1)};

    constexpr Transform3D() noexcept = default;
    constexpr Transform3D(Vec3 pos, Quat rot, Vec3 scl) noexcept
        : position(pos), rotation(rot), scale(scl) {}

    [[nodiscard]] static constexpr Transform3D identity() noexcept {
        return Transform3D{};
    }

    [[nodiscard]] constexpr Vec3 transformPoint(Vec3 localPoint) const noexcept {
        const Vec3 scaled{
            localPoint.x * scale.x,
            localPoint.y * scale.y,
            localPoint.z * scale.z};
        return position + rotation.rotate(scaled);
    }

    [[nodiscard]] constexpr Vec3 inverseTransformPoint(Vec3 worldPoint) const noexcept {
        const Vec3 unrotated = rotation.conjugate().rotate(worldPoint - position);
        return Vec3{
            scale.x.raw != 0 ? (unrotated.x / scale.x) : Fixed{},
            scale.y.raw != 0 ? (unrotated.y / scale.y) : Fixed{},
            scale.z.raw != 0 ? (unrotated.z / scale.z) : Fixed{}};
    }

    [[nodiscard]] static constexpr Transform3D combine(
        const Transform3D& parent,
        const Transform3D& local) noexcept {
        return Transform3D{
            parent.transformPoint(local.position),
            (parent.rotation * local.rotation).normalized(),
            Vec3{
                parent.scale.x * local.scale.x,
                parent.scale.y * local.scale.y,
                parent.scale.z * local.scale.z}};
    }

    [[nodiscard]] constexpr Mat4 toMat4() const noexcept {
        return Mat4::trs(position, rotation, scale);
    }
};

struct AABB {
    Vec3 min{};
    Vec3 max{};

    constexpr AABB() noexcept = default;
    constexpr AABB(Vec3 minVal, Vec3 maxVal) noexcept : min(minVal), max(maxVal) {}

    [[nodiscard]] static constexpr AABB fromCenterExtents(Vec3 center, Vec3 extents) noexcept {
        return AABB{center - extents, center + extents};
    }

    [[nodiscard]] constexpr Vec3 center() const noexcept {
        return (min + max) / Fixed::fromInt(2);
    }

    [[nodiscard]] constexpr Vec3 extents() const noexcept {
        return (max - min) / Fixed::fromInt(2);
    }

    [[nodiscard]] constexpr bool contains(Vec3 p) const noexcept {
        return p.x >= min.x && p.x <= max.x &&
               p.y >= min.y && p.y <= max.y &&
               p.z >= min.z && p.z <= max.z;
    }

    [[nodiscard]] constexpr bool intersects(const AABB& other) const noexcept {
        return min.x <= other.max.x && max.x >= other.min.x &&
               min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }
};

struct Ray {
    Vec3 origin{};
    Vec3 direction{};

    constexpr Ray() noexcept = default;
    constexpr Ray(Vec3 orig, Vec3 dir) noexcept : origin(orig), direction(dir) {}

    [[nodiscard]] constexpr Vec3 at(Fixed t) const noexcept {
        return origin + direction * t;
    }

    [[nodiscard]] constexpr bool intersectsAABB(const AABB& box, Fixed* outTMin = nullptr) const noexcept {
        Fixed tmin = Fixed::fromRaw(-2147483647);
        Fixed tmax = Fixed::fromRaw(2147483647);

        const Fixed orig[3] = {origin.x, origin.y, origin.z};
        const Fixed dir[3] = {direction.x, direction.y, direction.z};
        const Fixed bmin[3] = {box.min.x, box.min.y, box.min.z};
        const Fixed bmax[3] = {box.max.x, box.max.y, box.max.z};

        for (int i = 0; i < 3; ++i) {
            if (dir[i].raw == 0) {
                if (orig[i] < bmin[i] || orig[i] > bmax[i]) return false;
            } else {
                Fixed t1 = (bmin[i] - orig[i]) / dir[i];
                Fixed t2 = (bmax[i] - orig[i]) / dir[i];
                if (t1 > t2) {
                    const Fixed tmp = t1;
                    t1 = t2;
                    t2 = tmp;
                }
                if (t1 > tmin) tmin = t1;
                if (t2 < tmax) tmax = t2;
                if (tmin > tmax) return false;
            }
        }
        if (tmax.raw < 0) return false;
        if (outTMin) *outTMin = tmin.raw >= 0 ? tmin : tmax;
        return true;
    }
};

} // namespace realscript::game


