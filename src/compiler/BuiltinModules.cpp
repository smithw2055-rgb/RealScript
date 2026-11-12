#include "CompilationInternal.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::compiler::detail {

const std::string& builtinDeterministicMathModuleSource() {
    static const std::string kSource = R"RS(
module RealScript.Math;

public class Math {
    public static int Abs(int v) {
        return v < 0 ? -v : v;
    }

    public static long Abs64(long v) {
        return v < 0 ? -v : v;
    }

    public static int Min(int a, int b) {
        return a <= b ? a : b;
    }

    public static int Max(int a, int b) {
        return a >= b ? a : b;
    }

    public static int Clamp(int v, int lo, int hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    public static long Sqrt64(long n) {
        if (n <= 0) return 0;
        long lo = 1;
        long hi = n < 3037000499 ? n : 3037000499;
        long ans = 1;
        while (lo <= hi) {
            long mid = lo + (hi - lo) / 2;
            if (mid <= n / mid) {
                ans = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return ans;
    }

    public static Fixed Pi() {
        return new Fixed(804);
    }

    public static Fixed HalfPi() {
        return new Fixed(402);
    }

    public static Fixed TwoPi() {
        return new Fixed(1608);
    }

    public static Fixed Sin(Fixed radians) {
        return Fixed.Sin(radians);
    }

    public static Fixed Cos(Fixed radians) {
        return Fixed.Cos(radians);
    }

    public static Fixed Atan2(Fixed y, Fixed x) {
        return Fixed.Atan2(y, x);
    }

    public static Fixed Lerp(Fixed a, Fixed b, Fixed t) {
        return Fixed.Lerp(a, b, t);
    }

    public static float Absf(float v) {
        return v < 0.0f ? -v : v;
    }

    public static float Minf(float a, float b) {
        return a <= b ? a : b;
    }

    public static float Maxf(float a, float b) {
        return a >= b ? a : b;
    }

    public static float Clampf(float v, float lo, float hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    public static float Lerpf(float a, float b, float t) {
        return a + (b - a) * t;
    }

    public static float Sqrtf(float v) {
        if (v <= 0.0f) return 0.0f;
        float x = v >= 1.0f ? v * 0.5f : 1.0f;
        for (int i = 0; i < 8; i = i + 1) {
            x = 0.5f * (x + v / x);
        }
        return x;
    }

    public static float Sinf(float radians) {
        if (radians != radians || (radians - radians) != 0.0f) return 0.0f;
        if (radians > 1000000000.0f || radians < -1000000000.0f) return 0.0f;
        float twoPi = 6.2831853f;
        float pi = 3.14159265f;
        float r = radians;
        if (r > twoPi || r < -twoPi) {
            long q = (long)((double)r / 6.283185307179586);
            r = (float)((double)r - (double)q * 6.283185307179586);
        }
        if (r > pi) r = r - twoPi;
        if (r < -pi) r = r + twoPi;
        float absR = r < 0.0f ? -r : r;
        float y = 1.27323954f * r - 0.405284735f * r * absR;
        float absY = y < 0.0f ? -y : y;
        return 0.225f * (y * absY - y) + y;
    }

    public static float Cosf(float radians) {
        return Math.Sinf(radians + 1.5707963f);
    }
}

public struct Fixed {
    public int raw;

    public Fixed(int rawValue) {
        raw = rawValue;
    }

    public static Fixed Zero() {
        return new Fixed(0);
    }

    public static Fixed One() {
        return new Fixed(256);
    }

    public static Fixed Half() {
        return new Fixed(128);
    }

    public static Fixed Pi() {
        return new Fixed(804);
    }

    public static Fixed HalfPi() {
        return new Fixed(402);
    }

    public static Fixed TwoPi() {
        return new Fixed(1608);
    }

    public static Fixed Raw(int rawValue) {
        return new Fixed(rawValue);
    }

    public static Fixed Of(int whole) {
        return new Fixed(whole * 256);
    }

    public static Fixed Ratio(int num, int den) {
        return new Fixed((int)(((long)num * 256) / (long)den));
    }

    public int ToInt() {
        return raw >= 0 ? raw / 256 : (raw - 255) / 256;
    }

    public int Trunc() {
        return raw / 256;
    }

    public int Floor() {
        return raw >= 0 ? raw / 256 : (raw - 255) / 256;
    }

    public int Round() {
        return raw >= 0 ? (raw + 128) / 256 : (raw - 128) / 256;
    }

    public static Fixed Abs(Fixed v) {
        return new Fixed(v.raw < 0 ? -v.raw : v.raw);
    }

    public static Fixed Min(Fixed a, Fixed b) {
        return a.raw <= b.raw ? a : b;
    }

    public static Fixed Max(Fixed a, Fixed b) {
        return a.raw >= b.raw ? a : b;
    }

    public static Fixed Clamp(Fixed v, Fixed lo, Fixed hi) {
        if (v.raw < lo.raw) return lo;
        if (v.raw > hi.raw) return hi;
        return v;
    }

    public static Fixed Lerp(Fixed a, Fixed b, Fixed t) {
        return a + (b - a) * t;
    }

    public static Fixed Sqrt(Fixed v) {
        if (v.raw <= 0) return new Fixed(0);
        return new Fixed((int)Math.Sqrt64((long)v.raw * 256));
    }

    public static Fixed Sin(Fixed radians) {
        int r = radians.raw % 1608;
        if (r > 804) r = r - 1608;
        if (r < -804) r = r + 1608;
        int absR = r < 0 ? -r : r;
        long y0 = ((long)r * (long)(804 - absR) * 262144) / (804 * 804);
        long absY0 = y0 < 0 ? -y0 : y0;
        long y = (y0 * 775 + ((y0 * absY0) / 65536) * 225) / 1000;
        return new Fixed((int)(y / 256));
    }

    public static Fixed Cos(Fixed radians) {
        return Fixed.Sin(new Fixed(radians.raw + 402));
    }

    public static Fixed Atan2(Fixed y, Fixed x) {
        if (x.raw == 0 && y.raw == 0) return new Fixed(0);
        long ax = x.raw < 0 ? -(long)x.raw : (long)x.raw;
        long ay = y.raw < 0 ? -(long)y.raw : (long)y.raw;
        long mn = ax < ay ? ax : ay;
        long mx = ax > ay ? ax : ay;
        long z = (mn * 65536) / mx;
        long angle = (201 * z + (70 * z * (65536 - z)) / 65536) / 65536;
        if (ay > ax) angle = 402 - angle;
        if (x.raw < 0) angle = 804 - angle;
        if (y.raw < 0) angle = -angle;
        return new Fixed((int)angle);
    }

    public static Fixed operator +(Fixed a) {
        return a;
    }

    public static Fixed operator -(Fixed a) {
        return new Fixed(-a.raw);
    }

    public static Fixed operator +(Fixed a, Fixed b) {
        return new Fixed(a.raw + b.raw);
    }

    public static Fixed operator +(Fixed a, int b) {
        return new Fixed(a.raw + b * 256);
    }

    public static Fixed operator +(int a, Fixed b) {
        return new Fixed(a * 256 + b.raw);
    }

    public static Fixed operator -(Fixed a, Fixed b) {
        return new Fixed(a.raw - b.raw);
    }

    public static Fixed operator -(Fixed a, int b) {
        return new Fixed(a.raw - b * 256);
    }

    public static Fixed operator -(int a, Fixed b) {
        return new Fixed(a * 256 - b.raw);
    }

    public static Fixed operator *(Fixed a, Fixed b) {
        return new Fixed((int)(((long)a.raw * (long)b.raw) / 256));
    }

    public static Fixed operator *(Fixed a, int scalar) {
        return new Fixed(a.raw * scalar);
    }

    public static Fixed operator *(int scalar, Fixed a) {
        return new Fixed(scalar * a.raw);
    }

    public static Fixed operator /(Fixed a, Fixed b) {
        return new Fixed((int)(((long)a.raw * 256) / (long)b.raw));
    }

    public static Fixed operator /(Fixed a, int scalar) {
        return new Fixed(a.raw / scalar);
    }

    public static Fixed operator %(Fixed a, Fixed b) {
        return new Fixed(a.raw % b.raw);
    }

    public static bool operator ==(Fixed a, Fixed b) {
        return a.raw == b.raw;
    }

    public static bool operator ==(Fixed a, int b) {
        return a.raw == b * 256;
    }

    public static bool operator ==(int a, Fixed b) {
        return a * 256 == b.raw;
    }

    public static bool operator !=(Fixed a, Fixed b) {
        return a.raw != b.raw;
    }

    public static bool operator !=(Fixed a, int b) {
        return a.raw != b * 256;
    }

    public static bool operator !=(int a, Fixed b) {
        return a * 256 != b.raw;
    }

    public static bool operator <(Fixed a, Fixed b) {
        return a.raw < b.raw;
    }

    public static bool operator <(Fixed a, int b) {
        return a.raw < b * 256;
    }

    public static bool operator <=(Fixed a, Fixed b) {
        return a.raw <= b.raw;
    }

    public static bool operator <=(Fixed a, int b) {
        return a.raw <= b * 256;
    }

    public static bool operator >(Fixed a, Fixed b) {
        return a.raw > b.raw;
    }

    public static bool operator >(Fixed a, int b) {
        return a.raw > b * 256;
    }

    public static bool operator >=(Fixed a, Fixed b) {
        return a.raw >= b.raw;
    }

    public static bool operator >=(Fixed a, int b) {
        return a.raw >= b * 256;
    }
}

public struct Fixed16 {
    public int raw;

    public Fixed16(int rawValue) {
        raw = rawValue;
    }

    public static Fixed16 Zero() {
        return new Fixed16(0);
    }

    public static Fixed16 One() {
        return new Fixed16(65536);
    }

    public static Fixed16 Half() {
        return new Fixed16(32768);
    }

    public static Fixed16 Pi() {
        return new Fixed16(205887);
    }

    public static Fixed16 HalfPi() {
        return new Fixed16(102944);
    }

    public static Fixed16 TwoPi() {
        return new Fixed16(411775);
    }

    public static Fixed16 Raw(int rawValue) {
        return new Fixed16(rawValue);
    }

    public static Fixed16 Of(int whole) {
        return new Fixed16(whole * 65536);
    }

    public static Fixed16 Ratio(int num, int den) {
        return new Fixed16((int)(((long)num * 65536) / (long)den));
    }

    public int ToInt() {
        return raw >= 0 ? raw / 65536 : (raw - 65535) / 65536;
    }

    public int Trunc() {
        return raw / 65536;
    }

    public int Floor() {
        return raw >= 0 ? raw / 65536 : (raw - 65535) / 65536;
    }

    public int Round() {
        return raw >= 0 ? (raw + 32768) / 65536 : (raw - 32768) / 65536;
    }

    public static Fixed16 Abs(Fixed16 v) {
        return new Fixed16(v.raw < 0 ? -v.raw : v.raw);
    }

    public static Fixed16 Min(Fixed16 a, Fixed16 b) {
        return a.raw <= b.raw ? a : b;
    }

    public static Fixed16 Max(Fixed16 a, Fixed16 b) {
        return a.raw >= b.raw ? a : b;
    }

    public static Fixed16 Clamp(Fixed16 v, Fixed16 lo, Fixed16 hi) {
        if (v.raw < lo.raw) return lo;
        if (v.raw > hi.raw) return hi;
        return v;
    }

    public static Fixed16 Lerp(Fixed16 a, Fixed16 b, Fixed16 t) {
        return a + (b - a) * t;
    }

    public static Fixed16 Sqrt(Fixed16 v) {
        if (v.raw <= 0) return new Fixed16(0);
        return new Fixed16((int)Math.Sqrt64((long)v.raw * 65536));
    }

    public static Fixed16 Sin(Fixed16 radians) {
        int r = radians.raw % 411775;
        if (r > 205887) r = r - 411775;
        if (r < -205887) r = r + 411775;
        int absR = r < 0 ? -r : r;
        long y0 = (((long)r * (long)(205887 - absR)) / 205887) * 262144 / 205887;
        long absY0 = y0 < 0 ? -y0 : y0;
        long y = (y0 * 775 + ((y0 * absY0) / 65536) * 225) / 1000;
        return new Fixed16((int)y);
    }

    public static Fixed16 Cos(Fixed16 radians) {
        return Fixed16.Sin(new Fixed16(radians.raw + 102944));
    }

    public static Fixed16 Atan2(Fixed16 y, Fixed16 x) {
        if (x.raw == 0 && y.raw == 0) return new Fixed16(0);
        long ax = x.raw < 0 ? -(long)x.raw : (long)x.raw;
        long ay = y.raw < 0 ? -(long)y.raw : (long)y.raw;
        long mn = ax < ay ? ax : ay;
        long mx = ax > ay ? ax : ay;
        long z = (mn * 65536) / mx;
        long angle = (51472 * z + (17891 * z * (65536 - z)) / 65536) / 65536;
        if (ay > ax) angle = 102944 - angle;
        if (x.raw < 0) angle = 205887 - angle;
        if (y.raw < 0) angle = -angle;
        return new Fixed16((int)angle);
    }

    public static Fixed16 operator +(Fixed16 a) {
        return a;
    }

    public static Fixed16 operator -(Fixed16 a) {
        return new Fixed16(-a.raw);
    }

    public static Fixed16 operator +(Fixed16 a, Fixed16 b) {
        return new Fixed16(a.raw + b.raw);
    }

    public static Fixed16 operator +(Fixed16 a, int b) {
        return new Fixed16(a.raw + b * 65536);
    }

    public static Fixed16 operator +(int a, Fixed16 b) {
        return new Fixed16(a * 65536 + b.raw);
    }

    public static Fixed16 operator -(Fixed16 a, Fixed16 b) {
        return new Fixed16(a.raw - b.raw);
    }

    public static Fixed16 operator -(Fixed16 a, int b) {
        return new Fixed16(a.raw - b * 65536);
    }

    public static Fixed16 operator -(int a, Fixed16 b) {
        return new Fixed16(a * 65536 - b.raw);
    }

    public static Fixed16 operator *(Fixed16 a, Fixed16 b) {
        return new Fixed16((int)(((long)a.raw * (long)b.raw) / 65536));
    }

    public static Fixed16 operator *(Fixed16 a, int scalar) {
        return new Fixed16(a.raw * scalar);
    }

    public static Fixed16 operator *(int scalar, Fixed16 a) {
        return new Fixed16(scalar * a.raw);
    }

    public static Fixed16 operator /(Fixed16 a, Fixed16 b) {
        return new Fixed16((int)(((long)a.raw * 65536) / (long)b.raw));
    }

    public static Fixed16 operator /(Fixed16 a, int scalar) {
        return new Fixed16(a.raw / scalar);
    }

    public static Fixed16 operator %(Fixed16 a, Fixed16 b) {
        return new Fixed16(a.raw % b.raw);
    }

    public static bool operator ==(Fixed16 a, Fixed16 b) {
        return a.raw == b.raw;
    }

    public static bool operator ==(Fixed16 a, int b) {
        return a.raw == b * 65536;
    }

    public static bool operator ==(int a, Fixed16 b) {
        return a * 65536 == b.raw;
    }

    public static bool operator !=(Fixed16 a, Fixed16 b) {
        return a.raw != b.raw;
    }

    public static bool operator !=(Fixed16 a, int b) {
        return a.raw != b * 65536;
    }

    public static bool operator !=(int a, Fixed16 b) {
        return a * 65536 != b.raw;
    }

    public static bool operator <(Fixed16 a, Fixed16 b) {
        return a.raw < b.raw;
    }

    public static bool operator <=(Fixed16 a, Fixed16 b) {
        return a.raw <= b.raw;
    }

    public static bool operator >(Fixed16 a, Fixed16 b) {
        return a.raw > b.raw;
    }

    public static bool operator >=(Fixed16 a, Fixed16 b) {
        return a.raw >= b.raw;
    }
}

public struct Vec2 {
    public Fixed x;
    public Fixed y;

    public Vec2(Fixed xVal, Fixed yVal) {
        x = xVal;
        y = yVal;
    }

    public Vec2(int xInt, int yInt) {
        x = new Fixed(xInt * 256);
        y = new Fixed(yInt * 256);
    }

    public static Vec2 Zero() {
        return new Vec2(new Fixed(0), new Fixed(0));
    }

    public static Vec2 One() {
        return new Vec2(new Fixed(256), new Fixed(256));
    }

    public static Vec2 Of(int xInt, int yInt) {
        return new Vec2(new Fixed(xInt * 256), new Fixed(yInt * 256));
    }

    public static Vec2 Raw(int xRaw, int yRaw) {
        return new Vec2(new Fixed(xRaw), new Fixed(yRaw));
    }

    public static Fixed Dot(Vec2 a, Vec2 b) {
        return a.x * b.x + a.y * b.y;
    }

    public static Fixed Cross(Vec2 a, Vec2 b) {
        return a.x * b.y - a.y * b.x;
    }

    public Fixed LengthSq() {
        return x * x + y * y;
    }

    public Fixed Length() {
        long r2 = (long)x.raw * (long)x.raw + (long)y.raw * (long)y.raw;
        return new Fixed((int)Math.Sqrt64(r2));
    }

    public static Fixed DistSq(Vec2 a, Vec2 b) {
        Vec2 d = a - b;
        return d.LengthSq();
    }

    public static Fixed Dist(Vec2 a, Vec2 b) {
        Vec2 d = a - b;
        return d.Length();
    }

    public static Fixed Manhattan(Vec2 a, Vec2 b) {
        return Fixed.Abs(a.x - b.x) + Fixed.Abs(a.y - b.y);
    }

    public static Vec2 Lerp(Vec2 a, Vec2 b, Fixed t) {
        return a + (b - a) * t;
    }

    public Vec2 Normalized() {
        Fixed len = Length();
        if (len.raw <= 0) return Vec2.Zero();
        return new Vec2(x / len, y / len);
    }

    public static Vec2 MoveTowards(Vec2 current, Vec2 target, Fixed maxDelta) {
        Vec2 delta = target - current;
        Fixed dist = delta.Length();
        if (dist <= maxDelta || dist.raw <= 0) return target;
        return current + (delta * maxDelta) / dist;
    }

    public static Vec2 MoveTowards(Vec2 current, Vec2 target, int maxDelta) {
        return Vec2.MoveTowards(current, target, Fixed.Of(maxDelta));
    }

    public static Vec2 operator +(Vec2 a) {
        return a;
    }

    public static Vec2 operator -(Vec2 a) {
        return new Vec2(-a.x, -a.y);
    }

    public static Vec2 operator +(Vec2 a, Vec2 b) {
        return new Vec2(a.x + b.x, a.y + b.y);
    }

    public static Vec2 operator -(Vec2 a, Vec2 b) {
        return new Vec2(a.x - b.x, a.y - b.y);
    }

    public static Vec2 operator *(Vec2 v, Fixed s) {
        return new Vec2(v.x * s, v.y * s);
    }

    public static Vec2 operator *(Fixed s, Vec2 v) {
        return new Vec2(s * v.x, s * v.y);
    }

    public static Vec2 operator *(Vec2 v, int s) {
        return new Vec2(v.x * s, v.y * s);
    }

    public static Vec2 operator *(int s, Vec2 v) {
        return new Vec2(s * v.x, s * v.y);
    }

    public static Vec2 operator /(Vec2 v, Fixed s) {
        return new Vec2(v.x / s, v.y / s);
    }

    public static Vec2 operator /(Vec2 v, int s) {
        return new Vec2(v.x / s, v.y / s);
    }

    public static bool operator ==(Vec2 a, Vec2 b) {
        return a.x == b.x && a.y == b.y;
    }

    public static bool operator !=(Vec2 a, Vec2 b) {
        return a.x != b.x || a.y != b.y;
    }
}

public struct Vec3 {
    public Fixed x;
    public Fixed y;
    public Fixed z;

    public Vec3(Fixed xVal, Fixed yVal, Fixed zVal) {
        x = xVal;
        y = yVal;
        z = zVal;
    }

    public Vec3(int xInt, int yInt, int zInt) {
        x = new Fixed(xInt * 256);
        y = new Fixed(yInt * 256);
        z = new Fixed(zInt * 256);
    }

    public static Vec3 Zero() {
        return new Vec3(new Fixed(0), new Fixed(0), new Fixed(0));
    }

    public static Vec3 One() {
        return new Vec3(new Fixed(256), new Fixed(256), new Fixed(256));
    }

    public static Vec3 Of(int xInt, int yInt, int zInt) {
        return new Vec3(
            new Fixed(xInt * 256),
            new Fixed(yInt * 256),
            new Fixed(zInt * 256));
    }

    public static Vec3 Raw(int xRaw, int yRaw, int zRaw) {
        return new Vec3(
            new Fixed(xRaw),
            new Fixed(yRaw),
            new Fixed(zRaw));
    }

    public static Fixed Dot(Vec3 a, Vec3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    public static Vec3 Cross(Vec3 a, Vec3 b) {
        return new Vec3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x);
    }

    public Fixed LengthSq() {
        return x * x + y * y + z * z;
    }

    public Fixed Length() {
        long r2 =
            (long)x.raw * (long)x.raw +
            (long)y.raw * (long)y.raw +
            (long)z.raw * (long)z.raw;
        return new Fixed((int)Math.Sqrt64(r2));
    }

    public static Fixed Dist(Vec3 a, Vec3 b) {
        Vec3 d = a - b;
        return d.Length();
    }

    public static Fixed Manhattan(Vec3 a, Vec3 b) {
        return Fixed.Abs(a.x - b.x) +
            Fixed.Abs(a.y - b.y) +
            Fixed.Abs(a.z - b.z);
    }

    public static Vec3 Lerp(Vec3 a, Vec3 b, Fixed t) {
        return a + (b - a) * t;
    }

    public Vec3 Normalized() {
        Fixed len = Length();
        if (len.raw <= 0) return Vec3.Zero();
        return new Vec3(x / len, y / len, z / len);
    }

    public static Vec3 operator +(Vec3 a) {
        return a;
    }

    public static Vec3 operator -(Vec3 a) {
        return new Vec3(-a.x, -a.y, -a.z);
    }

    public static Vec3 operator +(Vec3 a, Vec3 b) {
        return new Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
    }

    public static Vec3 operator -(Vec3 a, Vec3 b) {
        return new Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
    }

    public static Vec3 operator *(Vec3 v, Fixed s) {
        return new Vec3(v.x * s, v.y * s, v.z * s);
    }

    public static Vec3 operator *(Fixed s, Vec3 v) {
        return new Vec3(s * v.x, s * v.y, s * v.z);
    }

    public static Vec3 operator *(Vec3 v, int s) {
        return new Vec3(v.x * s, v.y * s, v.z * s);
    }

    public static Vec3 operator *(int s, Vec3 v) {
        return new Vec3(s * v.x, s * v.y, s * v.z);
    }

    public static Vec3 operator /(Vec3 v, Fixed s) {
        return new Vec3(v.x / s, v.y / s, v.z / s);
    }

    public static Vec3 operator /(Vec3 v, int s) {
        return new Vec3(v.x / s, v.y / s, v.z / s);
    }

    public static bool operator ==(Vec3 a, Vec3 b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    public static bool operator !=(Vec3 a, Vec3 b) {
        return a.x != b.x || a.y != b.y || a.z != b.z;
    }
}

public struct Quat {
    public Fixed x;
    public Fixed y;
    public Fixed z;
    public Fixed w;

    public Quat(Fixed xVal, Fixed yVal, Fixed zVal, Fixed wVal) {
        x = xVal;
        y = yVal;
        z = zVal;
        w = wVal;
    }

    public Quat(int xInt, int yInt, int zInt, int wInt) {
        x = new Fixed(xInt * 256);
        y = new Fixed(yInt * 256);
        z = new Fixed(zInt * 256);
        w = new Fixed(wInt * 256);
    }

    public static Quat Identity() {
        return new Quat(new Fixed(0), new Fixed(0), new Fixed(0), new Fixed(256));
    }

    public static Quat Raw(int xRaw, int yRaw, int zRaw, int wRaw) {
        return new Quat(
            new Fixed(xRaw),
            new Fixed(yRaw),
            new Fixed(zRaw),
            new Fixed(wRaw));
    }

    public static Quat FromAxisAngle(Vec3 axis, Fixed angle) {
        Vec3 n = axis.Normalized();
        Fixed half = angle / 2;
        Fixed s = Fixed.Sin(half);
        Fixed c = Fixed.Cos(half);
        return new Quat(n.x * s, n.y * s, n.z * s, c);
    }

    public static Quat FromEuler(Fixed pitch, Fixed yaw, Fixed roll) {
        Fixed hp = pitch / 2;
        Fixed hy = yaw / 2;
        Fixed hr = roll / 2;
        Fixed sp = Fixed.Sin(hp);
        Fixed cp = Fixed.Cos(hp);
        Fixed sy = Fixed.Sin(hy);
        Fixed cy = Fixed.Cos(hy);
        Fixed sr = Fixed.Sin(hr);
        Fixed cr = Fixed.Cos(hr);
        return new Quat(
            cy * sp * cr + sy * cp * sr,
            sy * cp * cr - cy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr);
    }

    public static Fixed Dot(Quat a, Quat b) {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    public Fixed LengthSq() {
        return x * x + y * y + z * z + w * w;
    }

    public Fixed Length() {
        long r2 =
            (long)x.raw * (long)x.raw +
            (long)y.raw * (long)y.raw +
            (long)z.raw * (long)z.raw +
            (long)w.raw * (long)w.raw;
        return new Fixed((int)Math.Sqrt64(r2));
    }

    public Quat Normalized() {
        Fixed len = Length();
        if (len.raw <= 0) return Quat.Identity();
        return new Quat(x / len, y / len, z / len, w / len);
    }

    public Quat Conjugate() {
        return new Quat(-x, -y, -z, w);
    }

    public Quat Inverse() {
        Fixed lsq = LengthSq();
        if (lsq.raw <= 0) return Quat.Identity();
        Quat c = Conjugate();
        return new Quat(c.x / lsq, c.y / lsq, c.z / lsq, c.w / lsq);
    }

    public Vec3 Rotate(Vec3 v) {
        Vec3 u = new Vec3(x, y, z);
        Vec3 uv = Vec3.Cross(u, v);
        Vec3 uuv = Vec3.Cross(u, uv);
        return v + (uv * w + uuv) * 2;
    }

    public static Quat Lerp(Quat a, Quat b, Fixed t) {
        Quat target = b;
        if (Quat.Dot(a, b).raw < 0) {
            target = new Quat(-b.x, -b.y, -b.z, -b.w);
        }
        Quat res = new Quat(
            a.x + (target.x - a.x) * t,
            a.y + (target.y - a.y) * t,
            a.z + (target.z - a.z) * t,
            a.w + (target.w - a.w) * t);
        return res.Normalized();
    }

    public static Quat Slerp(Quat a, Quat b, Fixed t) {
        Quat qa = a.Normalized();
        Quat qb = b.Normalized();
        Fixed cosTheta = Quat.Dot(qa, qb);
        if (cosTheta.raw < 0) {
            qb = new Quat(-qb.x, -qb.y, -qb.z, -qb.w);
            cosTheta = -cosTheta;
        }
        if (cosTheta.raw >= 250) {
            return Quat.Lerp(qa, qb, t);
        }
        long sin2Raw = 65536 - (long)cosTheta.raw * (long)cosTheta.raw;
        Fixed sinTheta = new Fixed((int)Math.Sqrt64(sin2Raw > 0 ? sin2Raw : 0));
        if (sinTheta.raw <= 4) {
            return Quat.Lerp(qa, qb, t);
        }
        Fixed theta = Fixed.Atan2(sinTheta, cosTheta);
        Fixed s0 = Fixed.Sin((Fixed.One() - t) * theta) / sinTheta;
        Fixed s1 = Fixed.Sin(t * theta) / sinTheta;
        Quat res = new Quat(
            qa.x * s0 + qb.x * s1,
            qa.y * s0 + qb.y * s1,
            qa.z * s0 + qb.z * s1,
            qa.w * s0 + qb.w * s1);
        return res.Normalized();
    }

    public static Quat operator *(Quat a, Quat b) {
        return new Quat(
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
    }

    public static Vec3 operator *(Quat q, Vec3 v) {
        return q.Rotate(v);
    }

    public static bool operator ==(Quat a, Quat b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    public static bool operator !=(Quat a, Quat b) {
        return a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w;
    }
}

public Fixed FixedZero() { return Fixed.Zero(); }
public Fixed FixedOne() { return Fixed.One(); }
public Fixed FixedHalf() { return Fixed.Half(); }
public Fixed FixedFromRaw(int rawValue) { return Fixed.Raw(rawValue); }
public Fixed FixedFromInt(int whole) { return Fixed.Of(whole); }
public Fixed FixedFromRatio(int num, int den) { return Fixed.Ratio(num, den); }
public Fixed Pi() { return Fixed.Pi(); }
public Fixed HalfPi() { return Fixed.HalfPi(); }
public Fixed TwoPi() { return Fixed.TwoPi(); }
public Fixed Abs(Fixed v) { return Fixed.Abs(v); }
public Fixed Sin(Fixed radians) { return Fixed.Sin(radians); }
public Fixed Cos(Fixed radians) { return Fixed.Cos(radians); }
public Fixed Atan2(Fixed y, Fixed x) { return Fixed.Atan2(y, x); }
public Fixed Lerp(Fixed a, Fixed b, Fixed t) { return Fixed.Lerp(a, b, t); }
public Quat QuatIdentity() { return Quat.Identity(); }
public Quat QuatFromAxisAngle(Vec3 axis, Fixed angle) { return Quat.FromAxisAngle(axis, angle); }
public Quat QuatFromEuler(Fixed pitch, Fixed yaw, Fixed roll) { return Quat.FromEuler(pitch, yaw, roll); }
public Quat QuatNormalize(Quat q) { return q.Normalized(); }
public Vec3 QuatRotateVec3(Quat q, Vec3 v) { return q.Rotate(v); }
public Quat QuatLerp(Quat a, Quat b, Fixed t) { return Quat.Lerp(a, b, t); }
public Quat QuatSlerp(Quat a, Quat b, Fixed t) { return Quat.Slerp(a, b, t); }

public struct Fixed64 {
    public long raw;

    public Fixed64(long rawValue) {
        raw = rawValue;
    }

    public static Fixed64 Zero() {
        return new Fixed64(0);
    }

    public static Fixed64 One() {
        return new Fixed64(4294967296);
    }

    public static Fixed64 Half() {
        return new Fixed64(2147483648);
    }

    public static Fixed64 Raw(long rawValue) {
        return new Fixed64(rawValue);
    }

    public static Fixed64 Pi() {
        return new Fixed64(13493037705L);
    }

    public static Fixed64 HalfPi() {
        return new Fixed64(6746518852L);
    }

    public static Fixed64 TwoPi() {
        return new Fixed64(26986075409L);
    }

    public static Fixed64 FromRaw(long rawValue) {
        return new Fixed64(rawValue);
    }

    public static Fixed64 Of(long whole) {
        return new Fixed64(whole * 4294967296L);
    }

    public static Fixed64 FromInt(long whole) {
        return new Fixed64(whole * 4294967296L);
    }

    public static Fixed64 Ratio(long num, long den) {
        if (den == 0L) return new Fixed64(0L);
        return Fixed64.FromInt(num) / Fixed64.FromInt(den);
    }

    public static Fixed64 FromFixed(Fixed f) {
        return new Fixed64((long)f.raw * 16777216L);
    }

    public static Fixed64 FromFloat(float v) {
        return new Fixed64((long)((double)v * 4294967296.0));
    }

    public static Fixed64 FromDouble(double v) {
        return new Fixed64((long)(v * 4294967296.0));
    }

    public long ToInt() {
        return raw >= 0L ? raw / 4294967296L : (raw - 4294967295L) / 4294967296L;
    }

    public long ToLong() {
        return ToInt();
    }

    public Fixed ToFixed() {
        return new Fixed((int)(raw / 16777216L));
    }

    public float ToFloat() {
        return (float)((double)raw / 4294967296.0);
    }

    public double ToDouble() {
        return (double)raw / 4294967296.0;
    }

    public static long FloorToInt(Fixed64 v) {
        return v.ToInt();
    }

    public static long RoundToInt(Fixed64 v) {
        long half = 2147483648L;
        return v.raw >= 0L ? (v.raw + half) / 4294967296L : -((-v.raw + half) / 4294967296L);
    }

    public static Fixed64 Add(Fixed64 a, Fixed64 b) { return a + b; }
    public static Fixed64 Sub(Fixed64 a, Fixed64 b) { return a - b; }
    public static Fixed64 Mul(Fixed64 a, Fixed64 b) { return a * b; }
    public static Fixed64 Div(Fixed64 a, Fixed64 b) { return a / b; }

    public static Fixed64 Abs(Fixed64 v) {
        return new Fixed64(v.raw < 0L ? -v.raw : v.raw);
    }

    public static Fixed64 Min(Fixed64 a, Fixed64 b) {
        return a.raw <= b.raw ? a : b;
    }

    public static Fixed64 Max(Fixed64 a, Fixed64 b) {
        return a.raw >= b.raw ? a : b;
    }

    public static Fixed64 Clamp(Fixed64 v, Fixed64 lo, Fixed64 hi) {
        if (v.raw < lo.raw) return lo;
        if (v.raw > hi.raw) return hi;
        return v;
    }

    public static Fixed64 Lerp(Fixed64 a, Fixed64 b, Fixed64 t) {
        return a + (b - a) * t;
    }

    public static Fixed64 Sqrt(Fixed64 v) {
        if (v.raw <= 0L) return new Fixed64(0L);
        Fixed64 x = v.raw > 4294967296L ? new Fixed64(v.raw / 2L) : Fixed64.One();
        for (int i = 0; i < 16; i = i + 1) {
            Fixed64 next = new Fixed64((x.raw + (v / x).raw) / 2L);
            if (next.raw == x.raw) return x;
            x = next;
        }
        return x;
    }

    public static Fixed64 operator +(Fixed64 a, Fixed64 b) {
        return new Fixed64(a.raw + b.raw);
    }

    public static Fixed64 operator -(Fixed64 a, Fixed64 b) {
        return new Fixed64(a.raw - b.raw);
    }

    public static Fixed64 operator -(Fixed64 a) {
        return new Fixed64(-a.raw);
    }

    public static Fixed64 operator *(Fixed64 a, Fixed64 b) {
        long scale = 4294967296;
        long aHi = a.raw >= 0 ? a.raw / scale : (a.raw - (scale - 1)) / scale;
        long aLo = a.raw - aHi * scale;
        long bHi = b.raw >= 0 ? b.raw / scale : (b.raw - (scale - 1)) / scale;
        long bLo = b.raw - bHi * scale;
        long aLoHi = aLo / 65536;
        long aLoLo = aLo - aLoHi * 65536;
        long bLoHi = bLo / 65536;
        long bLoLo = bLo - bLoHi * 65536;
        long loCarry = (aLoHi * bLoLo + aLoLo * bLoHi + (aLoLo * bLoLo) / 65536) / 65536;
        long loProd = aLoHi * bLoHi + loCarry;
        return new Fixed64(aHi * bHi * scale + aHi * bLo + aLo * bHi + loProd);
    }

    public static Fixed64 operator /(Fixed64 a, Fixed64 b) {
        if (b.raw == 0) return new Fixed64(0);
        bool neg = (a.raw < 0) != (b.raw < 0);
        long ua = a.raw < 0 ? -a.raw : a.raw;
        long ub = b.raw < 0 ? -b.raw : b.raw;
        long qHi = ua / ub;
        long rem = ua - qHi * ub;
        long qLo = 0;
        int bit = 0;
        while (bit < 32) {
            qLo = qLo * 2;
            if (rem >= ub - rem) {
                rem = rem - (ub - rem);
                qLo = qLo + 1;
            } else {
                rem = rem * 2;
            }
            bit = bit + 1;
        }
        long q = qHi * 4294967296 + qLo;
        return new Fixed64(neg ? -q : q);
    }

    public static bool operator ==(Fixed64 a, Fixed64 b) {
        return a.raw == b.raw;
    }

    public static bool operator !=(Fixed64 a, Fixed64 b) {
        return a.raw != b.raw;
    }

    public static bool operator <(Fixed64 a, Fixed64 b) {
        return a.raw < b.raw;
    }

    public static bool operator <=(Fixed64 a, Fixed64 b) {
        return a.raw <= b.raw;
    }

    public static bool operator >(Fixed64 a, Fixed64 b) {
        return a.raw > b.raw;
    }

    public static bool operator >=(Fixed64 a, Fixed64 b) {
        return a.raw >= b.raw;
    }
}

public struct Vec2f {
    public float x;
    public float y;

    public Vec2f(float xVal, float yVal) {
        x = xVal;
        y = yVal;
    }

    public static Vec2f Create(float xVal, float yVal) { return new Vec2f(xVal, yVal); }
    public static Vec2f Zero() { return new Vec2f(0.0f, 0.0f); }
    public static Vec2f One() { return new Vec2f(1.0f, 1.0f); }
    public static Vec2f Up() { return new Vec2f(0.0f, 1.0f); }
    public static Vec2f Right() { return new Vec2f(1.0f, 0.0f); }

    public static Vec2f Add(Vec2f a, Vec2f b) { return a + b; }
    public static Vec2f Sub(Vec2f a, Vec2f b) { return a - b; }
    public static Vec2f Mul(Vec2f v, float s) { return v * s; }
    public static Vec2f Div(Vec2f v, float s) { return v / s; }
    public static float Dot(Vec2f a, Vec2f b) { return a.x * b.x + a.y * b.y; }
    public static float Cross(Vec2f a, Vec2f b) { return a.x * b.y - a.y * b.x; }
    public float LengthSq() { return x * x + y * y; }
    public float Length() { return Math.Sqrtf(LengthSq()); }
    public static float Length(Vec2f v) { return v.Length(); }

    public Vec2f Normalized() {
        float len = Length();
        if (len <= 0.000001f) return new Vec2f(0.0f, 0.0f);
        float inv = 1.0f / len;
        return new Vec2f(x * inv, y * inv);
    }

    public static Vec2f Normalized(Vec2f v) { return v.Normalized(); }

    public static Vec2f Lerp(Vec2f a, Vec2f b, float t) {
        return new Vec2f(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    }

    public static Vec2f operator +(Vec2f a, Vec2f b) { return new Vec2f(a.x + b.x, a.y + b.y); }
    public static Vec2f operator -(Vec2f a, Vec2f b) { return new Vec2f(a.x - b.x, a.y - b.y); }
    public static Vec2f operator -(Vec2f v) { return new Vec2f(-v.x, -v.y); }
    public static Vec2f operator *(Vec2f v, float s) { return new Vec2f(v.x * s, v.y * s); }
    public static Vec2f operator *(float s, Vec2f v) { return new Vec2f(v.x * s, v.y * s); }
    public static Vec2f operator /(Vec2f v, float s) { return new Vec2f(v.x / s, v.y / s); }
    public static bool operator ==(Vec2f a, Vec2f b) { return a.x == b.x && a.y == b.y; }
    public static bool operator !=(Vec2f a, Vec2f b) { return a.x != b.x || a.y != b.y; }
}

public struct Vec3f {
    public float x;
    public float y;
    public float z;

    public Vec3f(float xVal, float yVal, float zVal) {
        x = xVal;
        y = yVal;
        z = zVal;
    }

    public static Vec3f Create(float xVal, float yVal, float zVal) { return new Vec3f(xVal, yVal, zVal); }
    public static Vec3f Zero() { return new Vec3f(0.0f, 0.0f, 0.0f); }
    public static Vec3f One() { return new Vec3f(1.0f, 1.0f, 1.0f); }
    public static Vec3f Up() { return new Vec3f(0.0f, 1.0f, 0.0f); }
    public static Vec3f Forward() { return new Vec3f(0.0f, 0.0f, 1.0f); }
    public static Vec3f Right() { return new Vec3f(1.0f, 0.0f, 0.0f); }

    public static Vec3f Add(Vec3f a, Vec3f b) { return a + b; }
    public static Vec3f Sub(Vec3f a, Vec3f b) { return a - b; }
    public static Vec3f Mul(Vec3f v, float s) { return v * s; }
    public static Vec3f Div(Vec3f v, float s) { return v / s; }
    public static float Dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    public static Vec3f Cross(Vec3f a, Vec3f b) {
        return new Vec3f(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x);
    }

    public float LengthSq() { return x * x + y * y + z * z; }
    public float Length() { return Math.Sqrtf(LengthSq()); }
    public static float Length(Vec3f v) { return v.Length(); }

    public Vec3f Normalized() {
        float len = Length();
        if (len <= 0.000001f) return new Vec3f(0.0f, 0.0f, 0.0f);
        float inv = 1.0f / len;
        return new Vec3f(x * inv, y * inv, z * inv);
    }

    public static Vec3f Normalized(Vec3f v) { return v.Normalized(); }

    public static Vec3f Lerp(Vec3f a, Vec3f b, float t) {
        return new Vec3f(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t);
    }

    public static Vec3f operator +(Vec3f a, Vec3f b) { return new Vec3f(a.x + b.x, a.y + b.y, a.z + b.z); }
    public static Vec3f operator -(Vec3f a, Vec3f b) { return new Vec3f(a.x - b.x, a.y - b.y, a.z - b.z); }
    public static Vec3f operator -(Vec3f v) { return new Vec3f(-v.x, -v.y, -v.z); }
    public static Vec3f operator *(Vec3f v, float s) { return new Vec3f(v.x * s, v.y * s, v.z * s); }
    public static Vec3f operator *(float s, Vec3f v) { return new Vec3f(v.x * s, v.y * s, v.z * s); }
    public static Vec3f operator /(Vec3f v, float s) { return new Vec3f(v.x / s, v.y / s, v.z / s); }
    public static bool operator ==(Vec3f a, Vec3f b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
    public static bool operator !=(Vec3f a, Vec3f b) { return a.x != b.x || a.y != b.y || a.z != b.z; }
}

public struct Quatf {
    public float x;
    public float y;
    public float z;
    public float w;

    public Quatf(float xVal, float yVal, float zVal, float wVal) {
        x = xVal;
        y = yVal;
        z = zVal;
        w = wVal;
    }

    public static Quatf Create(float xVal, float yVal, float zVal, float wVal) {
        return new Quatf(xVal, yVal, zVal, wVal);
    }

    public static Quatf Identity() { return new Quatf(0.0f, 0.0f, 0.0f, 1.0f); }

    public static Quatf FromAxisAngle(Vec3f axis, float radians) {
        Vec3f n = axis.Normalized();
        float half = radians * 0.5f;
        float s = Math.Sinf(half);
        float c = Math.Cosf(half);
        return new Quatf(n.x * s, n.y * s, n.z * s, c);
    }

    public static Quatf FromEuler(float pitch, float yaw, float roll) {
        float hp = pitch * 0.5f;
        float hy = yaw * 0.5f;
        float hr = roll * 0.5f;
        float cp = Math.Cosf(hp);
        float sp = Math.Sinf(hp);
        float cy = Math.Cosf(hy);
        float sy = Math.Sinf(hy);
        float cr = Math.Cosf(hr);
        float sr = Math.Sinf(hr);
        return new Quatf(
            cy * sp * cr + sy * cp * sr,
            sy * cp * cr - cy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr);
    }

    public static float Dot(Quatf a, Quatf b) {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    public float LengthSq() { return x * x + y * y + z * z + w * w; }

    public Quatf Normalized() {
        float len = Math.Sqrtf(LengthSq());
        if (len <= 0.000001f) return Quatf.Identity();
        float inv = 1.0f / len;
        return new Quatf(x * inv, y * inv, z * inv, w * inv);
    }

    public static Quatf Normalized(Quatf q) { return q.Normalized(); }
    public static Quatf Mul(Quatf a, Quatf b) { return a * b; }

    public Vec3f Rotate(Vec3f v) {
        Vec3f u = new Vec3f(x, y, z);
        Vec3f uv = Vec3f.Cross(u, v);
        Vec3f uuv = Vec3f.Cross(u, uv);
        return v + (uv * w + uuv) * 2.0f;
    }

    public static Vec3f Rotate(Quatf q, Vec3f v) { return q.Rotate(v); }

    public static Quatf Lerp(Quatf a, Quatf b, float t) {
        Quatf target = b;
        if (Quatf.Dot(a, b) < 0.0f) {
            target = new Quatf(-b.x, -b.y, -b.z, -b.w);
        }
        Quatf res = new Quatf(
            a.x + (target.x - a.x) * t,
            a.y + (target.y - a.y) * t,
            a.z + (target.z - a.z) * t,
            a.w + (target.w - a.w) * t);
        return res.Normalized();
    }

    public static Quatf operator *(Quatf a, Quatf b) {
        return new Quatf(
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
    }

    public static Vec3f operator *(Quatf q, Vec3f v) { return q.Rotate(v); }
    public static bool operator ==(Quatf a, Quatf b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
    public static bool operator !=(Quatf a, Quatf b) { return a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w; }
}

public struct Vec4 {
    public Fixed x;
    public Fixed y;
    public Fixed z;
    public Fixed w;

    public Vec4(Fixed xVal, Fixed yVal, Fixed zVal, Fixed wVal) {
        x = xVal;
        y = yVal;
        z = zVal;
        w = wVal;
    }

    public static Vec4 Of(int xInt, int yInt, int zInt, int wInt) {
        return new Vec4(Fixed.Of(xInt), Fixed.Of(yInt), Fixed.Of(zInt), Fixed.Of(wInt));
    }

    public static Fixed Dot(Vec4 a, Vec4 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    public Vec3 Xyz() {
        return new Vec3(x, y, z);
    }
}

public struct Transform3D {
    public Vec3 position;
    public Quat rotation;
    public Vec3 scale;

    public Transform3D(Vec3 pos, Quat rot, Vec3 scl) {
        position = pos;
        rotation = rot;
        scale = scl;
    }

    public static Transform3D Identity() {
        return new Transform3D(Vec3.Zero(), Quat.Identity(), Vec3.One());
    }

    public Vec3 TransformPoint(Vec3 localPoint) {
        Vec3 scaled = new Vec3(
            localPoint.x * scale.x,
            localPoint.y * scale.y,
            localPoint.z * scale.z);
        return position + rotation.Rotate(scaled);
    }

    public static Transform3D Combine(Transform3D parent, Transform3D local) {
        return new Transform3D(
            parent.TransformPoint(local.position),
            (parent.rotation * local.rotation).Normalized(),
            new Vec3(
                parent.scale.x * local.scale.x,
                parent.scale.y * local.scale.y,
                parent.scale.z * local.scale.z));
    }
}

public struct AABB {
    public Vec3 min;
    public Vec3 max;

    public AABB(Vec3 minVal, Vec3 maxVal) {
        min = minVal;
        max = maxVal;
    }

    public static AABB FromCenterExtents(Vec3 center, Vec3 extents) {
        return new AABB(center - extents, center + extents);
    }

    public bool Contains(Vec3 p) {
        return p.x >= min.x && p.x <= max.x &&
               p.y >= min.y && p.y <= max.y &&
               p.z >= min.z && p.z <= max.z;
    }

    public bool Intersects(AABB other) {
        return min.x <= other.max.x && max.x >= other.min.x &&
               min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }
}

public struct Ray {
    public Vec3 origin;
    public Vec3 direction;

    public Ray(Vec3 orig, Vec3 dir) {
        origin = orig;
        direction = dir;
    }

    public Vec3 At(Fixed t) {
        return origin + direction * t;
    }
}
)RS";
    return kSource;
}

const std::string& builtinAsyncModuleSource() {
    static const std::string kSource = R"RS(
module RealScript.Async;

public class AsyncException {
    public string message;
    public AsyncException() {
        message = "";
    }
    public AsyncException(string msg) {
        message = msg;
    }
}

public class AsyncCanceledException : AsyncException {
    public AsyncCanceledException(string msg) {
        message = msg;
    }
}

public struct CancellationToken {
    public long __handle;

    public CancellationToken(long h) {
        __handle = h;
    }

    public static CancellationToken None() {
        return new CancellationToken(0L);
    }

    public static CancellationToken PreCanceled() {
        return new CancellationToken(Async.__TokenPreCanceled());
    }

    public bool IsCancellationRequested() {
        return Async.__TokenIsCanceled(__handle);
    }

    public bool IsTimedOut() {
        return Async.__TokenIsTimedOut(__handle);
    }
}

public struct CancellationTokenSource {
    public long __handle;

    public CancellationTokenSource(long h) {
        __handle = h;
    }

    public static CancellationTokenSource Create() {
        return new CancellationTokenSource(Async.__CtsCreate());
    }

    public static CancellationTokenSource CreateLinked(CancellationToken parent) {
        return new CancellationTokenSource(Async.__CtsCreateLinked(parent.__handle));
    }

    public static CancellationTokenSource CreateLinked2(
        CancellationToken parentA,
        CancellationToken parentB) {
        return new CancellationTokenSource(
            Async.__CtsCreateLinked2(parentA.__handle, parentB.__handle));
    }

    public static CancellationTokenSource CreateTimeout(double timeoutSeconds) {
        return new CancellationTokenSource(Async.__CtsCreateTimeout(timeoutSeconds));
    }

    public static CancellationTokenSource CreateLinkedTimeout(
        CancellationToken parent,
        double timeoutSeconds) {
        return new CancellationTokenSource(
            Async.__CtsCreateLinkedTimeout(parent.__handle, timeoutSeconds));
    }

    public CancellationToken Token() {
        return new CancellationToken(Async.__CtsToken(__handle));
    }

    public int Cancel() {
        return Async.__CtsCancel(__handle);
    }

    public bool IsCancellationRequested() {
        return Async.__TokenIsCanceled(Async.__CtsToken(__handle));
    }

    public bool IsTimedOut() {
        return Async.__TokenIsTimedOut(Async.__CtsToken(__handle));
    }

    public bool Release() {
        return Async.__CtsRelease(__handle);
    }
}

public struct Awaitable {
    public long __handle;

    public Awaitable(long h) {
        __handle = h;
    }

    public static Awaitable Completed() {
        return new Awaitable(Async.__CompletedVoid());
    }

    public static Awaitable FromCanceled() {
        return new Awaitable(Async.__FromCanceled());
    }

    public static Awaitable FromFaulted(string msg) {
        return new Awaitable(Async.__FromFaulted(msg));
    }

    public static Awaitable Yield() {
        return new Awaitable(Async.__Yield(0L));
    }

    public static Awaitable YieldWithToken(CancellationToken token) {
        return new Awaitable(Async.__Yield(token.__handle));
    }

    public bool IsCompleted() {
        return Async.__IsCompleted(__handle);
    }

    public bool Release() {
        return Async.__Release(__handle);
    }
}

public class Async {
    public static long __CompletedVoid() { return 0L; }
    public static long __FromCanceled() { return 0L; }
    public static long __FromFaulted(string msg) { return 0L; }
    public static long __Yield(long tokenHandle) { return 0L; }
    public static long __NewPending(long tokenHandle) { return 0L; }
    public static bool __CompleteVoid(long rawHandle) { return false; }
    public static bool __CompleteInt(long rawHandle, int value) { return false; }
    public static bool __CompleteFault(long rawHandle, string msg) { return false; }
    public static bool __CompleteCancel(long rawHandle) { return false; }
    public static bool __IsCompleted(long rawHandle) { return false; }
    public static bool __Release(long rawHandle) { return false; }
    public static long __WhenAll(Awaitable[] tasks, long tokenHandle) { return 0L; }
    public static long __WhenAnyIndex(Awaitable[] tasks, long tokenHandle, long cancelLosersCtsHandle) { return 0L; }
    public static long __CtsCreate() { return 0L; }
    public static long __CtsCreateLinked(long parentTokenHandle) { return 0L; }
    public static long __CtsCreateLinked2(long parentA, long parentB) { return 0L; }
    public static long __CtsCreateTimeout(double timeoutSeconds) { return 0L; }
    public static long __CtsCreateLinkedTimeout(long parentTokenHandle, double timeoutSeconds) { return 0L; }
    public static long __CtsToken(long ctsHandle) { return 0L; }
    public static int __CtsCancel(long ctsHandle) { return 0; }
    public static bool __CtsRelease(long ctsHandle) { return false; }
    public static long __TokenPreCanceled() { return 0L; }
    public static bool __TokenIsCanceled(long tokenHandle) { return false; }
    public static bool __TokenIsTimedOut(long tokenHandle) { return false; }
    public static int __AdvanceTimeouts(double deltaSeconds) { return 0; }
    public static int __StepUpdate(int maxCount) { return 0; }
    public static int __ReadyUpdateCount() { return 0; }

    public static Awaitable NewSignal() {
        return new Awaitable(__NewPending(0L));
    }

    public static Awaitable NewSignalWithToken(CancellationToken token) {
        return new Awaitable(__NewPending(token.__handle));
    }

    public static bool ResolveSignal(Awaitable signal) {
        return __CompleteVoid(signal.__handle);
    }

    public static bool FaultSignal(Awaitable signal, string message) {
        return __CompleteFault(signal.__handle, message);
    }

    public static bool CancelSignal(Awaitable signal) {
        return __CompleteCancel(signal.__handle);
    }

    public static Awaitable WhenAll(Awaitable[] tasks) {
        return new Awaitable(__WhenAll(tasks, 0L));
    }

    public static Awaitable WhenAllWithToken(Awaitable[] tasks, CancellationToken token) {
        return new Awaitable(__WhenAll(tasks, token.__handle));
    }

    public static Awaitable<int> WhenAny(Awaitable[] tasks) {
        return Awaitable<int>.FromHandle(__WhenAnyIndex(tasks, 0L, 0L));
    }

    public static Awaitable<int> WhenAnyRace(
        Awaitable[] tasks,
        CancellationToken token,
        CancellationTokenSource cancelLosers) {
        return Awaitable<int>.FromHandle(
            __WhenAnyIndex(tasks, token.__handle, cancelLosers.__handle));
    }

    public static Awaitable<int> NewIntSignal() {
        return Awaitable<int>.FromHandle(__NewPending(0L));
    }

    public static Awaitable<int> NewIntSignalWithToken(CancellationToken token) {
        return Awaitable<int>.FromHandle(__NewPending(token.__handle));
    }

    public static bool ResolveIntSignal(Awaitable<int> signal, int value) {
        return __CompleteInt(signal.__handle, value);
    }

    public static int StepUpdate() {
        return __StepUpdate(0);
    }

    public static int StepUpdateBounded(int maxCount) {
        return __StepUpdate(maxCount);
    }

    public static int ReadyUpdateCount() {
        return __ReadyUpdateCount();
    }

    public static int AdvanceTimeouts(double deltaSeconds) {
        return __AdvanceTimeouts(deltaSeconds);
    }
}
)RS";
    return kSource;
}

bool sourceMentionsAsyncPrimitives(std::string_view text) {
    if (text.find("async") == std::string_view::npos &&
        text.find("await") == std::string_view::npos &&
        text.find("Awaitable") == std::string_view::npos &&
        text.find("CancellationToken") == std::string_view::npos &&
        text.find("Async") == std::string_view::npos) {
        return false;
    }
    text::SourceText source(std::string(text), "$async_scan");
    diagnostics::DiagnosticBag ignored;
    syntax::Lexer lexer(source, ignored);
    for (;;) {
        const auto token = lexer.nextToken();
        if (token.kind == syntax::SyntaxKind::EndOfFileToken) {
            break;
        }
        if (token.kind == syntax::SyntaxKind::AsyncKeyword ||
            token.kind == syntax::SyntaxKind::AwaitKeyword) {
            return true;
        }
        if (token.kind == syntax::SyntaxKind::IdentifierToken) {
            if (token.text == "Awaitable" ||
                token.text == "CancellationToken" ||
                token.text == "CancellationTokenSource" ||
                token.text == "AsyncException" ||
                token.text == "AsyncCanceledException" ||
                token.text == "Async") {
                return true;
            }
        } else if (token.kind == syntax::SyntaxKind::InterpolatedStringToken) {
            const std::string& raw = token.text;
            std::size_t idx = (raw.size() >= 2 && raw[0] == '$' && raw[1] == '"') ? 2 : 0;
            const std::size_t limit = (!raw.empty() && raw.back() == '"') ? (raw.size() - 1) : raw.size();
            while (idx < limit) {
                if (raw[idx] == '\\') {
                    idx += 2;
                    continue;
                }
                if (raw[idx] == '{') {
                    if (idx + 1 < limit && raw[idx + 1] == '{') {
                        idx += 2;
                        continue;
                    }
                    ++idx;
                    const std::size_t holeStart = idx;
                    int depth = 1;
                    bool inStr = false;
                    while (idx < limit && depth > 0) {
                        const char ch = raw[idx];
                        if (inStr) {
                            if (ch == '\\') {
                                idx += 2;
                                continue;
                            }
                            if (ch == '"') inStr = false;
                        } else {
                            if (ch == '"') inStr = true;
                            else if (ch == '{') ++depth;
                            else if (ch == '}') {
                                --depth;
                                if (depth == 0) break;
                            }
                        }
                        ++idx;
                    }
                    const std::size_t holeLen = idx > holeStart ? (idx - holeStart) : 0;
                    if (holeLen > 0 &&
                        sourceMentionsAsyncPrimitives(
                            std::string_view(raw).substr(holeStart, holeLen))) {
                        return true;
                    }
                    if (idx < limit && raw[idx] == '}') ++idx;
                    continue;
                }
                ++idx;
            }
        }
    }
    return false;
}

const std::string& builtinFiberModuleSource() {
    static const std::string kSource = R"RS(
module RealScript.Fiber;

public class Fiber {
    public static void Yield(int ticks) {}
    public static void YieldRandom(int minTicks, int maxTicks) {}
    public static void WaitUntil(int channel, int key, int requiredValue) {}
    public static void SetUnbreakable(bool unbreakable) {}
    public static void Terminate() {}
    public static void Fork(string functionName) {}
    public static void ForkPc(int targetPc) {}
    public static void Command(int commandId, int arg16, int arg32) {}
    public static bool Cond(int conditionKind, int key16, int threshold16) {
        return false;
    }
    public static bool Chance(int threshold255) {
        return false;
    }
    public static int GetReg(int slot) {
        return 0;
    }
    public static void SetReg(int slot, int value) {}
}
)RS";
    return kSource;
}

const std::string& builtinResourceModuleSource() {
    static const std::string kSource = R"RS(
module RealScript.Resource;

long __RsHostResAcquire(string path, string guid);
int __RsHostResRetain(long resourceId);
int __RsHostResRelease(long resourceId);
int __RsHostResUseCount(long resourceId);
bool __RsHostResIsDisposed(long resourceId);
string __RsHostResGetPath(long resourceId);
string __RsHostResGetGuid(long resourceId);
long __RsHostResFindByPath(string path);
long __RsHostResFindByGuid(string guid);
int __RsHostResLiveCount();

public interface IDisposable {
    void Dispose();
}

public class Resource : IDisposable {
    public long __resourceId;
    public string __path;
    public string __guid;
    public int __useCount;
    public bool __disposed;
    public bool __hostBacked;

    public Resource() {
        __path = "";
        __guid = "";
        __useCount = 1;
        __disposed = false;
        __hostBacked = false;
        __resourceId = ResourceServer.__RegisterScriptResource(this, "", "");
    }

    public Resource(string path) {
        __path = path == null ? "" : path;
        __guid = "";
        __useCount = 1;
        __disposed = false;
        __hostBacked = false;
        __resourceId = ResourceServer.__RegisterScriptResource(this, __path, "");
    }

    public Resource(string path, string guid) {
        __path = path == null ? "" : path;
        __guid = guid == null ? "" : guid;
        __useCount = 1;
        __disposed = false;
        __hostBacked = false;
        __resourceId = ResourceServer.__RegisterScriptResource(this, __path, __guid);
    }

    internal Resource(long hostId, string path, string guid, bool hostBacked) {
        __resourceId = hostId;
        __path = path == null ? "" : path;
        __guid = guid == null ? "" : guid;
        __hostBacked = hostBacked;
        bool useBridge = hostBacked && ResourceServer.IsHostBridgeEnabled();
        __disposed = useBridge ? __RsHostResIsDisposed(hostId) : false;
        __useCount = useBridge ? __RsHostResUseCount(hostId) : 1;
    }

    internal void __RebindHost(long hostId, string path, string guid) {
        __resourceId = hostId;
        __path = path == null ? "" : path;
        __guid = guid == null ? "" : guid;
        __hostBacked = true;
        bool useBridge = ResourceServer.IsHostBridgeEnabled();
        __disposed = useBridge ? __RsHostResIsDisposed(hostId) : false;
        __useCount = useBridge ? __RsHostResUseCount(hostId) : 1;
    }

    public static Resource WrapHost(long hostId, string path, string guid) {
        Resource existing = ResourceServer.__FindWrapperDirect(hostId);
        if (existing != null) {
            existing.__RebindHost(hostId, path, guid);
            ResourceServer.__AttachHostWrapper(hostId, existing);
            return existing;
        }
        Resource r = new Resource(hostId, path, guid, true);
        ResourceServer.__AttachHostWrapper(hostId, r);
        return r;
    }

    public string Path {
        get { return GetPath(); }
        set { SetPath(value); }
    }

    public string Guid {
        get { return GetGuid(); }
        set { SetGuid(value); }
    }

    public long ResourceId() {
        return __resourceId;
    }

    public string GetPath() {
        if (__hostBacked && ResourceServer.IsHostBridgeEnabled()) {
            return __RsHostResGetPath(__resourceId);
        }
        return __path;
    }

    public void SetPath(string path) {
        __path = path == null ? "" : path;
        ResourceServer.__UpdatePath(__resourceId, __path);
    }

    public string GetGuid() {
        if (__hostBacked && ResourceServer.IsHostBridgeEnabled()) {
            return __RsHostResGetGuid(__resourceId);
        }
        return __guid;
    }

    public void SetGuid(string guid) {
        __guid = guid == null ? "" : guid;
        ResourceServer.__UpdateGuid(__resourceId, __guid);
    }

    public int Retain() {
        if (__hostBacked && ResourceServer.IsHostBridgeEnabled()) {
            if (__RsHostResIsDisposed(__resourceId)) {
                __disposed = true;
                __useCount = 0;
                return 0;
            }
            int nextCount = __RsHostResRetain(__resourceId);
            __disposed = false;
            __useCount = nextCount;
            return nextCount;
        }
        if (__disposed || __useCount <= 0) {
            return 0;
        }
        __useCount = __useCount + 1;
        return __useCount;
    }

    public int Release() {
        if (__hostBacked && ResourceServer.IsHostBridgeEnabled()) {
            if (__disposed || __RsHostResIsDisposed(__resourceId)) {
                __disposed = true;
                __useCount = 0;
                return 0;
            }
            int rem = __RsHostResRelease(__resourceId);
            __useCount = rem;
            if (rem <= 0) {
                __disposed = true;
                ResourceServer.__OnScriptResourceZero(__resourceId);
                OnDispose();
            }
            return rem;
        }
        if (__disposed || __useCount <= 0) {
            return 0;
        }
        __useCount = __useCount - 1;
        if (__useCount == 0) {
            __disposed = true;
            ResourceServer.__OnScriptResourceZero(__resourceId);
            OnDispose();
        }
        return __useCount;
    }

    public int UseCount() {
        if (__hostBacked && ResourceServer.IsHostBridgeEnabled()) {
            return __RsHostResUseCount(__resourceId);
        }
        if (__disposed) return 0;
        return __useCount;
    }

    public bool IsDisposed() {
        if (__hostBacked && ResourceServer.IsHostBridgeEnabled()) {
            return __RsHostResIsDisposed(__resourceId);
        }
        return __disposed;
    }

    public virtual void OnDispose() {
    }

    public virtual void Dispose() {
        Release();
    }

    public ResourceHandle AcquireHandle() {
        return ResourceHandle.FromResource(this);
    }

    public ResourceHandle AdoptHandle() {
        return ResourceHandle.Adopt(this);
    }
}

public struct ResourceHandle : IDisposable {
    public long __leaseId;
    public long __resourceId;

    public ResourceHandle(long leaseId, long resourceId) {
        __leaseId = leaseId;
        __resourceId = resourceId;
    }

    public static ResourceHandle Empty() {
        return new ResourceHandle(0L, 0L);
    }

    public static ResourceHandle __AssignHandle(ResourceHandle oldVal, ResourceHandle newVal) {
        if (oldVal.__leaseId > 0L && oldVal.__leaseId != newVal.__leaseId) {
            ResourceServer.__ReleaseLease(oldVal.__leaseId);
        }
        return newVal;
    }

    public static ResourceHandle Adopt(Resource resource) {
        if (resource == null) return new ResourceHandle(0L, 0L);
        long rid = resource.ResourceId();
        long lid = ResourceServer.__AllocObjectLease(resource, rid, false);
        return new ResourceHandle(lid, rid);
    }

    public static ResourceHandle FromResource(Resource resource) {
        if (resource == null) return new ResourceHandle(0L, 0L);
        long rid = resource.ResourceId();
        long lid = ResourceServer.__AllocObjectLease(resource, rid, true);
        return new ResourceHandle(lid, rid);
    }

    public static ResourceHandle Load(string path) {
        return ResourceServer.Load(path);
    }

    public static ResourceHandle LoadWithGuid(string path, string guid) {
        return ResourceServer.LoadWithGuid(path, guid);
    }

    public ResourceHandle Retain() {
        if (!IsValid()) return new ResourceHandle(0L, 0L);
        long lid = ResourceServer.__AllocLease(__resourceId, true);
        if (lid <= 0L) return new ResourceHandle(0L, 0L);
        return new ResourceHandle(lid, __resourceId);
    }

    public int Release() {
        if (__leaseId <= 0L || !ResourceServer.__IsLeaseActive(__leaseId)) {
            return UseCount();
        }
        return ResourceServer.__ReleaseLease(__leaseId);
    }

    public void Dispose() {
        ResourceServer.__ReleaseLease(__leaseId);
    }

    public int UseCount() {
        if (__resourceId <= 0L) return 0;
        return ResourceServer.GetUseCount(__resourceId);
    }

    public bool IsDisposed() {
        if (__resourceId <= 0L) return true;
        return ResourceServer.IsResourceDisposed(__resourceId);
    }

    public bool IsValid() {
        return __leaseId > 0L &&
            ResourceServer.__IsLeaseActive(__leaseId) &&
            !IsDisposed();
    }

    public long ResourceId() {
        return __resourceId;
    }

    public string GetPath() {
        return ResourceServer.GetPath(__resourceId);
    }

    public string GetGuid() {
        return ResourceServer.GetGuid(__resourceId);
    }

    public string Path {
        get { return GetPath(); }
    }

    public string Guid {
        get { return GetGuid(); }
    }

    public Resource Get() {
        if (!IsValid()) return null;
        return ResourceServer.GetResource(__resourceId);
    }
}

public class ResourceServer {
    static bool __initialized = false;
    static bool __hostBridge = false;
    static long __nextScriptId = 1L;

    static Resource[] __resources;
    static long[] __resourceIds;
    static bool[] __resourceActive;
    static string[] __resourcePaths;
    static string[] __resourceGuids;
    static int[] __freeResourceSlots;
    static int __freeResourceCount = 0;
    static int __resourceCount = 0;

    static IDisposable[] __leaseObjects;
    static long[] __leaseResourceIds;
    static long[] __leaseGenerations;
    static bool[] __leaseActive;
    static int[] __freeLeaseSlots;
    static int __freeLeaseCount = 0;
    static int __leaseCount = 0;
    static int __disposedCount = 0;

    static void __EnsureInit() {
        if (__initialized) return;
        __resources = new Resource[16];
        __resourceIds = new long[16];
        __resourceActive = new bool[16];
        __resourcePaths = new string[16];
        __resourceGuids = new string[16];
        __freeResourceSlots = new int[16];
        __freeResourceCount = 0;
        __resourceCount = 0;

        __leaseObjects = new IDisposable[32];
        __leaseResourceIds = new long[32];
        __leaseGenerations = new long[32];
        __leaseActive = new bool[32];
        __freeLeaseSlots = new int[32];
        __freeLeaseCount = 0;
        __leaseCount = 0;
        __initialized = true;
    }

    static void __GrowResources() {
        int oldLen = __resources.length;
        int nextLen = oldLen * 2;
        Resource[] nr = new Resource[nextLen];
        long[] ni = new long[nextLen];
        bool[] na = new bool[nextLen];
        string[] np = new string[nextLen];
        string[] ng = new string[nextLen];
        int[] nf = new int[nextLen];
        int i = 0;
        while (i < __resourceCount) {
            nr[i] = __resources[i];
            ni[i] = __resourceIds[i];
            na[i] = __resourceActive[i];
            np[i] = __resourcePaths[i];
            ng[i] = __resourceGuids[i];
            i = i + 1;
        }
        int fi = 0;
        while (fi < __freeResourceCount) {
            nf[fi] = __freeResourceSlots[fi];
            fi = fi + 1;
        }
        __resources = nr;
        __resourceIds = ni;
        __resourceActive = na;
        __resourcePaths = np;
        __resourceGuids = ng;
        __freeResourceSlots = nf;
    }

    static void __GrowLeases() {
        int oldLen = __leaseResourceIds.length;
        int nextLen = oldLen * 2;
        IDisposable[] no = new IDisposable[nextLen];
        long[] nl = new long[nextLen];
        long[] ng = new long[nextLen];
        bool[] na = new bool[nextLen];
        int[] nf = new int[nextLen];
        int i = 0;
        while (i < __leaseCount) {
            no[i] = __leaseObjects[i];
            nl[i] = __leaseResourceIds[i];
            ng[i] = __leaseGenerations[i];
            na[i] = __leaseActive[i];
            i = i + 1;
        }
        int fi = 0;
        while (fi < __freeLeaseCount) {
            nf[fi] = __freeLeaseSlots[fi];
            fi = fi + 1;
        }
        __leaseObjects = no;
        __leaseResourceIds = nl;
        __leaseGenerations = ng;
        __leaseActive = na;
        __freeLeaseSlots = nf;
    }

    static int __AllocResourceSlot() {
        __EnsureInit();
        if (__freeResourceCount > 0) {
            __freeResourceCount = __freeResourceCount - 1;
            return __freeResourceSlots[__freeResourceCount];
        }
        if (__resourceCount >= __resources.length) {
            __GrowResources();
        }
        int slot = __resourceCount;
        __resourceCount = __resourceCount + 1;
        return slot;
    }

    static int __FindIndexById(long resourceId) {
        __EnsureInit();
        if (resourceId <= 0L) return -1;
        int i = 0;
        while (i < __resourceCount) {
            if (__resourceActive[i] && __resourceIds[i] == resourceId) return i;
            i = i + 1;
        }
        return -1;
    }

    internal static Resource __FindWrapperDirect(long hostId) {
        int idx = __FindIndexById(hostId);
        if (idx >= 0) {
            return __resources[idx];
        }
        return null;
    }

    public static void EnableHostBridge(bool enabled) {
        __EnsureInit();
        __hostBridge = enabled;
    }

    public static bool IsHostBridgeEnabled() {
        return __hostBridge;
    }

    public static void ResetForTesting() {
        __initialized = false;
        __hostBridge = false;
        __resourceCount = 0;
        __freeResourceCount = 0;
        __leaseCount = 0;
        __freeLeaseCount = 0;
        __disposedCount = 0;
        __EnsureInit();
    }

    public static int __DebugResourceHighWaterMark() {
        __EnsureInit();
        return __resourceCount;
    }

    public static int __DebugLeaseHighWaterMark() {
        __EnsureInit();
        return __leaseCount;
    }

    public static long __RegisterScriptResource(Resource res, string path, string guid) {
        __EnsureInit();
        long id = 1000000L + __nextScriptId;
        __nextScriptId = __nextScriptId + 1L;
        int idx = __AllocResourceSlot();
        if (res != null) {
            res.__disposed = false;
        }
        __resources[idx] = res;
        __resourceIds[idx] = id;
        __resourceActive[idx] = true;
        __resourcePaths[idx] = path == null ? "" : path;
        __resourceGuids[idx] = guid == null ? "" : guid;
        return id;
    }

    public static void __AttachHostWrapper(long hostId, Resource wrapper) {
        __EnsureInit();
        if (wrapper == null || hostId <= 0L) return;
        bool hostDisp = __hostBridge ? __RsHostResIsDisposed(hostId) : false;
        wrapper.__disposed = hostDisp;
        int idx = __FindIndexById(hostId);
        if (idx >= 0) {
            __resources[idx] = wrapper;
            __resourceActive[idx] = !hostDisp;
            __resourcePaths[idx] = wrapper.__path;
            __resourceGuids[idx] = wrapper.__guid;
            return;
        }
        int slot = __AllocResourceSlot();
        __resources[slot] = wrapper;
        __resourceIds[slot] = hostId;
        __resourceActive[slot] = !hostDisp;
        __resourcePaths[slot] = wrapper.__path;
        __resourceGuids[slot] = wrapper.__guid;
    }

    public static void __UpdatePath(long resourceId, string path) {
        int idx = __FindIndexById(resourceId);
        if (idx >= 0) {
            __resourcePaths[idx] = path == null ? "" : path;
        }
    }

    public static void __UpdateGuid(long resourceId, string guid) {
        int idx = __FindIndexById(resourceId);
        if (idx >= 0) {
            __resourceGuids[idx] = guid == null ? "" : guid;
        }
    }

    public static void __OnScriptResourceZero(long resourceId) {
        int idx = __FindIndexById(resourceId);
        if (idx >= 0) {
            if (__resourceActive[idx]) {
                __disposedCount = __disposedCount + 1;
            }
            __resourceActive[idx] = false;
            __resources[idx] = null;
            __resourceIds[idx] = 0L;
            __resourcePaths[idx] = null;
            __resourceGuids[idx] = null;
            if (__freeResourceCount >= __freeResourceSlots.length) {
                __GrowResources();
            }
            __freeResourceSlots[__freeResourceCount] = idx;
            __freeResourceCount = __freeResourceCount + 1;
        }
    }

    static long __AllocRawLeaseWithObject(IDisposable targetObj, long resourceId) {
        __EnsureInit();
        int slot = -1;
        if (__freeLeaseCount > 0) {
            __freeLeaseCount = __freeLeaseCount - 1;
            slot = __freeLeaseSlots[__freeLeaseCount];
        } else {
            if (__leaseCount >= __leaseResourceIds.length) {
                __GrowLeases();
            }
            slot = __leaseCount;
            __leaseCount = __leaseCount + 1;
        }
        long gen = __leaseGenerations[slot];
        if (gen <= 0L) {
            gen = 1L;
            __leaseGenerations[slot] = 1L;
        }
        __leaseObjects[slot] = targetObj;
        __leaseResourceIds[slot] = resourceId;
        __leaseActive[slot] = true;
        return gen * 1000000000L + (long)(slot + 1);
    }

    public static long __AllocRawLease(long resourceId) {
        int idx = __FindIndexById(resourceId);
        IDisposable obj = (idx >= 0) ? __resources[idx] : null;
        return __AllocRawLeaseWithObject(obj, resourceId);
    }

    public static long __AllocLease(long resourceId, bool retainNow) {
        if (resourceId <= 0L) return 0L;
        int idx = __FindIndexById(resourceId);
        if (idx >= 0 && __resources[idx] != null) {
            Resource res = __resources[idx];
            if (res.IsDisposed()) return 0L;
            if (retainNow) {
                if (res.Retain() <= 0) return 0L;
            }
            return __AllocRawLeaseWithObject(res, resourceId);
        }
        if (__hostBridge) {
            if (__RsHostResIsDisposed(resourceId)) return 0L;
            if (retainNow) {
                if (__RsHostResRetain(resourceId) <= 0) return 0L;
            }
            return __AllocRawLeaseWithObject(null, resourceId);
        }
        return 0L;
    }

    public static long __AllocObjectLease(IDisposable targetObj, long resourceId, bool retainNow) {
        if (targetObj == null || resourceId <= 0L) return 0L;
        int idx = __FindIndexById(resourceId);
        if (idx >= 0 && __resources[idx] != null) {
            Resource res = __resources[idx];
            if (res.IsDisposed()) return 0L;
            if (retainNow) {
                if (res.Retain() <= 0) return 0L;
            }
            return __AllocRawLeaseWithObject(targetObj, resourceId);
        }
        if (__hostBridge) {
            if (__RsHostResIsDisposed(resourceId)) return 0L;
            if (retainNow) {
                if (__RsHostResRetain(resourceId) <= 0) return 0L;
            }
            return __AllocRawLeaseWithObject(targetObj, resourceId);
        }
        return 0L;
    }

    public static long __RetainByResourceId(long resourceId) {
        return __AllocLease(resourceId, true);
    }

    public static bool __IsLeaseActive(long leaseId) {
        __EnsureInit();
        if (leaseId <= 0L) return false;
        long gen = leaseId / 1000000000L;
        int leaseIdx = (int)(leaseId % 1000000000L) - 1;
        if (gen <= 0L || leaseIdx < 0 || leaseIdx >= __leaseCount) return false;
        return __leaseActive[leaseIdx] && __leaseGenerations[leaseIdx] == gen;
    }

    public static int __ReleaseLease(long leaseId) {
        __EnsureInit();
        if (leaseId <= 0L) return 0;
        long gen = leaseId / 1000000000L;
        int leaseIdx = (int)(leaseId % 1000000000L) - 1;
        if (gen <= 0L || leaseIdx < 0 || leaseIdx >= __leaseCount) return 0;
        if (!__leaseActive[leaseIdx] || __leaseGenerations[leaseIdx] != gen) {
            return 0;
        }
        long rid = __leaseResourceIds[leaseIdx];
        __leaseActive[leaseIdx] = false;
        __leaseObjects[leaseIdx] = null;
        __leaseResourceIds[leaseIdx] = 0L;
        __leaseGenerations[leaseIdx] = gen + 1L;
        if (__freeLeaseCount >= __freeLeaseSlots.length) {
            __GrowLeases();
        }
        __freeLeaseSlots[__freeLeaseCount] = leaseIdx;
        __freeLeaseCount = __freeLeaseCount + 1;

        int idx = __FindIndexById(rid);
        if (idx >= 0 && __resources[idx] != null) {
            return __resources[idx].Release();
        }
        if (__hostBridge) {
            return __RsHostResRelease(rid);
        }
        return 0;
    }

    public static Resource GetResource(long resourceId) {
        int idx = __FindIndexById(resourceId);
        if (idx >= 0 && __resourceActive[idx] && __resources[idx] != null) {
            return __resources[idx];
        }
        if (__hostBridge && resourceId > 0L && !__RsHostResIsDisposed(resourceId)) {
            return Resource.WrapHost(
                resourceId,
                __RsHostResGetPath(resourceId),
                __RsHostResGetGuid(resourceId));
        }
        return null;
    }

    public static int GetUseCount(long resourceId) {
        int idx = __FindIndexById(resourceId);
        if (idx >= 0 && __resources[idx] != null) {
            return __resources[idx].UseCount();
        }
        if (__hostBridge && resourceId > 0L) {
            return __RsHostResUseCount(resourceId);
        }
        return 0;
    }

    public static bool IsResourceDisposed(long resourceId) {
        int idx = __FindIndexById(resourceId);
        if (idx >= 0 && __resources[idx] != null) {
            return __resources[idx].IsDisposed();
        }
        if (__hostBridge && resourceId > 0L) {
            return __RsHostResIsDisposed(resourceId);
        }
        return true;
    }

    public static string GetPath(long resourceId) {
        int idx = __FindIndexById(resourceId);
        if (idx >= 0 && __resources[idx] != null) {
            return __resources[idx].GetPath();
        }
        if (__hostBridge && resourceId > 0L) {
            return __RsHostResGetPath(resourceId);
        }
        return "";
    }

    public static string GetGuid(long resourceId) {
        int idx = __FindIndexById(resourceId);
        if (idx >= 0 && __resources[idx] != null) {
            return __resources[idx].GetGuid();
        }
        if (__hostBridge && resourceId > 0L) {
            return __RsHostResGetGuid(resourceId);
        }
        return "";
    }

    public static Resource FindByPath(string path) {
        __EnsureInit();
        if (path == null || path == "") return null;
        int i = 0;
        while (i < __resourceCount) {
            if (__resourceActive[i] && __resources[i] != null && !__resources[i].IsDisposed()) {
                if (__resources[i].GetPath() == path) return __resources[i];
            }
            i = i + 1;
        }
        if (__hostBridge) {
            long hid = __RsHostResFindByPath(path);
            if (hid > 0L && !__RsHostResIsDisposed(hid)) {
                return GetResource(hid);
            }
        }
        return null;
    }

    public static Resource FindByGuid(string guid) {
        __EnsureInit();
        if (guid == null || guid == "") return null;
        int i = 0;
        while (i < __resourceCount) {
            if (__resourceActive[i] && __resources[i] != null && !__resources[i].IsDisposed()) {
                if (__resources[i].GetGuid() == guid) return __resources[i];
            }
            i = i + 1;
        }
        if (__hostBridge) {
            long hid = __RsHostResFindByGuid(guid);
            if (hid > 0L && !__RsHostResIsDisposed(hid)) {
                return GetResource(hid);
            }
        }
        return null;
    }

    public static ResourceHandle LoadWithGuid(string path, string guid) {
        __EnsureInit();
        string normPath = path == null ? "" : path;
        string normGuid = guid == null ? "" : guid;
        if (__hostBridge) {
            long hid = __RsHostResAcquire(normPath, normGuid);
            if (hid <= 0L) return ResourceHandle.Empty();
            Resource wrapper = Resource.WrapHost(
                hid, __RsHostResGetPath(hid), __RsHostResGetGuid(hid));
            long lid = __AllocRawLeaseWithObject(wrapper, hid);
            return new ResourceHandle(lid, hid);
        }
        if (normGuid != "") {
            Resource byGuid = FindByGuid(normGuid);
            if (byGuid != null) {
                return ResourceHandle.FromResource(byGuid);
            }
        }
        if (normPath != "") {
            Resource byPath = FindByPath(normPath);
            if (byPath != null) {
                return ResourceHandle.FromResource(byPath);
            }
        }
        Resource created = new Resource(normPath, normGuid);
        return ResourceHandle.Adopt(created);
    }

    public static ResourceHandle Load(string path) {
        return LoadWithGuid(path, "");
    }

    public static int LiveCount() {
        __EnsureInit();
        if (__hostBridge) {
            return __RsHostResLiveCount();
        }
        int live = 0;
        int i = 0;
        while (i < __resourceCount) {
            if (__resourceActive[i] && __resources[i] != null && !__resources[i].IsDisposed()) {
                live = live + 1;
            }
            i = i + 1;
        }
        return live;
    }

    public static int DisposedCount() {
        return __disposedCount;
    }
}
)RS";
    return kSource;
}

bool sourceMentionsResourcePrimitives(std::string_view text) {
    // Fast pre-filter before running the full lexer: if none of the exact
    // keyword/type tokens appear anywhere in `text`, we can skip lexing immediately.
    if (text.find("using") == std::string_view::npos &&
        text.find("IDisposable") == std::string_view::npos &&
        text.find("Resource") == std::string_view::npos) {
        return false;
    }
    text::SourceText source(std::string(text), "$resource_scan");
    diagnostics::DiagnosticBag ignored;
    syntax::Lexer lexer(source, ignored);
    for (;;) {
        const auto token = lexer.nextToken();
        if (token.kind == syntax::SyntaxKind::EndOfFileToken) {
            break;
        }
        if (token.kind == syntax::SyntaxKind::IdentifierToken) {
            if (token.text == "using" ||
                token.text == "IDisposable" ||
                token.text == "Resource" ||
                token.text == "ResourceHandle" ||
                token.text == "ResourceServer") {
                return true;
            }
        } else if (token.kind == syntax::SyntaxKind::InterpolatedStringToken) {
            // Scan only `{ ... }` expression holes inside `$"..."` interpolated strings.
            const std::string& raw = token.text;
            std::size_t idx = (raw.size() >= 2 && raw[0] == '$' && raw[1] == '"') ? 2 : 0;
            const std::size_t limit = (!raw.empty() && raw.back() == '"') ? (raw.size() - 1) : raw.size();
            while (idx < limit) {
                if (raw[idx] == '\\') {
                    idx += 2;
                    continue;
                }
                if (raw[idx] == '{') {
                    if (idx + 1 < limit && raw[idx + 1] == '{') {
                        idx += 2;
                        continue;
                    }
                    ++idx;
                    const std::size_t holeStart = idx;
                    int depth = 1;
                    bool inStr = false;
                    while (idx < limit && depth > 0) {
                        const char ch = raw[idx];
                        if (inStr) {
                            if (ch == '\\') {
                                idx += 2;
                                continue;
                            }
                            if (ch == '"') inStr = false;
                        } else {
                            if (ch == '"') inStr = true;
                            else if (ch == '{') ++depth;
                            else if (ch == '}') {
                                --depth;
                                if (depth == 0) break;
                            }
                        }
                        ++idx;
                    }
                    const std::size_t holeLen = idx > holeStart ? (idx - holeStart) : 0;
                    if (holeLen > 0 &&
                        sourceMentionsResourcePrimitives(
                            std::string_view(raw).substr(holeStart, holeLen))) {
                        return true;
                    }
                    if (idx < limit && raw[idx] == '}') ++idx;
                    continue;
                }
                ++idx;
            }
        }
    }
    return false;
}

} // namespace realscript::compiler::detail
