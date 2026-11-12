#include "realscript/compiler/ModuleBuildCache.h"

#include "realscript/Version.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace realscript::compiler {
namespace {

constexpr std::uint32_t kCacheFormatVersion = 3u;
constexpr std::uint32_t kMirOverlayVersion = 1u;
constexpr std::uint32_t kStandaloneMirVersion = 1u;
constexpr std::uint32_t kOptArtifactVersion = 1u;
constexpr std::uint32_t kGraphMetaVersion = 1u;
constexpr std::uint32_t kAotBundleVersion = 2u;

void mixBytes(std::uint64_t& state, const void* data, std::size_t length) noexcept {
    const auto* ptr = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        state ^= static_cast<std::uint64_t>(ptr[i]);
        state *= 1099511628211ull;
    }
}

void mixU8(std::uint64_t& state, std::uint8_t value) noexcept {
    mixBytes(state, &value, sizeof(value));
}

void mixU64(std::uint64_t& state, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        const auto byte = static_cast<std::uint8_t>((value >> shift) & 0xffu);
        state ^= byte;
        state *= 1099511628211ull;
    }
    // SplitMix64 finalizer step to prevent collisions across structured boundaries
    state ^= state >> 30u;
    state *= 0xbf58476d1ce4e5b9ull;
    state ^= state >> 27u;
    state *= 0x94d049bb133111ebull;
    state ^= state >> 31u;
}

void mixString(std::uint64_t& state, std::string_view text) noexcept {
    mixU64(state, static_cast<std::uint64_t>(text.size()));
    mixBytes(state, text.data(), text.size());
    mixU8(state, 0xffu);
}

std::uint64_t checksum64(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t state = 14695981039346656037ull;
    mixU64(state, static_cast<std::uint64_t>(size));
    mixBytes(state, data, size);
    mixU64(state, 0xa5a55a5af0f00f0full);
    return state;
}

std::uint64_t checksum64(const std::vector<std::uint8_t>& bytes) noexcept {
    return checksum64(bytes.data(), bytes.size());
}

std::uint64_t checksum64Text(std::string_view text) noexcept {
    return checksum64(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size());
}

std::string hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::string sanitizeFilename(std::string name) {
    if (name.empty()) name = "module";
    for (char& ch : name) {
        const bool ok =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '.' || ch == '_' || ch == '-';
        if (!ok) ch = '_';
    }
    return name;
}

std::uint64_t compilerAndAbiVersionFingerprint() noexcept {
    std::uint64_t h = 14695981039346656037ull;
    mixU64(h, kCacheFormatVersion);
    mixU64(h, kMirOverlayVersion);
    mixU64(h, kStandaloneMirVersion);
    mixU64(h, kOptArtifactVersion);
    mixU64(h, kGraphMetaVersion);
    mixU64(h, kAotBundleVersion);
    mixU64(h, realscript::kVersionMajor);
    mixU64(h, realscript::kVersionMinor);
    mixU64(h, realscript::kVersionPatch);
    mixString(h, realscript::kVersionString);
    mixU64(h, realscript::kSdkCompatibilityVersion);
    mixU64(h, realscript::kGameSdkPackageVersion);
    mixU64(h, realscript::kScriptObjectStateVersion);
    const bytecode::Version bcVer{};
    mixU64(h, bcVer.major);
    mixU64(h, bcVer.minor);
    mixU64(h, aot::RuntimeAbiMajor);
    mixU64(h, aot::RuntimeAbiMinor);
    return h;
}

void mixOptimizationOptions(
    std::uint64_t& h,
    const optimization::Options& opt) noexcept {
    mixU8(h, static_cast<std::uint8_t>(opt.level));
    mixU64(h, static_cast<std::uint64_t>(opt.maximumIterations));
    mixU8(h, opt.preserveDebugInfo ? 1u : 0u);
    mixU8(h, opt.enableInlining ? 1u : 0u);
    mixU8(h, opt.enableLicm ? 1u : 0u);
    mixU8(h, opt.enableStructFieldForwarding ? 1u : 0u);
    mixU8(h, opt.enableDevirtualization ? 1u : 0u);
    mixU64(h, static_cast<std::uint64_t>(opt.maxInlineInstructions));
}

bool isBuiltinModuleName(const std::string& name) noexcept {
    return name == "RealScript.Math" ||
        name == "RealScript.Fiber" ||
        name == "RealScript.Async" ||
        name == "RealScript.Resource";
}

bool scanInterpolatedStringHoles(
    const std::string& raw,
    const std::function<bool(std::string_view)>& holePredicate) {
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
            if (holeLen > 0 && holePredicate(std::string_view(raw).substr(holeStart, holeLen))) {
                return true;
            }
            if (idx < limit && raw[idx] == '}') ++idx;
            continue;
        }
        ++idx;
    }
    return false;
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
        if (token.kind == syntax::SyntaxKind::EndOfFileToken) break;
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
            if (scanInterpolatedStringHoles(
                    token.text,
                    [](std::string_view hole) { return sourceMentionsAsyncPrimitives(hole); })) {
                return true;
            }
        }
    }
    return false;
}

bool sourceMentionsResourcePrimitives(std::string_view text) {
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
        if (token.kind == syntax::SyntaxKind::EndOfFileToken) break;
        if (token.kind == syntax::SyntaxKind::IdentifierToken) {
            if (token.text == "using" ||
                token.text == "IDisposable" ||
                token.text == "Resource" ||
                token.text == "ResourceHandle" ||
                token.text == "ResourceServer") {
                return true;
            }
        } else if (token.kind == syntax::SyntaxKind::InterpolatedStringToken) {
            if (scanInterpolatedStringHoles(
                    token.text,
                    [](std::string_view hole) { return sourceMentionsResourcePrimitives(hole); })) {
                return true;
            }
        }
    }
    return false;
}

std::uint64_t currentProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t randomProcessSalt() noexcept {
    static const std::uint64_t s_salt = []() noexcept -> std::uint64_t {
        try {
            std::random_device rd;
            const auto hi = static_cast<std::uint64_t>(rd());
            const auto lo = static_cast<std::uint64_t>(rd());
            return (hi << 32u) ^ lo;
        } catch (...) {
            return 0x9e3779b97f4a7c15ull;
        }
    }();
    return s_salt;
}

std::filesystem::path makeUniqueTempPath(const std::filesystem::path& target) {
    static std::atomic<std::uint64_t> counter{1};
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    const auto pid = currentProcessId();
    const auto tid = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto tick = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t salt = randomProcessSalt();
    mixU64(salt, pid);
    mixU64(salt, tid);
    mixU64(salt, tick);
    mixU64(salt, seq);
    std::ostringstream suffix;
    suffix << ".tmp." << pid << "." << seq << "." << std::hex << salt;
    return std::filesystem::path(target.string() + suffix.str());
}

bool writeBinaryAtomic(
    const std::filesystem::path& target,
    const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) return false;
    }
    const auto tmpPath = makeUniqueTempPath(target);
    {
        std::ofstream stream(tmpPath, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        if (!bytes.empty()) {
            stream.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }
#if defined(_WIN32)
    if (!::MoveFileExW(
            tmpPath.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);
        return false;
    }
#else
    std::filesystem::rename(tmpPath, target, ec);
    if (ec) {
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);
        return false;
    }
#endif
    return true;
}

bool writeTextAtomic(
    const std::filesystem::path& target,
    std::string_view content) {
    std::vector<std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(content.data()),
        reinterpret_cast<const std::uint8_t*>(content.data()) + content.size());
    return writeBinaryAtomic(target, bytes);
}

bool readBinaryFile(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& out) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const auto endPos = stream.tellg();
    if (endPos < 0) return false;
    const auto size = static_cast<std::size_t>(endPos);
    out.resize(size);
    stream.seekg(0, std::ios::beg);
    if (size != 0) {
        stream.read(
            reinterpret_cast<char*>(out.data()),
            static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(stream);
}

bool readTextFile(
    const std::filesystem::path& path,
    std::string& out) {
    std::vector<std::uint8_t> bytes;
    if (!readBinaryFile(path, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

class BinaryWriter {
public:
    void u8(std::uint8_t v) { bytes_.push_back(v); }
    void u32(std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>((v >> shift) & 0xffu));
        }
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void u64(std::uint64_t v) {
        for (int shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>((v >> shift) & 0xffu));
        }
    }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void f64(double v) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        u64(bits);
    }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        bytes_.insert(
            bytes_.end(),
            reinterpret_cast<const std::uint8_t*>(s.data()),
            reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
    }
    void raw(const char* p, std::size_t n) {
        bytes_.insert(
            bytes_.end(),
            reinterpret_cast<const std::uint8_t*>(p),
            reinterpret_cast<const std::uint8_t*>(p) + n);
    }
    void bytes(const std::vector<std::uint8_t>& b) {
        u32(static_cast<std::uint32_t>(b.size()));
        bytes_.insert(bytes_.end(), b.begin(), b.end());
    }
    void span(text::TextSpan sp) {
        u64(static_cast<std::uint64_t>(sp.start));
        u64(static_cast<std::uint64_t>(sp.length));
    }
    void range(const debug::SourceRange& r) {
        u32(r.fileId);
        span(r.span);
        u32(static_cast<std::uint32_t>(r.start.line));
        u32(static_cast<std::uint32_t>(r.start.column));
        u32(static_cast<std::uint32_t>(r.end.line));
        u32(static_cast<std::uint32_t>(r.end.column));
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class BinaryReader {
public:
    explicit BinaryReader(const std::vector<std::uint8_t>& bytes, std::size_t endLimit = std::size_t(-1))
        : bytes_(bytes),
          end_(endLimit == std::size_t(-1) ? bytes.size() : std::min(endLimit, bytes.size())) {}

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool empty() const noexcept { return valid_ && pos_ == end_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return (valid_ && pos_ <= end_) ? (end_ - pos_) : 0;
    }

    std::uint8_t u8() {
        if (!need(1)) return 0;
        return bytes_[pos_++];
    }
    std::uint32_t u32() {
        std::uint32_t r = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            r |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return r;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() {
        std::uint64_t r = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            r |= static_cast<std::uint64_t>(u8()) << shift;
        }
        return r;
    }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    double f64() {
        const auto bits = u64();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::string str() {
        const auto len = u32();
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(bytes_.data() + pos_), len);
        pos_ += len;
        return s;
    }
    std::vector<std::uint8_t> byteVec() {
        const auto len = u32();
        if (!need(len)) return {};
        std::vector<std::uint8_t> out(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_),
                                      bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
        pos_ += len;
        return out;
    }
    std::string fixedBytes(std::size_t len) {
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(bytes_.data() + pos_), len);
        pos_ += len;
        return s;
    }
    text::TextSpan span() {
        text::TextSpan sp;
        sp.start = static_cast<std::size_t>(u64());
        sp.length = static_cast<std::size_t>(u64());
        return sp;
    }
    debug::SourceRange range() {
        debug::SourceRange r;
        r.fileId = u32();
        r.span = span();
        r.start.line = u32();
        r.start.column = u32();
        r.end.line = u32();
        r.end.column = u32();
        return r;
    }

private:
    bool need(std::size_t count) {
        if (!valid_ || count > end_ - pos_) {
            valid_ = false;
            return false;
        }
        return true;
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t pos_ = 0;
    std::size_t end_ = 0;
    bool valid_ = true;
};

bool validPrimitiveTypeTag(std::uint8_t tag) noexcept {
    return tag <= static_cast<std::uint8_t>(semantic::PrimitiveType::Null);
}

std::vector<std::uint8_t> encodeMirFunctionsOverlay(const mir::Module& module) {
    BinaryWriter w;
    w.raw("RSMR", 4);
    w.u32(kMirOverlayVersion);
    w.str(module.name);
    w.u32(static_cast<std::uint32_t>(module.functions.size()));
    for (const auto& fn : module.functions) {
        w.u64(fn.symbolId);
        w.str(fn.moduleName);
        w.str(fn.name);
        w.u8(static_cast<std::uint8_t>(fn.returnType));
        w.u64(fn.returnTypeId);

        w.u32(static_cast<std::uint32_t>(fn.parameterTypes.size()));
        for (const auto pt : fn.parameterTypes) w.u8(static_cast<std::uint8_t>(pt));
        w.u32(static_cast<std::uint32_t>(fn.parameterTypeIds.size()));
        for (const auto id : fn.parameterTypeIds) w.u64(id);

        w.u32(static_cast<std::uint32_t>(fn.localTypes.size()));
        for (const auto lt : fn.localTypes) w.u8(static_cast<std::uint8_t>(lt));
        w.u32(static_cast<std::uint32_t>(fn.localTypeIds.size()));
        for (const auto id : fn.localTypeIds) w.u64(id);

        w.u32(static_cast<std::uint32_t>(fn.blocks.size()));
        for (const auto& blk : fn.blocks) {
            w.u32(blk.id);
            w.u32(static_cast<std::uint32_t>(blk.parameters.size()));
            for (const auto& bp : blk.parameters) {
                w.i32(bp.value);
                w.u8(static_cast<std::uint8_t>(bp.type));
                w.u64(bp.typeId);
            }
            w.u32(static_cast<std::uint32_t>(blk.instructions.size()));
            for (const auto& inst : blk.instructions) {
                w.i32(inst.result);
                w.u8(static_cast<std::uint8_t>(inst.resultType));
                w.u64(inst.resultTypeId);
                w.u8(static_cast<std::uint8_t>(inst.opcode));
                w.u32(static_cast<std::uint32_t>(inst.operands.size()));
                for (const auto op : inst.operands) w.i32(op);
                w.u64(static_cast<std::uint64_t>(inst.localIndex));
                w.u64(inst.typeId);
                w.u64(static_cast<std::uint64_t>(inst.fieldIndex));
                w.u8(static_cast<std::uint8_t>(inst.elementType));
                w.u64(inst.elementTypeId);
                w.i64(inst.integerImmediate);
                w.f64(inst.doubleImmediate);
                w.u8(inst.boolImmediate ? 1u : 0u);
                w.u8(inst.checkedArithmetic ? 1u : 0u);
                w.str(inst.stringImmediate);
                w.u64(inst.symbolId);
                w.u8(inst.virtualDispatch ? 1u : 0u);
                w.u32(inst.virtualSlot);
                w.u8(inst.interfaceDispatch ? 1u : 0u);
                w.u64(inst.interfaceTypeId);
                w.u32(inst.interfaceSlot);
                w.str(inst.symbolName);
                w.u32(static_cast<std::uint32_t>(inst.parameterTypes.size()));
                for (const auto pt : inst.parameterTypes) w.u8(static_cast<std::uint8_t>(pt));
                w.u32(static_cast<std::uint32_t>(inst.parameterTypeIds.size()));
                for (const auto id : inst.parameterTypeIds) w.u64(id);
                w.span(inst.sourceSpan);
            }
            w.u8(static_cast<std::uint8_t>(blk.terminator.kind));
            w.i32(blk.terminator.condition);
            w.i32(blk.terminator.value);
            w.u32(blk.terminator.target);
            w.u32(blk.terminator.falseTarget);
            w.u32(static_cast<std::uint32_t>(blk.terminator.arguments.size()));
            for (const auto arg : blk.terminator.arguments) w.i32(arg);
            w.u32(static_cast<std::uint32_t>(blk.terminator.falseArguments.size()));
            for (const auto arg : blk.terminator.falseArguments) w.i32(arg);
            w.span(blk.terminator.sourceSpan);
        }

        w.u32(static_cast<std::uint32_t>(fn.exceptionHandlers.size()));
        for (const auto& eh : fn.exceptionHandlers) {
            w.u32(static_cast<std::uint32_t>(eh.protectedBlocks.size()));
            for (const auto bid : eh.protectedBlocks) w.u32(bid);
            w.u32(eh.handlerBlock);
            w.u64(eh.catchTypeId);
            w.u64(static_cast<std::uint64_t>(eh.exceptionLocal));
        }

        w.u32(fn.debugInfo.sourceFileId);
        w.str(fn.debugInfo.sourceName);
        w.range(fn.debugInfo.declaration);
        w.range(fn.debugInfo.body);
        w.u32(static_cast<std::uint32_t>(fn.debugInfo.sequencePoints.size()));
        for (const auto& sp : fn.debugInfo.sequencePoints) {
            w.u32(sp.blockId);
            w.u32(sp.instructionIndex);
            w.u8(sp.terminator ? 1u : 0u);
            w.range(sp.range);
        }
        w.u32(static_cast<std::uint32_t>(fn.debugInfo.locals.size()));
        for (const auto& loc : fn.debugInfo.locals) {
            w.str(loc.name);
            w.u32(loc.slot);
            w.u8(static_cast<std::uint8_t>(loc.type));
            w.u64(loc.typeId);
            w.u8(loc.parameter ? 1u : 0u);
            w.range(loc.declaration);
            w.range(loc.scope);
        }
    }
    return w.take();
}

bool decodeMirFunctionsOverlay(
    const std::vector<std::uint8_t>& bytes,
    const bytecode::Module& bcModule,
    mir::Module& outMir) {
    outMir = {};
    BinaryReader r(bytes);
    if (r.fixedBytes(4) != "RSMR") return false;
    if (r.u32() != kMirOverlayVersion) return false;
    const auto modName = r.str();
    if (!r.valid() || modName != bcModule.name) return false;

    outMir.name = bcModule.name;
    outMir.languageMetadata = bcModule.languageMetadata;
    outMir.sourceFiles = bcModule.sourceFiles;
    outMir.types = bcModule.types;

    const auto fnCount = r.u32();
    if (!r.valid() || fnCount > r.remaining() / 24u + 1u) return false;
    outMir.functions.reserve(fnCount);

    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        mir::Function fn;
        fn.symbolId = r.u64();
        fn.moduleName = r.str();
        fn.name = r.str();
        const auto retTag = r.u8();
        if (!validPrimitiveTypeTag(retTag)) return false;
        fn.returnType = static_cast<semantic::PrimitiveType>(retTag);
        fn.returnTypeId = r.u64();

        const auto readPrimVec = [&](std::vector<semantic::PrimitiveType>& dst) -> bool {
            const auto n = r.u32();
            if (!r.valid() || n > r.remaining()) return false;
            dst.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto t = r.u8();
                if (!validPrimitiveTypeTag(t)) return false;
                dst.push_back(static_cast<semantic::PrimitiveType>(t));
            }
            return r.valid();
        };
        const auto readIdVec = [&](std::vector<semantic::SymbolId>& dst) -> bool {
            const auto n = r.u32();
            if (!r.valid() || n > r.remaining() / 8u) return false;
            dst.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) dst.push_back(r.u64());
            return r.valid();
        };

        if (!readPrimVec(fn.parameterTypes) ||
            !readIdVec(fn.parameterTypeIds) ||
            !readPrimVec(fn.localTypes) ||
            !readIdVec(fn.localTypeIds)) {
            return false;
        }

        const auto blkCount = r.u32();
        if (!r.valid() || blkCount > r.remaining() / 16u + 1u) return false;
        fn.blocks.reserve(blkCount);
        for (std::uint32_t bi = 0; bi < blkCount; ++bi) {
            mir::BasicBlock blk;
            blk.id = r.u32();
            const auto bpCount = r.u32();
            if (!r.valid() || bpCount > r.remaining() / 13u) return false;
            blk.parameters.reserve(bpCount);
            for (std::uint32_t pi = 0; pi < bpCount; ++pi) {
                mir::BlockParameter bp;
                bp.value = r.i32();
                const auto pt = r.u8();
                if (!validPrimitiveTypeTag(pt)) return false;
                bp.type = static_cast<semantic::PrimitiveType>(pt);
                bp.typeId = r.u64();
                blk.parameters.push_back(bp);
            }

            const auto instCount = r.u32();
            if (!r.valid() || instCount > r.remaining() / 48u + 1u) return false;
            blk.instructions.reserve(instCount);
            for (std::uint32_t ii = 0; ii < instCount; ++ii) {
                mir::Instruction inst;
                inst.result = r.i32();
                const auto rt = r.u8();
                if (!validPrimitiveTypeTag(rt)) return false;
                inst.resultType = static_cast<semantic::PrimitiveType>(rt);
                inst.resultTypeId = r.u64();
                const auto op = r.u8();
                if (op > static_cast<std::uint8_t>(mir::Opcode::AsyncResolve)) return false;
                inst.opcode = static_cast<mir::Opcode>(op);
                const auto opCount = r.u32();
                if (!r.valid() || opCount > r.remaining() / 4u) return false;
                inst.operands.reserve(opCount);
                for (std::uint32_t oi = 0; oi < opCount; ++oi) inst.operands.push_back(r.i32());
                inst.localIndex = static_cast<std::size_t>(r.u64());
                inst.typeId = r.u64();
                inst.fieldIndex = static_cast<std::size_t>(r.u64());
                const auto et = r.u8();
                if (!validPrimitiveTypeTag(et)) return false;
                inst.elementType = static_cast<semantic::PrimitiveType>(et);
                inst.elementTypeId = r.u64();
                inst.integerImmediate = r.i64();
                inst.doubleImmediate = r.f64();
                const auto bImm = r.u8();
                const auto chk = r.u8();
                if (bImm > 1u || chk > 1u) return false;
                inst.boolImmediate = (bImm != 0u);
                inst.checkedArithmetic = (chk != 0u);
                inst.stringImmediate = r.str();
                inst.symbolId = r.u64();
                const auto vDisp = r.u8();
                inst.virtualSlot = r.u32();
                const auto iDisp = r.u8();
                if (vDisp > 1u || iDisp > 1u) return false;
                inst.virtualDispatch = (vDisp != 0u);
                inst.interfaceDispatch = (iDisp != 0u);
                inst.interfaceTypeId = r.u64();
                inst.interfaceSlot = r.u32();
                inst.symbolName = r.str();
                if (!readPrimVec(inst.parameterTypes) ||
                    !readIdVec(inst.parameterTypeIds)) {
                    return false;
                }
                inst.sourceSpan = r.span();
                if (!r.valid()) return false;
                blk.instructions.push_back(std::move(inst));
            }

            const auto termKind = r.u8();
            if (termKind > static_cast<std::uint8_t>(mir::TerminatorKind::Throw)) return false;
            blk.terminator.kind = static_cast<mir::TerminatorKind>(termKind);
            blk.terminator.condition = r.i32();
            blk.terminator.value = r.i32();
            blk.terminator.target = r.u32();
            blk.terminator.falseTarget = r.u32();
            const auto tArgCount = r.u32();
            if (!r.valid() || tArgCount > r.remaining() / 4u) return false;
            blk.terminator.arguments.reserve(tArgCount);
            for (std::uint32_t ai = 0; ai < tArgCount; ++ai) blk.terminator.arguments.push_back(r.i32());
            const auto fArgCount = r.u32();
            if (!r.valid() || fArgCount > r.remaining() / 4u) return false;
            blk.terminator.falseArguments.reserve(fArgCount);
            for (std::uint32_t ai = 0; ai < fArgCount; ++ai) blk.terminator.falseArguments.push_back(r.i32());
            blk.terminator.sourceSpan = r.span();
            if (!r.valid()) return false;
            fn.blocks.push_back(std::move(blk));
        }

        const auto ehCount = r.u32();
        if (!r.valid() || ehCount > r.remaining() / 20u + 1u) return false;
        fn.exceptionHandlers.reserve(ehCount);
        for (std::uint32_t ei = 0; ei < ehCount; ++ei) {
            mir::ExceptionHandler eh;
            const auto pbCount = r.u32();
            if (!r.valid() || pbCount > r.remaining() / 4u) return false;
            eh.protectedBlocks.reserve(pbCount);
            for (std::uint32_t pi = 0; pi < pbCount; ++pi) eh.protectedBlocks.push_back(r.u32());
            eh.handlerBlock = r.u32();
            eh.catchTypeId = r.u64();
            eh.exceptionLocal = static_cast<std::size_t>(r.u64());
            fn.exceptionHandlers.push_back(std::move(eh));
        }

        fn.debugInfo.sourceFileId = r.u32();
        fn.debugInfo.sourceName = r.str();
        fn.debugInfo.declaration = r.range();
        fn.debugInfo.body = r.range();
        const auto spCount = r.u32();
        if (!r.valid() || spCount > r.remaining() / 40u + 1u) return false;
        fn.debugInfo.sequencePoints.reserve(spCount);
        for (std::uint32_t si = 0; si < spCount; ++si) {
            debug::SequencePoint sp;
            sp.blockId = r.u32();
            sp.instructionIndex = r.u32();
            const auto termFlag = r.u8();
            if (termFlag > 1u) return false;
            sp.terminator = (termFlag != 0u);
            sp.range = r.range();
            fn.debugInfo.sequencePoints.push_back(sp);
        }
        const auto locCount = r.u32();
        if (!r.valid() || locCount > r.remaining() / 60u + 1u) return false;
        fn.debugInfo.locals.reserve(locCount);
        for (std::uint32_t li = 0; li < locCount; ++li) {
            debug::LocalVariableInfo loc;
            loc.name = r.str();
            loc.slot = r.u32();
            const auto lt = r.u8();
            if (!validPrimitiveTypeTag(lt)) return false;
            loc.type = static_cast<semantic::PrimitiveType>(lt);
            loc.typeId = r.u64();
            const auto pFlag = r.u8();
            if (pFlag > 1u) return false;
            loc.parameter = (pFlag != 0u);
            loc.declaration = r.range();
            loc.scope = r.range();
            fn.debugInfo.locals.push_back(std::move(loc));
        }
        if (!r.valid()) return false;
        outMir.functions.push_back(std::move(fn));
    }

    if (!r.empty()) return false;
    diagnostics::DiagnosticBag verifyDiag;
    return mir::verifyModule(outMir, verifyDiag) && !verifyDiag.hasErrors();
}

constexpr const char* kEmptyModuleFunctionSentinel = "__rs_cache_empty_module_sentinel__";

void addDummySentinelFunction(bytecode::Module& mod) {
    bytecode::Function fn;
    fn.symbolId = 0xfeedfacecafebeefULL;
    fn.name = kEmptyModuleFunctionSentinel;
    fn.returnType = semantic::PrimitiveType::Void;
    fn.returnTypeId = 0;
    bytecode::BasicBlock bb;
    bb.id = 0;
    bb.terminator.kind = bytecode::TerminatorKind::ReturnVoid;
    fn.blocks.push_back(std::move(bb));
    if (!mod.sourceFiles.empty()) {
        fn.debugInfo.sourceFileId = mod.sourceFiles.front().id;
    }
    mod.functions.push_back(std::move(fn));
}

std::vector<std::uint8_t> encodeCacheBytecodeModule(const bytecode::Module& mod) {
    if (!mod.functions.empty()) {
        return bytecode::encodeModule(mod);
    }
    bytecode::Module copy = mod;
    copy.version.minor = 9;
    addDummySentinelFunction(copy);
    return bytecode::encodeModule(copy);
}

bool decodeCacheBytecodeModule(
    const std::vector<std::uint8_t>& bytes,
    bytecode::Module& outMod,
    diagnostics::DiagnosticBag& diag) {
    if (!bytecode::decodeModule(bytes, outMod, diag) || diag.hasErrors()) {
        return false;
    }
    if (outMod.functions.size() == 1 &&
        outMod.functions[0].name == kEmptyModuleFunctionSentinel &&
        outMod.functions[0].symbolId == 0xfeedfacecafebeefULL) {
        outMod.functions.clear();
    }
    return bytecode::verifyModule(outMod, diag) && !diag.hasErrors();
}

bytecode::Module makeMirHeaderShell(const mir::Module& mod) {
    bytecode::Module shell;
    shell.version.minor = 9;
    shell.name = mod.name;
    shell.languageMetadata = mod.languageMetadata;
    shell.sourceFiles = mod.sourceFiles;
    shell.types = mod.types;
    addDummySentinelFunction(shell);
    return shell;
}

std::vector<std::uint8_t> encodeStandaloneMirModule(const mir::Module& mod) {
    const auto headerBytes = bytecode::encodeModule(makeMirHeaderShell(mod));
    const auto fnOverlayBytes = encodeMirFunctionsOverlay(mod);
    BinaryWriter w;
    w.raw("RSUM", 4);
    w.u32(kStandaloneMirVersion);
    w.bytes(headerBytes);
    w.bytes(fnOverlayBytes);
    const auto crc = checksum64(w.data());
    w.u64(crc);
    return w.take();
}

bool decodeStandaloneMirModule(
    const std::vector<std::uint8_t>& bytes,
    mir::Module& outMir) {
    if (bytes.size() < 24u) return false;
    const std::size_t payloadLen = bytes.size() - 8u;
    BinaryReader r(bytes, payloadLen);
    if (r.fixedBytes(4) != "RSUM") return false;
    if (r.u32() != kStandaloneMirVersion) return false;
    const auto headerBytes = r.byteVec();
    const auto fnOverlayBytes = r.byteVec();
    if (!r.empty()) return false;

    BinaryReader tail(bytes);
    (void)tail.fixedBytes(payloadLen);
    if (tail.u64() != checksum64(bytes.data(), payloadLen) || !tail.empty()) {
        return false;
    }

    bytecode::Module shell;
    diagnostics::DiagnosticBag diag;
    if (!bytecode::decodeModule(headerBytes, shell, diag) || diag.hasErrors()) {
        return false;
    }
    return decodeMirFunctionsOverlay(fnOverlayBytes, shell, outMir);
}

std::uint64_t hashMirModuleContent(const mir::Module& mod) {
    const auto headerBytes = bytecode::encodeModule(makeMirHeaderShell(mod));
    const auto fnBytes = encodeMirFunctionsOverlay(mod);
    std::uint64_t h = 14695981039346656037ull;
    mixU64(h, compilerAndAbiVersionFingerprint());
    mixString(h, mod.name);
    mixU64(h, checksum64(headerBytes));
    mixU64(h, checksum64(fnBytes));
    return h;
}

struct ModuleMetaRecord {
    std::string moduleName;
    std::uint64_t cacheKey = 0;
    std::uint64_t implementationFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyPublicFingerprint = 0;
    std::uint64_t optMirHash = 0;
    std::uint64_t rsbcSize = 0;
    std::uint64_t rsbcChecksum = 0;
    std::uint64_t rsmirSize = 0;
    std::uint64_t rsmirChecksum = 0;
    std::uint64_t unoptSize = 0;
    std::uint64_t unoptChecksum = 0;
    std::vector<std::string> usedBuiltins;
};

std::vector<std::uint8_t> encodeModuleMeta(const ModuleMetaRecord& meta) {
    BinaryWriter w;
    w.raw("RSCM", 4);
    w.u32(kCacheFormatVersion);
    w.str(meta.moduleName);
    w.u64(meta.cacheKey);
    w.u64(meta.implementationFingerprint);
    w.u64(meta.publicFingerprint);
    w.u64(meta.dependencyPublicFingerprint);
    w.u64(meta.optMirHash);
    w.u64(meta.rsbcSize);
    w.u64(meta.rsbcChecksum);
    w.u64(meta.rsmirSize);
    w.u64(meta.rsmirChecksum);
    w.u64(meta.unoptSize);
    w.u64(meta.unoptChecksum);
    w.u32(static_cast<std::uint32_t>(meta.usedBuiltins.size()));
    for (const auto& b : meta.usedBuiltins) {
        w.str(b);
    }
    const auto payloadChecksum = checksum64(w.data());
    w.u64(payloadChecksum);
    return w.take();
}

bool decodeModuleMeta(const std::vector<std::uint8_t>& bytes, ModuleMetaRecord& out) {
    if (bytes.size() < 104u) return false;
    const std::size_t payloadLen = bytes.size() - 8u;
    BinaryReader r(bytes, payloadLen);
    if (r.fixedBytes(4) != "RSCM") return false;
    if (r.u32() != kCacheFormatVersion) return false;
    out.moduleName = r.str();
    out.cacheKey = r.u64();
    out.implementationFingerprint = r.u64();
    out.publicFingerprint = r.u64();
    out.dependencyPublicFingerprint = r.u64();
    out.optMirHash = r.u64();
    out.rsbcSize = r.u64();
    out.rsbcChecksum = r.u64();
    out.rsmirSize = r.u64();
    out.rsmirChecksum = r.u64();
    out.unoptSize = r.u64();
    out.unoptChecksum = r.u64();
    const auto bCount = r.u32();
    if (!r.valid() || bCount > r.remaining() / 4u + 1u) return false;
    out.usedBuiltins.clear();
    out.usedBuiltins.reserve(bCount);
    for (std::uint32_t i = 0; i < bCount; ++i) {
        auto b = r.str();
        if (!r.valid()) return false;
        out.usedBuiltins.push_back(std::move(b));
    }
    if (!r.empty()) return false;

    BinaryReader tail(bytes);
    (void)tail.fixedBytes(payloadLen);
    const auto storedChecksum = tail.u64();
    if (!tail.empty()) return false;
    return storedChecksum == checksum64(bytes.data(), payloadLen);
}

struct OptArtifactMeta {
    std::string moduleName;
    std::uint64_t optMirHash = 0;
    std::uint64_t rsbcSize = 0;
    std::uint64_t rsbcChecksum = 0;
    std::uint64_t rsmirSize = 0;
    std::uint64_t rsmirChecksum = 0;
};

std::vector<std::uint8_t> encodeOptArtifactMeta(const OptArtifactMeta& meta) {
    BinaryWriter w;
    w.raw("RSOM", 4);
    w.u32(kOptArtifactVersion);
    w.str(meta.moduleName);
    w.u64(meta.optMirHash);
    w.u64(meta.rsbcSize);
    w.u64(meta.rsbcChecksum);
    w.u64(meta.rsmirSize);
    w.u64(meta.rsmirChecksum);
    const auto crc = checksum64(w.data());
    w.u64(crc);
    return w.take();
}

bool decodeOptArtifactMeta(const std::vector<std::uint8_t>& bytes, OptArtifactMeta& out) {
    if (bytes.size() < 56u) return false;
    const std::size_t payloadLen = bytes.size() - 8u;
    BinaryReader r(bytes, payloadLen);
    if (r.fixedBytes(4) != "RSOM") return false;
    if (r.u32() != kOptArtifactVersion) return false;
    out.moduleName = r.str();
    out.optMirHash = r.u64();
    out.rsbcSize = r.u64();
    out.rsbcChecksum = r.u64();
    out.rsmirSize = r.u64();
    out.rsmirChecksum = r.u64();
    if (!r.empty()) return false;

    BinaryReader tail(bytes);
    (void)tail.fixedBytes(payloadLen);
    return tail.u64() == checksum64(bytes.data(), payloadLen) && tail.empty();
}

struct GraphMetaRecord {
    std::uint64_t graphKey = 0;
    std::vector<std::pair<std::string, std::uint64_t>> moduleOptHashes;
};

std::vector<std::uint8_t> encodeGraphMeta(const GraphMetaRecord& rec) {
    BinaryWriter w;
    w.raw("RSGM", 4);
    w.u32(kGraphMetaVersion);
    w.u64(rec.graphKey);
    w.u32(static_cast<std::uint32_t>(rec.moduleOptHashes.size()));
    for (const auto& [name, hash] : rec.moduleOptHashes) {
        w.str(name);
        w.u64(hash);
    }
    const auto crc = checksum64(w.data());
    w.u64(crc);
    return w.take();
}

bool decodeGraphMeta(const std::vector<std::uint8_t>& bytes, GraphMetaRecord& out) {
    if (bytes.size() < 28u) return false;
    const std::size_t payloadLen = bytes.size() - 8u;
    BinaryReader r(bytes, payloadLen);
    if (r.fixedBytes(4) != "RSGM") return false;
    if (r.u32() != kGraphMetaVersion) return false;
    out.graphKey = r.u64();
    const auto count = r.u32();
    if (!r.valid() || count > r.remaining() / 12u + 1u) return false;
    out.moduleOptHashes.clear();
    out.moduleOptHashes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto name = r.str();
        const auto h = r.u64();
        if (!r.valid()) return false;
        out.moduleOptHashes.emplace_back(std::move(name), h);
    }
    if (!r.empty()) return false;

    BinaryReader tail(bytes);
    (void)tail.fixedBytes(payloadLen);
    return tail.u64() == checksum64(bytes.data(), payloadLen) && tail.empty();
}

struct AotCacheBundle {
    std::uint64_t aotKey = 0;
    std::uint64_t contentHash = 0;
    std::uint64_t sharedGenericBodyCount = 0;
    std::uint64_t foldedGenericFunctionCount = 0;
    std::string header;
    std::string source;
    std::string manifest;
};

std::vector<std::uint8_t> encodeAotBundle(const AotCacheBundle& bundle) {
    BinaryWriter w;
    w.raw("RSAO", 4);
    w.u32(kAotBundleVersion);
    w.u64(bundle.aotKey);
    w.u64(bundle.contentHash);
    w.u64(bundle.sharedGenericBodyCount);
    w.u64(bundle.foldedGenericFunctionCount);
    w.u64(checksum64Text(bundle.header));
    w.u64(checksum64Text(bundle.source));
    w.u64(checksum64Text(bundle.manifest));
    w.str(bundle.header);
    w.str(bundle.source);
    w.str(bundle.manifest);
    const auto payloadChecksum = checksum64(w.data());
    w.u64(payloadChecksum);
    return w.take();
}

bool decodeAotBundle(const std::vector<std::uint8_t>& bytes, AotCacheBundle& out) {
    if (bytes.size() < 80u) return false;
    const std::size_t payloadLen = bytes.size() - 8u;
    BinaryReader r(bytes, payloadLen);
    if (r.fixedBytes(4) != "RSAO") return false;
    if (r.u32() != kAotBundleVersion) return false;
    out.aotKey = r.u64();
    out.contentHash = r.u64();
    out.sharedGenericBodyCount = r.u64();
    out.foldedGenericFunctionCount = r.u64();
    const auto hdrCrc = r.u64();
    const auto srcCrc = r.u64();
    const auto manCrc = r.u64();
    out.header = r.str();
    out.source = r.str();
    out.manifest = r.str();
    if (!r.empty()) return false;

    BinaryReader tail(bytes);
    (void)tail.fixedBytes(payloadLen);
    const auto storedChecksum = tail.u64();
    if (!tail.empty()) return false;
    if (storedChecksum != checksum64(bytes.data(), payloadLen)) return false;
    return checksum64Text(out.header) == hdrCrc &&
        checksum64Text(out.source) == srcCrc &&
        checksum64Text(out.manifest) == manCrc;
}

// ============================================================================
// AST / Semantic Public Interface Extraction & .d.rscript Stub Generation
// ============================================================================

struct ParsedSourceUnit {
    SourceFile rawSource;
    std::shared_ptr<text::SourceText> sourceText;
    std::shared_ptr<syntax::CompilationUnitSyntax> ast;
    std::vector<syntax::SyntaxToken> tokens;
    std::string moduleName;
    std::vector<std::string> imports;
    bool deterministic = false;
};

std::string canonicalizeTypeSyntax(
    const syntax::TypeSyntax& typeSyn,
    const semantic::TypeSymbolMap& visibleTypes) {
    if (typeSyn.isNullable() && typeSyn.typeArguments.size() == 1) {
        return canonicalizeTypeSyntax(typeSyn.typeArguments[0], visibleTypes) + "?";
    }
    std::string base = typeSyn.name.text;
    const auto prim = semantic::resolvePrimitiveType(base);
    if (prim != semantic::PrimitiveType::Error) {
        base = semantic::primitiveTypeName(prim);
    } else {
        const auto it = visibleTypes.find(base);
        if (it != visibleTypes.end()) {
            base = semantic::canonicalTypeName(it->second);
        }
    }
    if (typeSyn.isGeneric()) {
        base.push_back('<');
        for (std::size_t i = 0; i < typeSyn.typeArguments.size(); ++i) {
            if (i != 0) base.push_back(',');
            base += canonicalizeTypeSyntax(typeSyn.typeArguments[i], visibleTypes);
        }
        base.push_back('>');
    }
    if (typeSyn.isArray()) {
        base += "[]";
    }
    return base;
}

std::string renderTypeSyntaxForStub(const syntax::TypeSyntax& typeSyn) {
    if (typeSyn.isNullable() && typeSyn.typeArguments.size() == 1) {
        return renderTypeSyntaxForStub(typeSyn.typeArguments[0]) + "?";
    }
    std::string out = typeSyn.name.text;
    if (typeSyn.isGeneric()) {
        out.push_back('<');
        for (std::size_t i = 0; i < typeSyn.typeArguments.size(); ++i) {
            if (i != 0) out += ", ";
            out += renderTypeSyntaxForStub(typeSyn.typeArguments[i]);
        }
        out.push_back('>');
    }
    if (typeSyn.isArray()) {
        out += "[]";
    }
    return out;
}

void collectReferencedTypeNames(
    const syntax::TypeSyntax& typeSyn,
    std::unordered_set<std::string>& outNames) {
    if (semantic::resolvePrimitiveType(typeSyn.name.text) == semantic::PrimitiveType::Error &&
        !typeSyn.name.text.empty()) {
        outNames.insert(typeSyn.name.text);
    }
    for (const auto& arg : typeSyn.typeArguments) {
        collectReferencedTypeNames(arg, outNames);
    }
}

std::string formatAttributesCanonical(
    const std::vector<syntax::AttributeListSyntax>& lists,
    const text::SourceText& source) {
    std::vector<std::string> items;
    for (const auto& list : lists) {
        for (const auto& attr : list.attributes) {
            std::ostringstream s;
            s << attr.nameToken.text << '(';
            std::size_t pos = 0;
            for (const auto& arg : attr.arguments) {
                if (arg.nameToken) s << arg.nameToken->text << '=';
                else s << '$' << pos++ << '=';
                const auto sp = arg.valueSpan();
                if (!sp.empty()) s << source.view(sp);
                s << ';';
            }
            s << ')';
            items.push_back(s.str());
        }
    }
    std::sort(items.begin(), items.end());
    std::ostringstream joined;
    for (const auto& item : items) joined << '[' << item << ']';
    return joined.str();
}

std::string renderAttributesForStub(
    const std::vector<syntax::AttributeListSyntax>& lists,
    const text::SourceText& source,
    std::string_view indent) {
    std::ostringstream out;
    for (const auto& list : lists) {
        if (list.attributes.empty()) continue;
        out << indent << '[';
        for (std::size_t i = 0; i < list.attributes.size(); ++i) {
            if (i != 0) out << ", ";
            const auto& attr = list.attributes[i];
            out << attr.nameToken.text;
            if (attr.openParenToken) {
                out << '(';
                for (std::size_t j = 0; j < attr.arguments.size(); ++j) {
                    if (j != 0) out << ", ";
                    const auto& arg = attr.arguments[j];
                    if (arg.nameToken) out << arg.nameToken->text << " = ";
                    const auto sp = arg.valueSpan();
                    if (!sp.empty()) out << source.view(sp);
                }
                out << ')';
            }
        }
        out << "]\n";
    }
    return out.str();
}

std::string normalizedSpanTokens(
    const std::vector<syntax::SyntaxToken>& tokens,
    text::TextSpan span) {
    std::ostringstream out;
    const auto end = span.end();
    for (const auto& tok : tokens) {
        if (tok.kind == syntax::SyntaxKind::EndOfFileToken) break;
        if (tok.span.start < span.start) continue;
        if (tok.span.start >= end) break;
        out << syntax::syntaxKindName(tok.kind) << ':' << tok.text << ' ';
    }
    return out.str();
}

std::string escapeRsStringLiteral(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (const char ch : s) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << ch; break;
        }
    }
    out << '"';
    return out.str();
}

std::string renderConstantTokenValue(
    const syntax::TokenValue& val,
    semantic::PrimitiveType fieldType,
    const std::string& fieldTypeName,
    const semantic::TypeSymbolMap& visibleTypes) {
    if (fieldType == semantic::PrimitiveType::Enum) {
        if (const auto* iv = std::get_if<std::int64_t>(&val)) {
            const auto it = visibleTypes.find(fieldTypeName);
            if (it != visibleTypes.end()) {
                for (const auto& em : it->second.enumMembers) {
                    if (em.value == *iv) {
                        return it->second.name + "." + em.name;
                    }
                }
                return "(" + it->second.name + ")" + std::to_string(*iv);
            }
        }
    }
    if (const auto* b = std::get_if<bool>(&val)) {
        return *b ? "true" : "false";
    }
    if (const auto* iv = std::get_if<std::int64_t>(&val)) {
        if (fieldType == semantic::PrimitiveType::Long) return std::to_string(*iv) + "L";
        if (fieldType == semantic::PrimitiveType::Int) return std::to_string(*iv);
        return "(" + std::string(semantic::primitiveTypeName(fieldType)) + ")" + std::to_string(*iv);
    }
    if (const auto* dv = std::get_if<double>(&val)) {
        std::ostringstream s;
        s << std::setprecision(17) << *dv;
        auto txt = s.str();
        if (txt.find('.') == std::string::npos && txt.find('e') == std::string::npos &&
            txt.find('E') == std::string::npos) {
            txt += ".0";
        }
        if (fieldType == semantic::PrimitiveType::Float) {
            return "(float)" + txt;
        }
        return txt;
    }
    if (const auto* sv = std::get_if<std::string>(&val)) {
        return escapeRsStringLiteral(*sv);
    }
    return "null";
}

std::string canonicalTokenValueSignature(const syntax::TokenValue& val) {
    if (const auto* b = std::get_if<bool>(&val)) return *b ? "bool:1" : "bool:0";
    if (const auto* iv = std::get_if<std::int64_t>(&val)) return "i64:" + std::to_string(*iv);
    if (const auto* dv = std::get_if<double>(&val)) {
        std::ostringstream s;
        s << "f64:" << std::setprecision(17) << *dv;
        return s.str();
    }
    if (const auto* sv = std::get_if<std::string>(&val)) return "str:" + *sv;
    return "null";
}

std::string formatParameterSignature(
    const syntax::ParameterSyntax& param,
    const text::SourceText& source,
    const semantic::TypeSymbol* enclosingType,
    const semantic::TypeSymbolMap& visibleTypes) {
    std::ostringstream out;
    if (!param.attributes.empty()) {
        out << formatAttributesCanonical(param.attributes, source) << ' ';
    }
    if (param.paramsKeyword) out << "params ";
    if (param.thisKeyword) out << "this ";
    if (param.modifierToken) out << param.modifierToken->text << ' ';
    out << canonicalizeTypeSyntax(param.type, visibleTypes) << ' ' << param.identifierToken.text;
    if (param.defaultValue) {
        syntax::TokenValue cVal;
        semantic::PrimitiveType cType = semantic::PrimitiveType::Error;
        if (semantic::evaluateConstantSyntaxExpression(
                *param.defaultValue, enclosingType, visibleTypes, cVal, cType)) {
            out << "=const:" << canonicalTokenValueSignature(cVal);
        } else {
            out << '=' << source.view(param.defaultValue->span());
        }
    }
    return out.str();
}

std::string renderParameterForStub(
    const syntax::ParameterSyntax& param,
    const text::SourceText& source,
    const semantic::TypeSymbol* enclosingType,
    const semantic::TypeSymbolMap& visibleTypes) {
    std::ostringstream out;
    for (const auto& list : param.attributes) {
        if (list.attributes.empty()) continue;
        out << '[';
        for (std::size_t i = 0; i < list.attributes.size(); ++i) {
            if (i != 0) out << ", ";
            const auto& attr = list.attributes[i];
            out << attr.nameToken.text;
            if (attr.openParenToken) {
                out << '(';
                for (std::size_t j = 0; j < attr.arguments.size(); ++j) {
                    if (j != 0) out << ", ";
                    const auto& arg = attr.arguments[j];
                    if (arg.nameToken) out << arg.nameToken->text << " = ";
                    const auto sp = arg.valueSpan();
                    if (!sp.empty()) out << source.view(sp);
                }
                out << ')';
            }
        }
        out << "] ";
    }
    if (param.paramsKeyword) out << "params ";
    if (param.thisKeyword) out << "this ";
    if (param.modifierToken) out << param.modifierToken->text << ' ';
    out << renderTypeSyntaxForStub(param.type) << ' ' << param.identifierToken.text;
    if (param.defaultValue) {
        syntax::TokenValue cVal;
        semantic::PrimitiveType cType = semantic::PrimitiveType::Error;
        if (param.defaultValue->kind() != syntax::SyntaxKind::LiteralExpression &&
            semantic::evaluateConstantSyntaxExpression(
                *param.defaultValue, enclosingType, visibleTypes, cVal, cType)) {
            const syntax::TypeSyntax& valTypeSyn =
                (param.type.isNullable() && param.type.typeArguments.size() == 1)
                    ? param.type.typeArguments[0]
                    : param.type;
            semantic::PrimitiveType targetPrim = cType;
            std::string targetName;
            if (!valTypeSyn.isArray()) {
                const auto pPrim = semantic::resolvePrimitiveType(valTypeSyn.name.text);
                if (pPrim != semantic::PrimitiveType::Error) {
                    targetPrim = pPrim;
                } else {
                    const auto resolved = visibleTypes.find(valTypeSyn.name.text);
                    if (resolved != visibleTypes.end() &&
                        resolved->second.kind == semantic::TypeKind::Enum) {
                        targetPrim = semantic::PrimitiveType::Enum;
                        targetName = semantic::canonicalTypeName(resolved->second);
                    }
                }
            }
            out << " = " << renderConstantTokenValue(cVal, targetPrim, targetName, visibleTypes);
        } else {
            out << " = " << source.view(param.defaultValue->span());
        }
    }
    return out.str();
}

std::string dummyArgumentForTypeSyntax(
    const syntax::TypeSyntax& typeSyn,
    const semantic::TypeSymbolMap& visibleTypes) {
    if (typeSyn.isArray() || typeSyn.isNullable()) return "null";
    const auto& name = typeSyn.name.text;
    const auto prim = semantic::resolvePrimitiveType(name);
    switch (prim) {
    case semantic::PrimitiveType::Bool: return "false";
    case semantic::PrimitiveType::Int: return "0";
    case semantic::PrimitiveType::Long: return "0L";
    case semantic::PrimitiveType::Byte:
    case semantic::PrimitiveType::SByte:
    case semantic::PrimitiveType::Short:
    case semantic::PrimitiveType::UShort:
    case semantic::PrimitiveType::UInt:
    case semantic::PrimitiveType::ULong:
        return "(" + std::string(semantic::primitiveTypeName(prim)) + ")0";
    case semantic::PrimitiveType::Float: return "(float)0.0";
    case semantic::PrimitiveType::Double: return "0.0";
    case semantic::PrimitiveType::Char: return "(char)0";
    case semantic::PrimitiveType::String: return "\"\"";
    default: break;
    }
    const auto it = visibleTypes.find(name);
    if (it != visibleTypes.end()) {
        if (it->second.kind == semantic::TypeKind::Struct) {
            return "new " + name + "()";
        }
        if (it->second.kind == semantic::TypeKind::Enum) {
            if (!it->second.enumMembers.empty()) {
                return name + "." + it->second.enumMembers.front().name;
            }
            return "(" + name + ")0";
        }
    }
    return "null";
}

std::string dummyFunctionBodyForReturnType(
    const syntax::TypeSyntax& retType,
    bool hasRefReturn,
    bool isAsync) {
    if (hasRefReturn || isAsync) {
        return "{ while (true) {} }";
    }
    if (!retType.isArray() && !retType.isGeneric() && !retType.isNullable() &&
        retType.name.text == "void") {
        return "{ return; }";
    }
    return "{ while (true) {} }";
}

struct OperatorRenderInfo {
    bool isOperator = false;
    bool isConversion = false;
    std::string conversionKeyword; // "implicit" or "explicit"
    std::string operatorToken;     // "+", "-", "==", etc.
};

OperatorRenderInfo classifyOperatorMethodName(const std::string& ident) {
    if (ident == "op_Implicit") return {true, true, "implicit", {}};
    if (ident == "op_Explicit") return {true, true, "explicit", {}};
    if (ident == "op_UnaryPlus" || ident == "op_Addition") return {true, false, {}, "+"};
    if (ident == "op_UnaryNegation" || ident == "op_Subtraction") return {true, false, {}, "-"};
    if (ident == "op_LogicalNot") return {true, false, {}, "!"};
    if (ident == "op_OnesComplement") return {true, false, {}, "~"};
    if (ident == "op_Multiply") return {true, false, {}, "*"};
    if (ident == "op_Division") return {true, false, {}, "/"};
    if (ident == "op_Modulus") return {true, false, {}, "%"};
    if (ident == "op_Equality") return {true, false, {}, "=="};
    if (ident == "op_Inequality") return {true, false, {}, "!="};
    if (ident == "op_LessThan") return {true, false, {}, "<"};
    if (ident == "op_LessThanOrEqual") return {true, false, {}, "<="};
    if (ident == "op_GreaterThan") return {true, false, {}, ">"};
    if (ident == "op_GreaterThanOrEqual") return {true, false, {}, ">="};
    return {};
}

std::string accessibilityLabel(semantic::Accessibility acc) noexcept {
    switch (acc) {
    case semantic::Accessibility::Public: return "public";
    case semantic::Accessibility::Protected: return "protected";
    case semantic::Accessibility::Internal: return "internal";
    case semantic::Accessibility::Private: return "private";
    }
    return "public";
}

void registerBuiltinPublicTypes(semantic::TypeSymbolMap& map) {
    const auto addStruct = [&](const std::string& mod, const std::string& name,
                               std::initializer_list<std::pair<const char*, semantic::PrimitiveType>> flds) {
        semantic::TypeSymbol ts;
        ts.kind = semantic::TypeKind::Struct;
        ts.accessibility = semantic::Accessibility::Public;
        ts.sealedType = true;
        ts.moduleName = mod;
        ts.name = name;
        ts.id = semantic::stableTypeId(ts);
        std::size_t idx = 0;
        for (const auto& [fn, ft] : flds) {
            semantic::FieldSymbol fs;
            fs.name = fn;
            fs.type = ft;
            fs.index = idx++;
            fs.declaringTypeId = ts.id;
            fs.declaringTypeName = semantic::canonicalTypeName(ts);
            fs.id = semantic::stableTypeId(fs.declaringTypeName + "::field:" + fs.name);
            ts.fields.push_back(std::move(fs));
        }
        map[ts.name] = ts;
        map[semantic::canonicalTypeName(ts)] = ts;
    };
    addStruct("RealScript.Math", "Fixed", {{"raw", semantic::PrimitiveType::Int}});
    addStruct("RealScript.Math", "Fixed16", {{"raw", semantic::PrimitiveType::Int}});
    addStruct("RealScript.Math", "Vec2", {{"x", semantic::PrimitiveType::Struct}, {"y", semantic::PrimitiveType::Struct}});
    addStruct("RealScript.Math", "Vec3", {{"x", semantic::PrimitiveType::Struct}, {"y", semantic::PrimitiveType::Struct}, {"z", semantic::PrimitiveType::Struct}});
    addStruct("RealScript.Math", "Quat", {{"x", semantic::PrimitiveType::Struct}, {"y", semantic::PrimitiveType::Struct}, {"z", semantic::PrimitiveType::Struct}, {"w", semantic::PrimitiveType::Struct}});
    addStruct("RealScript.Math", "Rect", {{"position", semantic::PrimitiveType::Struct}, {"size", semantic::PrimitiveType::Struct}});
    addStruct("RealScript.Math", "Transform2D", {{"position", semantic::PrimitiveType::Struct}, {"rotation", semantic::PrimitiveType::Struct}, {"scale", semantic::PrimitiveType::Struct}});
    addStruct("RealScript.Math", "Transform3D", {{"position", semantic::PrimitiveType::Struct}, {"rotation", semantic::PrimitiveType::Struct}, {"scale", semantic::PrimitiveType::Struct}});
    addStruct("RealScript.Math", "AABB", {{"min", semantic::PrimitiveType::Struct}, {"max", semantic::PrimitiveType::Struct}});
    addStruct("RealScript.Math", "Ray", {{"origin", semantic::PrimitiveType::Struct}, {"direction", semantic::PrimitiveType::Struct}});

    addStruct("RealScript.Async", "Awaitable", {{"__handle", semantic::PrimitiveType::Long}});
    addStruct("RealScript.Async", "CancellationToken", {{"__handle", semantic::PrimitiveType::Long}});
    addStruct("RealScript.Async", "CancellationTokenSource", {{"__handle", semantic::PrimitiveType::Long}});

    addStruct("RealScript.Resource", "ResourceHandle", {
        {"__leaseId", semantic::PrimitiveType::Long},
        {"__resourceId", semantic::PrimitiveType::Long},
    });

    const auto addClass = [&](const std::string& mod, const std::string& name,
                              std::initializer_list<std::pair<const char*, semantic::PrimitiveType>> flds = {},
                              bool isIface = false,
                              const std::string& baseType = {}) {
        semantic::TypeSymbol ts;
        ts.kind = semantic::TypeKind::Class;
        ts.accessibility = semantic::Accessibility::Public;
        ts.interfaceType = isIface;
        ts.abstractType = isIface;
        ts.sealedType = isIface;
        ts.moduleName = mod;
        ts.name = name;
        ts.id = semantic::stableTypeId(ts);
        if (!baseType.empty()) {
            ts.baseTypeName = baseType;
            ts.baseTypeId = semantic::stableTypeId(baseType);
        }
        std::size_t idx = 0;
        for (const auto& [fn, ft] : flds) {
            semantic::FieldSymbol fs;
            fs.name = fn;
            fs.type = ft;
            fs.index = idx++;
            fs.declaringTypeId = ts.id;
            fs.declaringTypeName = semantic::canonicalTypeName(ts);
            fs.id = semantic::stableTypeId(fs.declaringTypeName + "::field:" + fs.name);
            ts.fields.push_back(std::move(fs));
        }
        map[ts.name] = ts;
        map[semantic::canonicalTypeName(ts)] = ts;
    };
    addClass("RealScript.Math", "Math");
    addClass("RealScript.Async", "Async");
    addClass("RealScript.Async", "AsyncException", {{"message", semantic::PrimitiveType::String}});
    addClass("RealScript.Async", "AsyncCanceledException",
             {{"message", semantic::PrimitiveType::String}},
             false,
             "RealScript.Async::AsyncException");
    addClass("RealScript.Fiber", "Fiber");

    addClass("RealScript.Resource", "IDisposable", {}, true);
    addClass("RealScript.Resource", "Resource", {
        {"__resourceId", semantic::PrimitiveType::Long},
        {"__path", semantic::PrimitiveType::String},
        {"__guid", semantic::PrimitiveType::String},
        {"__useCount", semantic::PrimitiveType::Int},
        {"__disposed", semantic::PrimitiveType::Bool},
        {"__hostBacked", semantic::PrimitiveType::Bool},
    });
    addClass("RealScript.Resource", "ResourceServer");
}

void syncImportedTypeDescriptors(
    std::vector<bytecode::Module>& bcModules,
    std::vector<mir::Module>* mirModules) {
    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> authoritative;
    for (const auto& mod : bcModules) {
        for (const auto& t : mod.types) {
            if (t.moduleName == mod.name) {
                authoritative[t.id] = &t;
            }
        }
    }
    for (auto& mod : bcModules) {
        for (auto& t : mod.types) {
            if (t.moduleName != mod.name) {
                const auto it = authoritative.find(t.id);
                if (it != authoritative.end()) {
                    t = *it->second;
                }
            }
        }
    }
    if (mirModules) {
        std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> authMir;
        for (const auto& mod : *mirModules) {
            for (const auto& t : mod.types) {
                if (t.moduleName == mod.name) {
                    authMir[t.id] = &t;
                }
            }
        }
        for (auto& mod : *mirModules) {
            for (auto& t : mod.types) {
                if (t.moduleName != mod.name) {
                    const auto it = authMir.find(t.id);
                    if (it != authMir.end()) {
                        t = *it->second;
                    } else {
                        const auto bcIt = authoritative.find(t.id);
                        if (bcIt != authoritative.end()) {
                            t = *bcIt->second;
                        }
                    }
                }
            }
        }
    }
}

void syncMirImportedTypes(std::vector<mir::Module>& mirModules) {
    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> auth;
    for (const auto& mod : mirModules) {
        for (const auto& t : mod.types) {
            if (t.moduleName == mod.name) {
                auth[t.id] = &t;
            }
        }
    }
    for (auto& mod : mirModules) {
        for (auto& t : mod.types) {
            if (t.moduleName != mod.name) {
                if (const auto it = auth.find(t.id); it != auth.end()) {
                    t = *it->second;
                }
            }
        }
    }
}

bool mergeAdditiveSpecializations(mir::Module& dst, const mir::Module& src) {
    if (dst.name != src.name) return false;
    bool changed = false;

    const auto encodeSingleType = [](const semantic::TypeSymbol& ts) {
        mir::Module m;
        m.name = ts.moduleName;
        m.types.push_back(ts);
        return bytecode::encodeModule(makeMirHeaderShell(m));
    };
    const auto encodeSingleFn = [](const mir::Function& fn) {
        mir::Module m;
        m.name = fn.moduleName;
        m.functions.push_back(fn);
        return encodeMirFunctionsOverlay(m);
    };

    std::unordered_map<std::string, debug::SourceFileId> dstPathToId;
    debug::SourceFileId maxFileId = 0;
    for (const auto& sf : dst.sourceFiles) {
        dstPathToId[sf.path] = sf.id;
        if (sf.id > maxFileId) maxFileId = sf.id;
    }

    std::unordered_map<debug::SourceFileId, debug::SourceFileId> fileIdMap;
    std::unordered_set<debug::SourceFileId> stubFileIds;
    for (const auto& sf : src.sourceFiles) {
        if (sf.path.rfind("$stub/", 0) == 0) {
            stubFileIds.insert(sf.id);
            continue;
        }
        const auto existing = dstPathToId.find(sf.path);
        if (existing != dstPathToId.end()) {
            fileIdMap[sf.id] = existing->second;
        } else {
            const debug::SourceFileId nextId = dst.sourceFiles.empty() ? 0u : (maxFileId + 1u);
            maxFileId = nextId;
            debug::SourceFileInfo copy = sf;
            copy.id = nextId;
            dst.sourceFiles.push_back(std::move(copy));
            dstPathToId[sf.path] = nextId;
            fileIdMap[sf.id] = nextId;
            changed = true;
        }
    }

    std::unordered_map<semantic::SymbolId, std::size_t> dstTypeIdx;
    dstTypeIdx.reserve(dst.types.size());
    for (std::size_t i = 0; i < dst.types.size(); ++i) {
        dstTypeIdx[dst.types[i].id] = i;
    }
    for (const auto& t : src.types) {
        if (t.sourceName.rfind("$stub/", 0) == 0) continue;
        const auto found = dstTypeIdx.find(t.id);
        if (found == dstTypeIdx.end()) {
            dstTypeIdx[t.id] = dst.types.size();
            dst.types.push_back(t);
            changed = true;
        } else if (t.moduleName == dst.name) {
            auto& dstType = dst.types[found->second];
            const auto beforeBytes = encodeSingleType(dstType);
            if (t.name.find("__") != std::string::npos) {
                auto extraMethods = dstType.methods;
                dstType = t;
                std::unordered_set<semantic::SymbolId> newMethodIds;
                for (const auto& m : dstType.methods) newMethodIds.insert(m.id);
                for (const auto& m : extraMethods) {
                    if (newMethodIds.insert(m.id).second) {
                        dstType.methods.push_back(m);
                    }
                }
            } else {
                std::unordered_map<semantic::SymbolId, std::size_t> methodIdx;
                for (std::size_t mi = 0; mi < dstType.methods.size(); ++mi) {
                    methodIdx[dstType.methods[mi].id] = mi;
                }
                for (const auto& m : t.methods) {
                    if (m.sourceName.rfind("$stub/", 0) == 0) continue;
                    const auto mIt = methodIdx.find(m.id);
                    if (mIt == methodIdx.end()) {
                        methodIdx[m.id] = dstType.methods.size();
                        dstType.methods.push_back(m);
                    } else if (m.name.find("__") != std::string::npos) {
                        dstType.methods[mIt->second] = m;
                    }
                }
            }
            if (!changed && encodeSingleType(dstType) != beforeBytes) {
                changed = true;
            }
        }
    }

    std::unordered_map<semantic::SymbolId, std::size_t> dstFnIdx;
    dstFnIdx.reserve(dst.functions.size());
    for (std::size_t i = 0; i < dst.functions.size(); ++i) {
        dstFnIdx[dst.functions[i].symbolId] = i;
    }

    const auto remapFileId = [&](debug::SourceFileId& fid) {
        if (const auto it = fileIdMap.find(fid); it != fileIdMap.end()) {
            fid = it->second;
        }
    };

    for (const auto& fn : src.functions) {
        if (fn.debugInfo.sourceName.rfind("$stub/", 0) == 0) continue;
        if (stubFileIds.find(fn.debugInfo.sourceFileId) != stubFileIds.end()) continue;

        const auto existingIt = dstFnIdx.find(fn.symbolId);
        if (existingIt != dstFnIdx.end() && fn.name.find("__") == std::string::npos) {
            continue;
        }

        mir::Function copy = fn;
        remapFileId(copy.debugInfo.sourceFileId);
        remapFileId(copy.debugInfo.declaration.fileId);
        remapFileId(copy.debugInfo.body.fileId);
        for (auto& sp : copy.debugInfo.sequencePoints) {
            remapFileId(sp.range.fileId);
        }
        for (auto& loc : copy.debugInfo.locals) {
            remapFileId(loc.declaration.fileId);
            remapFileId(loc.scope.fileId);
        }
        if (existingIt == dstFnIdx.end()) {
            dstFnIdx[copy.symbolId] = dst.functions.size();
            dst.functions.push_back(std::move(copy));
            changed = true;
        } else {
            auto& dstFn = dst.functions[existingIt->second];
            if (encodeSingleFn(dstFn) != encodeSingleFn(copy)) {
                dstFn = std::move(copy);
                changed = true;
            }
        }
    }

    std::unordered_set<std::string> seenGenInst;
    for (const auto& gi : dst.languageMetadata.genericInstantiations) {
        seenGenInst.insert(gi.generatedName);
    }
    for (const auto& gi : src.languageMetadata.genericInstantiations) {
        if (seenGenInst.insert(gi.generatedName).second) {
            dst.languageMetadata.genericInstantiations.push_back(gi);
            changed = true;
        }
    }

    std::unordered_set<std::string> seenIfaces;
    for (const auto& iface : dst.languageMetadata.interfaces) {
        seenIfaces.insert(iface.typeName);
    }
    for (const auto& iface : src.languageMetadata.interfaces) {
        if (seenIfaces.insert(iface.typeName).second) {
            dst.languageMetadata.interfaces.push_back(iface);
            changed = true;
        }
    }

    return changed;
}

bool mirModuleReferencesBuiltin(const mir::Module& mod, const std::string& builtinName) {
    const std::string prefix = builtinName + "::";
    for (const auto& t : mod.types) {
        if (t.moduleName == builtinName) return true;
        if (t.baseTypeName.rfind(prefix, 0) == 0) return true;
    }
    for (const auto& fn : mod.functions) {
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.symbolName.rfind(prefix, 0) == 0) return true;
            }
        }
    }
    return false;
}

} // namespace

std::uint64_t ModuleCacheKey::digest() const noexcept {
    std::uint64_t h = 14695981039346656037ull;
    mixU64(h, compilerAndAbiVersionFingerprint());
    mixString(h, moduleName);
    mixU64(h, implementationFingerprint);
    mixU8(h, static_cast<std::uint8_t>(optLevel));
    mixU64(h, flags);

    auto sortedDeps = dependencyPublicFingerprints;
    std::sort(
        sortedDeps.begin(),
        sortedDeps.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    mixU64(h, static_cast<std::uint64_t>(sortedDeps.size()));
    for (const auto& [depName, depPubFp] : sortedDeps) {
        mixString(h, depName);
        mixU64(h, depPubFp);
    }
    return h;
}

ModuleBuildCache::ModuleBuildCache(std::filesystem::path cacheDirectory)
    : cacheDirectory_(std::move(cacheDirectory)) {}

void ModuleBuildCache::setCacheDirectory(std::filesystem::path cacheDirectory) {
    cacheDirectory_ = std::move(cacheDirectory);
}

std::string ModuleBuildCache::generateDeclarationStub(
    const ExtractedModuleInfo& moduleInfo) {
    return moduleInfo.declarationStub;
}

std::filesystem::path ModuleBuildCache::materializeDeclarationStub(
    const ExtractedModuleInfo& moduleInfo,
    const std::filesystem::path& overrideCacheDirectory) const {
    const auto activeDir = overrideCacheDirectory.empty() ? cacheDirectory_ : overrideCacheDirectory;
    if (activeDir.empty()) return {};
    const auto stubPath = activeDir / "stubs" /
        (sanitizeFilename(moduleInfo.name) + "_" + hex64(moduleInfo.publicFingerprint) + ".d.rscript");
    std::string existing;
    if (!readTextFile(stubPath, existing) || existing != moduleInfo.declarationStub) {
        if (!writeTextAtomic(stubPath, moduleInfo.declarationStub)) {
            return {};
        }
    }
    return stubPath;
}

std::string ModuleBuildCache::formatStatistics(
    const BuildCacheStatistics& statistics) {
    std::ostringstream out;
    out << "cache-stats cacheHits=" << statistics.cacheHits
        << " cacheMisses=" << statistics.cacheMisses;
    if (statistics.corruptedEntriesHealed != 0) {
        out << " healed=" << statistics.corruptedEntriesHealed;
    }
    if (statistics.aotOutputSkipped) {
        out << " aotSkipped=1";
    }
    return out.str();
}

ModuleGraphExtraction ModuleBuildCache::extractModuleGraph(
    const std::vector<SourceFile>& sources,
    optimization::Options optimizationOptions,
    std::uint64_t flags) {
    ModuleGraphExtraction result;
    if (sources.empty()) return result;

    struct RawModuleGroup {
        std::string name;
        std::vector<ParsedSourceUnit> units;
        std::set<std::string> imports;
        bool deterministic = false;
    };
    std::map<std::string, RawModuleGroup> groups;

    for (const auto& src : sources) {
        ParsedSourceUnit unit;
        unit.rawSource = src;
        unit.sourceText = std::make_shared<text::SourceText>(src.content, src.path);
        diagnostics::DiagnosticBag parseDiag;
        syntax::Parser parser(*unit.sourceText, parseDiag);
        unit.ast = std::make_shared<syntax::CompilationUnitSyntax>(
            parser.parseCompilationUnit());
        unit.tokens = parser.tokens();
        for (const auto& d : parseDiag.items()) {
            result.diagnostics.report(d.code, d.message, d.span, d.severity, src.path);
        }
        unit.moduleName = unit.ast->moduleDeclaration
            ? unit.ast->moduleDeclaration->fullName()
            : "$global";
        for (const auto& imp : unit.ast->imports) {
            unit.imports.push_back(imp.fullName());
        }
        if (unit.ast->moduleDeclaration) {
            for (const auto& list : unit.ast->moduleDeclaration->attributes) {
                for (const auto& attr : list.attributes) {
                    if (attr.nameToken.text == "Deterministic") {
                        unit.deterministic = true;
                    }
                }
            }
        }
        auto& group = groups[unit.moduleName];
        group.name = unit.moduleName;
        if (unit.deterministic) group.deterministic = true;
        group.imports.insert(unit.imports.begin(), unit.imports.end());
        group.units.push_back(std::move(unit));
    }

    // Sort units inside each module deterministically by path
    for (auto& [modName, group] : groups) {
        (void)modName;
        std::sort(
            group.units.begin(),
            group.units.end(),
            [](const ParsedSourceUnit& a, const ParsedSourceUnit& b) {
                return a.rawSource.path < b.rawSource.path;
            });
    }

    // Validate imports & construct DAG for topological sort
    std::map<std::string, std::size_t> inDegree;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto& [modName, group] : groups) {
        inDegree.emplace(modName, 0u);
        for (const auto& imp : group.imports) {
            if (imp == modName) continue;
            if (groups.find(imp) != groups.end()) {
                ++inDegree[modName];
                dependents[imp].push_back(modName);
            } else if (!isBuiltinModuleName(imp)) {
                result.diagnostics.report(
                    "RS4001",
                    "module '" + modName + "' imports missing module '" + imp + "'",
                    {});
            }
        }
    }

    std::priority_queue<
        std::string,
        std::vector<std::string>,
        std::greater<std::string>> ready;
    for (const auto& [modName, deg] : inDegree) {
        if (deg == 0u) ready.push(modName);
    }

    while (!ready.empty()) {
        auto current = ready.top();
        ready.pop();
        result.topologicalOrder.push_back(current);
        const auto depIt = dependents.find(current);
        if (depIt != dependents.end()) {
            for (const auto& child : depIt->second) {
                auto& d = inDegree[child];
                if (d > 0u && --d == 0u) {
                    ready.push(child);
                }
            }
        }
    }

    if (result.topologicalOrder.size() < groups.size()) {
        for (const auto& [modName, deg] : inDegree) {
            if (deg > 0u) {
                result.diagnostics.report(
                    "RS4005",
                    "circular module import dependency involving module '" + modName + "'",
                    {});
                result.topologicalOrder.push_back(modName);
            }
        }
    }

    const auto versionFp = compilerAndAbiVersionFingerprint();
    std::unordered_map<std::string, semantic::TypeSymbolMap> exportedTypesByModule;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::vector<std::vector<const syntax::ParameterSyntax*>>>>
        exportedClassCtorParamsByModule;

    semantic::TypeSymbolMap builtinTypes;
    registerBuiltinPublicTypes(builtinTypes);

    for (const auto& modName : result.topologicalOrder) {
        auto& group = groups.at(modName);
        ExtractedModuleInfo info;
        info.name = modName;
        info.deterministic = group.deterministic;
        info.imports.assign(group.imports.begin(), group.imports.end());
        for (const auto& u : group.units) {
            info.sources.push_back(u.rawSource);
        }

        // 1. Implementation fingerprint: full source content + optLevel + compiler/bytecode ABI version
        std::uint64_t implFp = 14695981039346656037ull;
        mixU64(implFp, versionFp);
        mixOptimizationOptions(implFp, optimizationOptions);
        mixU64(implFp, flags);
        mixString(implFp, modName);
        mixU64(implFp, static_cast<std::uint64_t>(group.units.size()));
        for (const auto& u : group.units) {
            mixString(implFp, u.rawSource.path);
            mixString(implFp, u.rawSource.content);
        }
        info.implementationFingerprint = implFp;

        // 2. Build module-visible type table (imported public types + own type shells)
        semantic::TypeSymbolMap visibleTypes = builtinTypes;
        for (const auto& imp : group.imports) {
            const auto expIt = exportedTypesByModule.find(imp);
            if (expIt != exportedTypesByModule.end()) {
                for (const auto& [k, ts] : expIt->second) {
                    visibleTypes[k] = ts;
                }
            }
        }

        std::unordered_map<std::string, semantic::TypeSymbol> ownTypes;
        std::unordered_map<std::string, const syntax::StructDeclarationSyntax*> privateStructs;
        std::unordered_map<std::string, const syntax::EnumDeclarationSyntax*> privateEnums;
        std::unordered_map<std::string, const syntax::ClassDeclarationSyntax*> privateClasses;
        std::unordered_map<std::string, const syntax::InterfaceDeclarationSyntax*> privateInterfaces;
        std::unordered_map<std::string, const syntax::DelegateDeclarationSyntax*> privateDelegates;
        std::unordered_map<std::string, const ParsedSourceUnit*> privateDeclUnits;

        for (const auto& u : group.units) {
            for (const auto& en : u.ast->enums) {
                auto ts = semantic::declareTypeShell(modName, en);
                diagnostics::DiagnosticBag ignored;
                (void)semantic::populateEnumMembers(ts, en, ignored);
                ownTypes[ts.name] = ts;
                visibleTypes[ts.name] = ts;
                visibleTypes[semantic::canonicalTypeName(ts)] = ts;
                if (ts.accessibility != semantic::Accessibility::Public) {
                    privateEnums[ts.name] = &en;
                    privateDeclUnits[ts.name] = &u;
                }
            }
            for (const auto& st : u.ast->structs) {
                if (!st.typeParameters.empty()) continue;
                auto ts = semantic::declareTypeShell(modName, st);
                ownTypes[ts.name] = ts;
                visibleTypes[ts.name] = ts;
                visibleTypes[semantic::canonicalTypeName(ts)] = ts;
                if (ts.accessibility != semantic::Accessibility::Public) {
                    privateStructs[ts.name] = &st;
                    privateDeclUnits[ts.name] = &u;
                }
            }
            for (const auto& cl : u.ast->classes) {
                if (!cl.typeParameters.empty()) continue;
                auto ts = semantic::declareTypeShell(modName, cl);
                ownTypes[ts.name] = ts;
                visibleTypes[ts.name] = ts;
                visibleTypes[semantic::canonicalTypeName(ts)] = ts;
                if (ts.accessibility != semantic::Accessibility::Public) {
                    privateClasses[ts.name] = &cl;
                    privateDeclUnits[ts.name] = &u;
                }
            }
            for (const auto& iface : u.ast->interfaces) {
                if (!iface.typeParameters.empty()) continue;
                semantic::TypeSymbol ts;
                ts.kind = semantic::TypeKind::Class;
                ts.accessibility = semantic::accessibilityFromModifiers(iface.modifiers);
                ts.interfaceType = true;
                ts.abstractType = true;
                ts.sealedType = true;
                ts.moduleName = modName;
                ts.name = iface.identifierToken.text;
                ts.id = semantic::stableTypeId(ts);
                ownTypes[ts.name] = ts;
                visibleTypes[ts.name] = ts;
                visibleTypes[semantic::canonicalTypeName(ts)] = ts;
                if (ts.accessibility != semantic::Accessibility::Public) {
                    privateInterfaces[ts.name] = &iface;
                    privateDeclUnits[ts.name] = &u;
                }
            }
            for (const auto& del : u.ast->delegates) {
                if (!del.typeParameters.empty()) continue;
                semantic::TypeSymbol ts;
                ts.kind = semantic::TypeKind::Class;
                ts.accessibility = semantic::accessibilityFromModifiers(del.modifiers);
                ts.delegateType = true;
                ts.sealedType = true;
                ts.moduleName = modName;
                ts.name = del.identifierToken.text;
                ts.id = semantic::stableTypeId(ts);
                ownTypes[ts.name] = ts;
                visibleTypes[ts.name] = ts;
                visibleTypes[semantic::canonicalTypeName(ts)] = ts;
                if (ts.accessibility != semantic::Accessibility::Public) {
                    privateDelegates[ts.name] = &del;
                    privateDeclUnits[ts.name] = &u;
                }
            }
        }

        // Populate struct & class fields and resolve compile-time constants
        for (const auto& u : group.units) {
            for (const auto& st : u.ast->structs) {
                if (!st.typeParameters.empty()) continue;
                auto& ts = ownTypes[st.identifierToken.text];
                diagnostics::DiagnosticBag ignored;
                (void)semantic::populateTypeFields(ts, st, visibleTypes, ignored, true);
                visibleTypes[ts.name] = ts;
                visibleTypes[semantic::canonicalTypeName(ts)] = ts;
            }
            for (const auto& cl : u.ast->classes) {
                if (!cl.typeParameters.empty()) continue;
                auto& ts = ownTypes[cl.identifierToken.text];
                if (!cl.interfaces.empty()) {
                    const auto baseIt = visibleTypes.find(cl.interfaces.front().name.text);
                    if (baseIt != visibleTypes.end() &&
                        baseIt->second.kind == semantic::TypeKind::Class &&
                        !baseIt->second.interfaceType) {
                        ts.baseTypeId = baseIt->second.id;
                        ts.baseTypeName = semantic::canonicalTypeName(baseIt->second);
                    }
                }
                diagnostics::DiagnosticBag ignored;
                (void)semantic::populateTypeFields(ts, cl, visibleTypes, ignored, true);
                visibleTypes[ts.name] = ts;
                visibleTypes[semantic::canonicalTypeName(ts)] = ts;
            }
        }

        bool constProgressed = true;
        while (constProgressed) {
            constProgressed = false;
            for (auto& [tName, ts] : ownTypes) {
                (void)tName;
                if (ts.kind != semantic::TypeKind::Class &&
                    ts.kind != semantic::TypeKind::Struct) {
                    continue;
                }
                if (semantic::resolveConstantFieldsPass(ts, visibleTypes)) {
                    visibleTypes[ts.name] = ts;
                    visibleTypes[semantic::canonicalTypeName(ts)] = ts;
                    constProgressed = true;
                }
            }
        }

        // Identify non-public classes in this module transitively inherited by any public class
        std::unordered_set<std::string> exposedNonPublicClasses;
        {
            std::vector<std::string> baseWorklist;
            for (const auto& [tName, ts] : ownTypes) {
                (void)tName;
                if (ts.kind == semantic::TypeKind::Class &&
                    !ts.interfaceType &&
                    ts.accessibility == semantic::Accessibility::Public &&
                    !ts.baseTypeName.empty()) {
                    baseWorklist.push_back(ts.baseTypeName);
                }
            }
            while (!baseWorklist.empty()) {
                auto baseCanon = std::move(baseWorklist.back());
                baseWorklist.pop_back();
                const auto sep = baseCanon.find("::");
                const std::string baseMod = sep == std::string::npos ? modName : baseCanon.substr(0, sep);
                if (baseMod != modName) continue;
                const std::string localName = sep == std::string::npos ? baseCanon : baseCanon.substr(sep + 2);
                const auto ownIt = ownTypes.find(localName);
                if (ownIt == ownTypes.end()) continue;
                if (ownIt->second.accessibility != semantic::Accessibility::Public) {
                    if (!exposedNonPublicClasses.insert(localName).second) continue;
                    privateClasses.erase(localName);
                }
                if (!ownIt->second.baseTypeName.empty()) {
                    baseWorklist.push_back(ownIt->second.baseTypeName);
                }
            }
        }

        // Record constructor parameter lists of public (and exposed base) classes for derived `: base(...)` stub emission
        auto& ownClassCtorParams = exportedClassCtorParamsByModule[modName];
        for (const auto& u : group.units) {
            for (const auto& cl : u.ast->classes) {
                if (!cl.typeParameters.empty()) continue;
                const auto& ts = ownTypes[cl.identifierToken.text];
                const bool isExposedBase =
                    exposedNonPublicClasses.find(ts.name) != exposedNonPublicClasses.end();
                if (ts.accessibility != semantic::Accessibility::Public && !isExposedBase) continue;
                auto& ctorList = ownClassCtorParams[semantic::canonicalTypeName(ts)];
                for (const auto& ctor : cl.constructors) {
                    const auto cAcc = semantic::accessibilityFromModifiers(ctor.modifiers);
                    const bool isStatic = semantic::hasModifier(
                        ctor.modifiers, syntax::SyntaxKind::StaticKeyword);
                    if (isStatic ||
                        (cAcc != semantic::Accessibility::Public &&
                         cAcc != semantic::Accessibility::Protected &&
                         !(isExposedBase && cAcc == semantic::Accessibility::Internal))) {
                        continue;
                    }
                    std::vector<const syntax::ParameterSyntax*> pVec;
                    for (const auto& p : ctor.parameters) pVec.push_back(&p);
                    ctorList.push_back(std::move(pVec));
                }
            }
        }

        // Export public (and exposed base) types to downstream modules
        auto& exportedMap = exportedTypesByModule[modName];
        for (const auto& [tName, ts] : ownTypes) {
            (void)tName;
            if (ts.accessibility == semantic::Accessibility::Public ||
                exposedNonPublicClasses.find(ts.name) != exposedNonPublicClasses.end()) {
                exportedMap[ts.name] = ts;
                exportedMap[semantic::canonicalTypeName(ts)] = ts;
            }
        }

        // 3. Extract canonical public surface items + generate .d.rscript declaration stub
        std::vector<std::string> publicSurfaceItems;
        std::ostringstream stubBody;
        std::unordered_set<std::string> neededLayoutPrivateTypes;
        bool moduleHasPublicGenerics = false;

        // Module-level attributes
        std::vector<std::string> moduleAttrStubs;
        for (const auto& u : group.units) {
            if (u.ast->moduleDeclaration && !u.ast->moduleDeclaration->attributes.empty()) {
                const auto canonicalAttr = formatAttributesCanonical(
                    u.ast->moduleDeclaration->attributes, *u.sourceText);
                if (!canonicalAttr.empty()) {
                    publicSurfaceItems.push_back("module-attr:" + canonicalAttr);
                    moduleAttrStubs.push_back(renderAttributesForStub(
                        u.ast->moduleDeclaration->attributes, *u.sourceText, ""));
                }
            }
        }

        // Enums
        for (const auto& u : group.units) {
            for (const auto& en : u.ast->enums) {
                const auto& ts = ownTypes.at(en.identifierToken.text);
                if (ts.accessibility != semantic::Accessibility::Public) continue;
                std::ostringstream sig;
                sig << "enum:" << formatAttributesCanonical(en.attributes, *u.sourceText)
                    << ':' << semantic::canonicalTypeName(ts) << '{';
                stubBody << renderAttributesForStub(en.attributes, *u.sourceText, "");
                stubBody << "public enum " << ts.name << " {\n";
                for (const auto& em : ts.enumMembers) {
                    sig << em.name << '=' << em.value << ';';
                    stubBody << "    " << em.name << " = " << em.value << ",\n";
                }
                sig << '}';
                stubBody << "}\n\n";
                publicSurfaceItems.push_back(sig.str());
            }
        }

        // Interfaces (STRICTLY preserve declaration order for interfaceSlot indices!)
        for (const auto& u : group.units) {
            for (const auto& iface : u.ast->interfaces) {
                const auto acc = semantic::accessibilityFromModifiers(iface.modifiers);
                if (acc != semantic::Accessibility::Public) continue;
                if (!iface.typeParameters.empty()) {
                    moduleHasPublicGenerics = true;
                    publicSurfaceItems.push_back(
                        "generic-interface:" + normalizedSpanTokens(u.tokens, iface.span()));
                    stubBody << u.sourceText->view(iface.span()) << "\n\n";
                    continue;
                }
                std::ostringstream sig;
                sig << "interface:" << formatAttributesCanonical(iface.attributes, *u.sourceText)
                    << ':' << modName << "::" << iface.identifierToken.text << '{';
                stubBody << renderAttributesForStub(iface.attributes, *u.sourceText, "");
                stubBody << "public interface " << iface.identifierToken.text << " {\n";
                for (std::size_t mi = 0; mi < iface.methods.size(); ++mi) {
                    const auto& m = iface.methods[mi];
                    sig << "slot#" << mi << ':'
                        << formatAttributesCanonical(m.attributes, *u.sourceText)
                        << canonicalizeTypeSyntax(m.returnType, visibleTypes) << ' '
                        << m.identifierToken.text << '(';
                    stubBody << renderAttributesForStub(m.attributes, *u.sourceText, "    ");
                    stubBody << "    " << renderTypeSyntaxForStub(m.returnType) << ' '
                             << m.identifierToken.text << '(';
                    for (std::size_t pi = 0; pi < m.parameters.size(); ++pi) {
                        if (pi != 0) {
                            sig << ',';
                            stubBody << ", ";
                        }
                        sig << formatParameterSignature(
                            m.parameters[pi], *u.sourceText, nullptr, visibleTypes);
                        stubBody << renderParameterForStub(
                            m.parameters[pi], *u.sourceText, nullptr, visibleTypes);
                    }
                    if (m.hasBody()) {
                        moduleHasPublicGenerics = true;
                        sig << ')' << normalizedSpanTokens(u.tokens, m.body->span()) << ';';
                        stubBody << ") " << u.sourceText->view(m.body->span()) << "\n";
                    } else {
                        sig << ");";
                        stubBody << ");\n";
                    }
                }
                sig << '}';
                stubBody << "}\n\n";
                publicSurfaceItems.push_back(sig.str());
            }
        }

        // Delegates
        for (const auto& u : group.units) {
            for (const auto& del : u.ast->delegates) {
                const auto acc = semantic::accessibilityFromModifiers(del.modifiers);
                if (acc != semantic::Accessibility::Public) continue;
                if (!del.typeParameters.empty()) {
                    moduleHasPublicGenerics = true;
                    publicSurfaceItems.push_back(
                        "generic-delegate:" + normalizedSpanTokens(u.tokens, del.span()));
                    stubBody << u.sourceText->view(del.span()) << "\n\n";
                    continue;
                }
                std::ostringstream sig;
                sig << "delegate:" << formatAttributesCanonical(del.attributes, *u.sourceText)
                    << ':' << canonicalizeTypeSyntax(del.returnType, visibleTypes)
                    << ' ' << modName << "::" << del.identifierToken.text << '(';
                stubBody << renderAttributesForStub(del.attributes, *u.sourceText, "");
                stubBody << "public delegate " << renderTypeSyntaxForStub(del.returnType)
                         << ' ' << del.identifierToken.text << '(';
                for (std::size_t pi = 0; pi < del.parameters.size(); ++pi) {
                    if (pi != 0) {
                        sig << ',';
                        stubBody << ", ";
                    }
                    sig << formatParameterSignature(
                        del.parameters[pi], *u.sourceText, nullptr, visibleTypes);
                    stubBody << renderParameterForStub(
                        del.parameters[pi], *u.sourceText, nullptr, visibleTypes);
                }
                sig << ')';
                stubBody << ");\n\n";
                publicSurfaceItems.push_back(sig.str());
            }
        }

        // Helper for Structs & Classes
        const auto processRecordType = [&](const auto& decl, const ParsedSourceUnit& u, bool isStruct) {
            const auto acc = semantic::accessibilityFromModifiers(decl.modifiers);
            const bool isExposedBase =
                !isStruct &&
                exposedNonPublicClasses.find(decl.identifierToken.text) !=
                    exposedNonPublicClasses.end();
            if (acc != semantic::Accessibility::Public && !isExposedBase) return;

            if (!decl.typeParameters.empty()) {
                moduleHasPublicGenerics = true;
                publicSurfaceItems.push_back(
                    std::string(isStruct ? "generic-struct:" : "generic-class:") +
                    normalizedSpanTokens(u.tokens, decl.span()));
                stubBody << u.sourceText->view(decl.span()) << "\n\n";
                return;
            }

            const auto& ts = ownTypes.at(decl.identifierToken.text);
            std::ostringstream sig;
            sig << (isStruct ? "struct:" : "class:")
                << accessibilityLabel(acc) << ':'
                << formatAttributesCanonical(decl.attributes, *u.sourceText) << ':'
                << (ts.abstractType ? "abstract:" : "")
                << (ts.sealedType ? "sealed:" : "")
                << (ts.staticType ? "static:" : "")
                << semantic::canonicalTypeName(ts);

            stubBody << renderAttributesForStub(decl.attributes, *u.sourceText, "");
            stubBody << accessibilityLabel(acc) << ' ';
            if (ts.staticType) stubBody << "static ";
            else {
                if (ts.abstractType) stubBody << "abstract ";
                if (!isStruct && ts.sealedType) stubBody << "sealed ";
            }
            stubBody << (isStruct ? "struct " : "class ") << ts.name;

            if (!decl.interfaces.empty()) {
                sig << ":bases[";
                stubBody << " : ";
                for (std::size_t bi = 0; bi < decl.interfaces.size(); ++bi) {
                    if (bi != 0) {
                        sig << ',';
                        stubBody << ", ";
                    }
                    collectReferencedTypeNames(decl.interfaces[bi], neededLayoutPrivateTypes);
                    sig << canonicalizeTypeSyntax(decl.interfaces[bi], visibleTypes);
                    stubBody << renderTypeSyntaxForStub(decl.interfaces[bi]);
                }
                sig << ']';
            }
            sig << '{';
            stubBody << " {\n";

            // Fields & Memory Layout (all declared fields in exact slot order)
            bool hasDynamicInstanceInit = false;
            for (std::size_t fi = 0; fi < decl.fields.size() && fi < ts.fields.size(); ++fi) {
                const auto& fSyn = decl.fields[fi];
                const auto& fSym = ts.fields[fi];
                collectReferencedTypeNames(fSyn.type, neededLayoutPrivateTypes);

                const bool isExportedField =
                    fSym.accessibility == semantic::Accessibility::Public ||
                    (!isStruct && fSym.accessibility == semantic::Accessibility::Protected);

                sig << "field#" << fi << ':'
                    << static_cast<int>(fSym.accessibility) << ':'
                    << (fSym.staticField ? "S" : "I")
                    << (fSym.constField ? "C" : "-")
                    << (fSym.readonlyField ? "R" : "-") << ':'
                    << canonicalizeTypeSyntax(fSyn.type, visibleTypes) << ':';
                if (isExportedField || isStruct) {
                    sig << fSym.name;
                }
                if (fSym.hasConstantValue && (isExportedField || !fSym.staticField)) {
                    sig << '=' << canonicalTokenValueSignature(fSym.constantValue);
                }
                if (!fSym.staticField && !fSym.constField &&
                    !fSym.hasConstantValue && fSyn.initializer != nullptr) {
                    hasDynamicInstanceInit = true;
                }
                sig << ';';

                // Emit field in .d.rscript stub to preserve exact slot layout
                stubBody << renderAttributesForStub(fSyn.attributes, *u.sourceText, "    ");
                stubBody << "    " << accessibilityLabel(fSym.accessibility) << ' ';
                if (fSym.constField) stubBody << "const ";
                else {
                    if (fSym.staticField) stubBody << "static ";
                    if (fSym.readonlyField) stubBody << "readonly ";
                }
                stubBody << renderTypeSyntaxForStub(fSyn.type) << ' ' << fSym.name;
                if (fSym.hasConstantValue) {
                    stubBody << " = " << renderConstantTokenValue(
                        fSym.constantValue, fSym.type, fSym.typeName, visibleTypes);
                } else if (fSym.constField) {
                    stubBody << " = " << dummyArgumentForTypeSyntax(fSyn.type, visibleTypes);
                }
                stubBody << ";\n";
            }

            // Events (all events add synthetic $event_*_delegate field to class layout)
            for (const auto& ev : decl.events) {
                const auto evAcc = semantic::accessibilityFromModifiers(ev.modifiers);
                collectReferencedTypeNames(ev.delegateType, neededLayoutPrivateTypes);
                sig << "event:" << static_cast<int>(evAcc) << ':'
                    << canonicalizeTypeSyntax(ev.delegateType, visibleTypes) << ':'
                    << ev.identifierToken.text << ';';
                stubBody << renderAttributesForStub(ev.attributes, *u.sourceText, "    ");
                stubBody << "    " << accessibilityLabel(evAcc) << " event "
                         << renderTypeSyntaxForStub(ev.delegateType) << ' '
                         << ev.identifierToken.text << ";\n";
            }

            // Constructors (public & protected; plus implicit synthesized parameterless .ctor check)
            auto findBaseCtorDummyCall = [&]() -> std::string {
                if (isStruct || ts.baseTypeName.empty()) return {};
                const auto sepPos = ts.baseTypeName.find("::");
                const std::string baseMod = sepPos == std::string::npos
                    ? modName
                    : ts.baseTypeName.substr(0, sepPos);
                const auto modCtorsIt = exportedClassCtorParamsByModule.find(baseMod);
                if (modCtorsIt == exportedClassCtorParamsByModule.end()) return {};
                const auto clsCtorsIt = modCtorsIt->second.find(ts.baseTypeName);
                if (clsCtorsIt == modCtorsIt->second.end() || clsCtorsIt->second.empty()) {
                    return {};
                }
                for (const auto& pList : clsCtorsIt->second) {
                    bool canCallEmpty = true;
                    for (const auto* p : pList) {
                        if (!p->defaultValue && !p->paramsKeyword) {
                            canCallEmpty = false;
                            break;
                        }
                    }
                    if (canCallEmpty) return {};
                }
                const auto& firstCtor = clsCtorsIt->second.front();
                std::ostringstream args;
                args << " : base(";
                for (std::size_t pi = 0; pi < firstCtor.size(); ++pi) {
                    if (pi != 0) args << ", ";
                    args << dummyArgumentForTypeSyntax(firstCtor[pi]->type, visibleTypes);
                }
                args << ')';
                return args.str();
            };
            const std::string baseInitClause = findBaseCtorDummyCall();

            bool emittedAnyInstanceCtor = false;
            for (const auto& ctor : decl.constructors) {
                const bool isStaticCtor = semantic::hasModifier(
                    ctor.modifiers, syntax::SyntaxKind::StaticKeyword);
                if (isStaticCtor) continue;
                emittedAnyInstanceCtor = true;
                const auto cAcc = semantic::accessibilityFromModifiers(ctor.modifiers);
                const bool isExportedCtor =
                    cAcc == semantic::Accessibility::Public ||
                    (!isStruct && cAcc == semantic::Accessibility::Protected) ||
                    (isExposedBase && cAcc == semantic::Accessibility::Internal);
                if (!isExportedCtor) {
                    stubBody << "    private " << ts.name << "(";
                    for (std::size_t pi = 0; pi < ctor.parameters.size(); ++pi) {
                        if (pi != 0) stubBody << ", ";
                        stubBody << renderParameterForStub(
                            ctor.parameters[pi], *u.sourceText, &ts, visibleTypes);
                    }
                    stubBody << ")" << baseInitClause << " {}\n";
                    sig << "has-nonpublic-ctor;";
                    continue;
                }
                sig << "ctor:" << accessibilityLabel(cAcc) << ':'
                    << formatAttributesCanonical(ctor.attributes, *u.sourceText) << '(';
                stubBody << renderAttributesForStub(ctor.attributes, *u.sourceText, "    ");
                stubBody << "    " << accessibilityLabel(cAcc) << ' ' << ts.name << '(';
                for (std::size_t pi = 0; pi < ctor.parameters.size(); ++pi) {
                    if (pi != 0) {
                        sig << ',';
                        stubBody << ", ";
                    }
                    sig << formatParameterSignature(
                        ctor.parameters[pi], *u.sourceText, &ts, visibleTypes);
                    stubBody << renderParameterForStub(
                        ctor.parameters[pi], *u.sourceText, &ts, visibleTypes);
                }
                sig << ");";
                stubBody << ")" << baseInitClause << " {}\n";
            }

            if (!isStruct && !ts.staticType && !emittedAnyInstanceCtor && hasDynamicInstanceInit) {
                sig << "synth-default-ctor;";
                stubBody << "    public " << ts.name << "()" << baseInitClause << " {}\n";
            }

            // Methods & Operators (public and protected methods/operators affect publicFingerprint)
            for (const auto& m : decl.methods) {
                const auto mAcc = semantic::accessibilityFromModifiers(m.modifiers);
                const bool mIsVtableSlot =
                    semantic::hasModifier(m.modifiers, syntax::SyntaxKind::VirtualKeyword) ||
                    semantic::hasModifier(m.modifiers, syntax::SyntaxKind::OverrideKeyword) ||
                    semantic::hasModifier(m.modifiers, syntax::SyntaxKind::AbstractKeyword);
                const bool isExportedMethod =
                    mAcc == semantic::Accessibility::Public ||
                    (!isStruct && mAcc == semantic::Accessibility::Protected) ||
                    (isExposedBase && mIsVtableSlot && mAcc != semantic::Accessibility::Private);
                if (!isExportedMethod) continue;

                if (!m.typeParameters.empty()) {
                    moduleHasPublicGenerics = true;
                    sig << "generic-method:" << normalizedSpanTokens(u.tokens, m.span()) << ';';
                    stubBody << "    " << u.sourceText->view(m.span()) << '\n';
                    continue;
                }

                const bool isStatic = m.staticKeyword.has_value();
                const bool isVirtual = semantic::hasModifier(m.modifiers, syntax::SyntaxKind::VirtualKeyword);
                const bool isOverride = semantic::hasModifier(m.modifiers, syntax::SyntaxKind::OverrideKeyword);
                const bool isAbstract = semantic::hasModifier(m.modifiers, syntax::SyntaxKind::AbstractKeyword);
                const bool isSealed = semantic::hasModifier(m.modifiers, syntax::SyntaxKind::SealedKeyword);
                const bool isAsync = semantic::hasModifier(m.modifiers, syntax::SyntaxKind::AsyncKeyword);
                const bool hasRefRet = m.refReturnKeyword.has_value();
                const auto opInfo = classifyOperatorMethodName(m.identifierToken.text);

                if (opInfo.isOperator) {
                    sig << "operator:" << accessibilityLabel(mAcc) << ':'
                        << formatAttributesCanonical(m.attributes, *u.sourceText) << ':'
                        << (isStatic ? "static:" : "")
                        << (hasRefRet ? "ref:" : "");
                    if (opInfo.isConversion) {
                        sig << opInfo.conversionKeyword << "->"
                            << canonicalizeTypeSyntax(m.returnType, visibleTypes) << '(';
                    } else {
                        sig << canonicalizeTypeSyntax(m.returnType, visibleTypes)
                            << " operator " << opInfo.operatorToken << '(';
                    }

                    stubBody << renderAttributesForStub(m.attributes, *u.sourceText, "    ");
                    stubBody << "    " << accessibilityLabel(mAcc) << ' ';
                    if (isStatic) stubBody << "static ";
                    if (opInfo.isConversion) {
                        stubBody << opInfo.conversionKeyword << " operator "
                                 << renderTypeSyntaxForStub(m.returnType) << '(';
                    } else {
                        if (hasRefRet) stubBody << "ref ";
                        stubBody << renderTypeSyntaxForStub(m.returnType)
                                 << " operator " << opInfo.operatorToken << '(';
                    }
                    for (std::size_t pi = 0; pi < m.parameters.size(); ++pi) {
                        if (pi != 0) {
                            sig << ',';
                            stubBody << ", ";
                        }
                        sig << formatParameterSignature(
                            m.parameters[pi], *u.sourceText, &ts, visibleTypes);
                        stubBody << renderParameterForStub(
                            m.parameters[pi], *u.sourceText, &ts, visibleTypes);
                    }
                    sig << ");";
                    stubBody << ") "
                             << dummyFunctionBodyForReturnType(m.returnType, hasRefRet, false)
                             << '\n';
                    continue;
                }

                sig << "method:" << accessibilityLabel(mAcc) << ':'
                    << formatAttributesCanonical(m.attributes, *u.sourceText) << ':'
                    << (isStatic ? "static:" : "")
                    << (isVirtual ? "virtual:" : "")
                    << (isOverride ? "override:" : "")
                    << (isAbstract ? "abstract:" : "")
                    << (isSealed ? "sealed:" : "")
                    << (isAsync ? "async:" : "")
                    << (hasRefRet ? "ref:" : "")
                    << canonicalizeTypeSyntax(m.returnType, visibleTypes) << ' '
                    << m.identifierToken.text << '(';

                stubBody << renderAttributesForStub(m.attributes, *u.sourceText, "    ");
                stubBody << "    " << accessibilityLabel(mAcc) << ' ';
                if (isStatic) stubBody << "static ";
                if (isVirtual) stubBody << "virtual ";
                if (isOverride) stubBody << "override ";
                if (isAbstract) stubBody << "abstract ";
                if (isSealed) stubBody << "sealed ";
                if (isAsync) stubBody << "async ";
                if (hasRefRet) stubBody << "ref ";
                stubBody << renderTypeSyntaxForStub(m.returnType) << ' '
                         << m.identifierToken.text << '(';

                for (std::size_t pi = 0; pi < m.parameters.size(); ++pi) {
                    if (pi != 0) {
                        sig << ',';
                        stubBody << ", ";
                    }
                    sig << formatParameterSignature(
                        m.parameters[pi], *u.sourceText, &ts, visibleTypes);
                    stubBody << renderParameterForStub(
                        m.parameters[pi], *u.sourceText, &ts, visibleTypes);
                }
                sig << ");";
                if (isAbstract || m.semicolonToken.has_value()) {
                    stubBody << ");\n";
                } else {
                    stubBody << ") "
                             << dummyFunctionBodyForReturnType(m.returnType, hasRefRet, isAsync)
                             << '\n';
                }
            }

            // Properties
            for (const auto& prop : decl.properties) {
                const auto pAcc = semantic::accessibilityFromModifiers(prop.modifiers);
                const bool getterAuto = prop.getter && prop.getter->semicolonToken.has_value();
                const bool setterAuto = prop.setter && prop.setter->semicolonToken.has_value();
                const bool autoProp = getterAuto || setterAuto;
                const bool isExportedProp =
                    pAcc == semantic::Accessibility::Public ||
                    (!isStruct && pAcc == semantic::Accessibility::Protected);

                if (autoProp) {
                    collectReferencedTypeNames(prop.type, neededLayoutPrivateTypes);
                    sig << "autoprop-layout:" << static_cast<int>(pAcc) << ':'
                        << (prop.staticKeyword ? "S:" : "I:")
                        << canonicalizeTypeSyntax(prop.type, visibleTypes) << ':'
                        << prop.identifierToken.text
                        << (prop.getter ? ":g" : ":-")
                        << (prop.setter ? ":s" : ":-");
                    if (prop.initializer) {
                        syntax::TokenValue cVal;
                        semantic::PrimitiveType cType = semantic::PrimitiveType::Error;
                        if (semantic::evaluateConstantSyntaxExpression(
                                *prop.initializer, &ts, visibleTypes, cVal, cType)) {
                            sig << '=' << canonicalTokenValueSignature(cVal);
                        }
                    }
                    sig << ';';

                    stubBody << renderAttributesForStub(prop.attributes, *u.sourceText, "    ");
                    stubBody << "    " << accessibilityLabel(pAcc) << ' ';
                    if (prop.staticKeyword) stubBody << "static ";
                    stubBody << renderTypeSyntaxForStub(prop.type) << ' '
                             << prop.identifierToken.text << " { ";
                    if (prop.getter) stubBody << "get; ";
                    if (prop.setter) stubBody << "set; ";
                    stubBody << '}';
                    if (prop.initializer) {
                        syntax::TokenValue cVal;
                        semantic::PrimitiveType cType = semantic::PrimitiveType::Error;
                        if (semantic::evaluateConstantSyntaxExpression(
                                *prop.initializer, &ts, visibleTypes, cVal, cType)) {
                            const auto resolvedP = visibleTypes.find(prop.type.name.text);
                            const auto pPrim = semantic::resolvePrimitiveType(prop.type.name.text);
                            const auto targetPrim = pPrim != semantic::PrimitiveType::Error
                                ? pPrim
                                : (resolvedP != visibleTypes.end()
                                    ? (resolvedP->second.kind == semantic::TypeKind::Enum
                                        ? semantic::PrimitiveType::Enum
                                        : semantic::PrimitiveType::Object)
                                    : cType);
                            const std::string targetName = resolvedP != visibleTypes.end()
                                ? semantic::canonicalTypeName(resolvedP->second)
                                : std::string{};
                            stubBody << " = " << renderConstantTokenValue(
                                cVal, targetPrim, targetName, visibleTypes) << ';';
                        }
                    }
                    stubBody << '\n';
                } else if (isExportedProp) {
                    sig << "prop:" << accessibilityLabel(pAcc) << ':'
                        << formatAttributesCanonical(prop.attributes, *u.sourceText) << ':'
                        << (prop.staticKeyword ? "S:" : "I:")
                        << canonicalizeTypeSyntax(prop.type, visibleTypes) << ':'
                        << prop.identifierToken.text
                        << (prop.getter ? ":g" : ":-")
                        << (prop.setter ? ":s" : ":-") << ';';

                    stubBody << renderAttributesForStub(prop.attributes, *u.sourceText, "    ");
                    stubBody << "    " << accessibilityLabel(pAcc) << ' ';
                    if (prop.staticKeyword) stubBody << "static ";
                    stubBody << renderTypeSyntaxForStub(prop.type) << ' '
                             << prop.identifierToken.text << " {\n";
                    if (prop.getter) {
                        stubBody << "        get "
                                 << dummyFunctionBodyForReturnType(prop.type, false, false)
                                 << '\n';
                    }
                    if (prop.setter) {
                        stubBody << "        set { return; }\n";
                    }
                    stubBody << "    }\n";
                }
            }

            // Sequences (affect class layout via synthetic state/local fields)
            for (const auto& seq : decl.sequences) {
                sig << "sequence:" << normalizedSpanTokens(u.tokens, seq.span()) << ';';
                stubBody << "    " << u.sourceText->view(seq.span()) << '\n';
            }

            sig << '}';
            stubBody << "}\n\n";
            publicSurfaceItems.push_back(sig.str());
        };

        for (const auto& u : group.units) {
            for (const auto& st : u.ast->structs) {
                processRecordType(st, u, true);
            }
            for (const auto& cl : u.ast->classes) {
                processRecordType(cl, u, false);
            }
        }

        // Top-level Functions (public functions)
        for (const auto& u : group.units) {
            for (const auto& fn : u.ast->functions) {
                const auto fAcc = semantic::accessibilityFromModifiers(fn.modifiers);
                if (fAcc != semantic::Accessibility::Public) continue;

                if (!fn.typeParameters.empty()) {
                    moduleHasPublicGenerics = true;
                    publicSurfaceItems.push_back(
                        "generic-fn:" + normalizedSpanTokens(u.tokens, fn.span()));
                    stubBody << u.sourceText->view(fn.span()) << "\n\n";
                    continue;
                }

                const bool isStatic = fn.staticKeyword.has_value();
                const bool isAsync = semantic::hasModifier(fn.modifiers, syntax::SyntaxKind::AsyncKeyword);
                const bool hasRefRet = fn.refReturnKeyword.has_value();

                std::ostringstream sig;
                sig << "fn:" << formatAttributesCanonical(fn.attributes, *u.sourceText) << ':'
                    << (isStatic ? "static:" : "")
                    << (isAsync ? "async:" : "")
                    << (hasRefRet ? "ref:" : "")
                    << canonicalizeTypeSyntax(fn.returnType, visibleTypes) << ' '
                    << modName << "::" << fn.identifierToken.text << '(';

                stubBody << renderAttributesForStub(fn.attributes, *u.sourceText, "");
                stubBody << "public ";
                if (isStatic) stubBody << "static ";
                if (isAsync) stubBody << "async ";
                if (hasRefRet) stubBody << "ref ";
                stubBody << renderTypeSyntaxForStub(fn.returnType) << ' '
                         << fn.identifierToken.text << '(';

                for (std::size_t pi = 0; pi < fn.parameters.size(); ++pi) {
                    if (pi != 0) {
                        sig << ',';
                        stubBody << ", ";
                    }
                    sig << formatParameterSignature(
                        fn.parameters[pi], *u.sourceText, nullptr, visibleTypes);
                    stubBody << renderParameterForStub(
                        fn.parameters[pi], *u.sourceText, nullptr, visibleTypes);
                }
                sig << ')';
                if (fn.semicolonToken.has_value()) {
                    sig << ":extern";
                    stubBody << ");\n";
                } else {
                    stubBody << ") "
                             << dummyFunctionBodyForReturnType(fn.returnType, hasRefRet, isAsync)
                             << '\n';
                }
                publicSurfaceItems.push_back(sig.str());
            }
        }

        // If module exports any public generic template, include same-module internal/private
        // helper declarations in publicFingerprint because hygienic AST monomorphization binds
        // generic bodies against same-module internal/private symbols.
        info.hasPublicGenerics = moduleHasPublicGenerics;
        if (moduleHasPublicGenerics) {
            for (const auto& u : group.units) {
                for (const auto& fn : u.ast->functions) {
                    if (semantic::accessibilityFromModifiers(fn.modifiers) !=
                        semantic::Accessibility::Public) {
                        publicSurfaceItems.push_back(
                            "generic-dep-internal-fn:" + normalizedSpanTokens(u.tokens, fn.span()));
                    }
                }
                for (const auto& cl : u.ast->classes) {
                    if (semantic::accessibilityFromModifiers(cl.modifiers) !=
                        semantic::Accessibility::Public) {
                        publicSurfaceItems.push_back(
                            "generic-dep-internal-class:" + normalizedSpanTokens(u.tokens, cl.span()));
                    } else {
                        for (const auto& m : cl.methods) {
                            const auto mAcc = semantic::accessibilityFromModifiers(m.modifiers);
                            if (mAcc != semantic::Accessibility::Public &&
                                mAcc != semantic::Accessibility::Protected) {
                                publicSurfaceItems.push_back(
                                    "generic-dep-priv-method:" + cl.identifierToken.text + ":" +
                                    normalizedSpanTokens(u.tokens, m.span()));
                            }
                        }
                    }
                }
                for (const auto& st : u.ast->structs) {
                    if (semantic::accessibilityFromModifiers(st.modifiers) !=
                        semantic::Accessibility::Public) {
                        publicSurfaceItems.push_back(
                            "generic-dep-internal-struct:" + normalizedSpanTokens(u.tokens, st.span()));
                    } else {
                        for (const auto& m : st.methods) {
                            if (semantic::accessibilityFromModifiers(m.modifiers) !=
                                semantic::Accessibility::Public) {
                                publicSurfaceItems.push_back(
                                    "generic-dep-priv-method:" + st.identifierToken.text + ":" +
                                    normalizedSpanTokens(u.tokens, m.span()));
                            }
                        }
                    }
                }
                for (const auto& en : u.ast->enums) {
                    if (semantic::accessibilityFromModifiers(en.modifiers) !=
                        semantic::Accessibility::Public) {
                        publicSurfaceItems.push_back(
                            "generic-dep-internal-enum:" + normalizedSpanTokens(u.tokens, en.span()));
                    }
                }
                for (const auto& iface : u.ast->interfaces) {
                    if (semantic::accessibilityFromModifiers(iface.modifiers) !=
                        semantic::Accessibility::Public) {
                        publicSurfaceItems.push_back(
                            "generic-dep-internal-iface:" + normalizedSpanTokens(u.tokens, iface.span()));
                    }
                }
                for (const auto& del : u.ast->delegates) {
                    if (semantic::accessibilityFromModifiers(del.modifiers) !=
                        semantic::Accessibility::Public) {
                        publicSurfaceItems.push_back(
                            "generic-dep-internal-del:" + normalizedSpanTokens(u.tokens, del.span()));
                    }
                }
            }
        }

        // Emit minimal layout shells for any module-private types referenced by public/exposed types
        std::ostringstream privateLayoutStubs;
        std::vector<std::string> layoutQueue(
            neededLayoutPrivateTypes.begin(), neededLayoutPrivateTypes.end());
        std::unordered_set<std::string> visitedLayoutTypes;
        const auto enqueueLayoutDeps = [&](const syntax::TypeSyntax& tSyn) {
            collectReferencedTypeNames(tSyn, neededLayoutPrivateTypes);
            for (const auto& dep : neededLayoutPrivateTypes) {
                if (visitedLayoutTypes.find(dep) == visitedLayoutTypes.end()) {
                    layoutQueue.push_back(dep);
                }
            }
        };
        for (std::size_t qi = 0; qi < layoutQueue.size(); ++qi) {
            const auto tName = layoutQueue[qi];
            if (!visitedLayoutTypes.insert(tName).second) continue;
            if (const auto pe = privateEnums.find(tName); pe != privateEnums.end()) {
                const auto& ts = ownTypes.at(tName);
                std::ostringstream ls;
                ls << "layout-private-enum:" << tName << '{';
                privateLayoutStubs << accessibilityLabel(ts.accessibility) << " enum " << tName << " {\n";
                for (const auto& em : ts.enumMembers) {
                    ls << em.name << '=' << em.value << ';';
                    privateLayoutStubs << "    " << em.name << " = " << em.value << ",\n";
                }
                ls << '}';
                privateLayoutStubs << "}\n\n";
                publicSurfaceItems.push_back(ls.str());
            } else if (const auto pi = privateInterfaces.find(tName); pi != privateInterfaces.end()) {
                const auto& ts = ownTypes.at(tName);
                const auto* declUnit = privateDeclUnits.at(tName);
                std::ostringstream ls;
                ls << "layout-private-iface:" << tName << '{';
                privateLayoutStubs << accessibilityLabel(ts.accessibility) << " interface " << tName << " {\n";
                for (std::size_t mi = 0; mi < pi->second->methods.size(); ++mi) {
                    const auto& m = pi->second->methods[mi];
                    enqueueLayoutDeps(m.returnType);
                    ls << "slot#" << mi << ':' << canonicalizeTypeSyntax(m.returnType, visibleTypes)
                       << ' ' << m.identifierToken.text << '(';
                    privateLayoutStubs << "    " << renderTypeSyntaxForStub(m.returnType) << ' '
                                       << m.identifierToken.text << '(';
                    for (std::size_t pIdx = 0; pIdx < m.parameters.size(); ++pIdx) {
                        if (pIdx != 0) {
                            ls << ',';
                            privateLayoutStubs << ", ";
                        }
                        enqueueLayoutDeps(m.parameters[pIdx].type);
                        ls << formatParameterSignature(
                            m.parameters[pIdx], *declUnit->sourceText, nullptr, visibleTypes);
                        privateLayoutStubs << renderParameterForStub(
                            m.parameters[pIdx], *declUnit->sourceText, nullptr, visibleTypes);
                    }
                    ls << ");";
                    privateLayoutStubs << ");\n";
                }
                ls << '}';
                privateLayoutStubs << "}\n\n";
                publicSurfaceItems.push_back(ls.str());
            } else if (const auto pd = privateDelegates.find(tName); pd != privateDelegates.end()) {
                const auto& ts = ownTypes.at(tName);
                const auto* declUnit = privateDeclUnits.at(tName);
                enqueueLayoutDeps(pd->second->returnType);
                std::ostringstream ls;
                ls << "layout-private-del:" << canonicalizeTypeSyntax(pd->second->returnType, visibleTypes)
                   << ' ' << tName << '(';
                privateLayoutStubs << accessibilityLabel(ts.accessibility) << " delegate "
                                   << renderTypeSyntaxForStub(pd->second->returnType) << ' '
                                   << tName << '(';
                for (std::size_t pIdx = 0; pIdx < pd->second->parameters.size(); ++pIdx) {
                    if (pIdx != 0) {
                        ls << ',';
                        privateLayoutStubs << ", ";
                    }
                    enqueueLayoutDeps(pd->second->parameters[pIdx].type);
                    ls << formatParameterSignature(
                        pd->second->parameters[pIdx], *declUnit->sourceText, nullptr, visibleTypes);
                    privateLayoutStubs << renderParameterForStub(
                        pd->second->parameters[pIdx], *declUnit->sourceText, nullptr, visibleTypes);
                }
                ls << ')';
                privateLayoutStubs << ");\n\n";
                publicSurfaceItems.push_back(ls.str());
            } else if (const auto ps = privateStructs.find(tName); ps != privateStructs.end()) {
                std::ostringstream ls;
                ls << "layout-private-struct:" << tName << '{';
                privateLayoutStubs << accessibilityLabel(ownTypes.at(tName).accessibility) << " struct " << tName << " {\n";
                for (const auto& fld : ps->second->fields) {
                    if (semantic::hasModifier(fld.modifiers, syntax::SyntaxKind::ConstKeyword)) continue;
                    enqueueLayoutDeps(fld.type);
                    const bool isStaticFld =
                        semantic::hasModifier(fld.modifiers, syntax::SyntaxKind::StaticKeyword);
                    ls << (isStaticFld ? "S:" : "I:")
                       << canonicalizeTypeSyntax(fld.type, visibleTypes) << ':'
                       << fld.identifierToken.text << ';';
                    privateLayoutStubs << "    " << (isStaticFld ? "static " : "")
                                       << renderTypeSyntaxForStub(fld.type) << ' '
                                       << fld.identifierToken.text << ";\n";
                }
                for (const auto& ev : ps->second->events) {
                    enqueueLayoutDeps(ev.delegateType);
                    ls << "ev:" << canonicalizeTypeSyntax(ev.delegateType, visibleTypes) << ':'
                       << ev.identifierToken.text << ';';
                    privateLayoutStubs << "    event " << renderTypeSyntaxForStub(ev.delegateType)
                                       << ' ' << ev.identifierToken.text << ";\n";
                }
                for (const auto& prop : ps->second->properties) {
                    const bool autoProp =
                        (prop.getter && prop.getter->semicolonToken.has_value()) ||
                        (prop.setter && prop.setter->semicolonToken.has_value());
                    if (!autoProp) continue;
                    enqueueLayoutDeps(prop.type);
                    ls << "ap:" << (prop.staticKeyword ? "S:" : "I:")
                       << canonicalizeTypeSyntax(prop.type, visibleTypes) << ':'
                       << prop.identifierToken.text << ';';
                    privateLayoutStubs << "    " << (prop.staticKeyword ? "static " : "")
                                       << renderTypeSyntaxForStub(prop.type) << ' '
                                       << prop.identifierToken.text << " { ";
                    if (prop.getter) privateLayoutStubs << "get; ";
                    if (prop.setter) privateLayoutStubs << "set; ";
                    privateLayoutStubs << "}\n";
                }
                ls << '}';
                privateLayoutStubs << "}\n\n";
                publicSurfaceItems.push_back(ls.str());
            } else if (const auto pc = privateClasses.find(tName); pc != privateClasses.end()) {
                const auto& ts = ownTypes.at(tName);
                std::ostringstream ls;
                ls << "layout-private-class:" << tName;
                privateLayoutStubs << accessibilityLabel(ts.accessibility) << " class " << tName;
                if (!ts.baseTypeName.empty() && !pc->second->interfaces.empty()) {
                    enqueueLayoutDeps(pc->second->interfaces.front());
                    ls << ":base=" << ts.baseTypeName;
                    privateLayoutStubs << " : " << renderTypeSyntaxForStub(pc->second->interfaces.front());
                }
                ls << '{';
                privateLayoutStubs << " {\n";
                for (const auto& fld : pc->second->fields) {
                    if (semantic::hasModifier(fld.modifiers, syntax::SyntaxKind::ConstKeyword)) continue;
                    enqueueLayoutDeps(fld.type);
                    const bool isStaticFld =
                        semantic::hasModifier(fld.modifiers, syntax::SyntaxKind::StaticKeyword);
                    ls << (isStaticFld ? "S:" : "I:")
                       << canonicalizeTypeSyntax(fld.type, visibleTypes) << ':'
                       << fld.identifierToken.text << ';';
                    privateLayoutStubs << "    " << (isStaticFld ? "static " : "")
                                       << renderTypeSyntaxForStub(fld.type) << ' '
                                       << fld.identifierToken.text << ";\n";
                }
                for (const auto& ev : pc->second->events) {
                    enqueueLayoutDeps(ev.delegateType);
                    ls << "ev:" << canonicalizeTypeSyntax(ev.delegateType, visibleTypes) << ':'
                       << ev.identifierToken.text << ';';
                    privateLayoutStubs << "    event " << renderTypeSyntaxForStub(ev.delegateType)
                                       << ' ' << ev.identifierToken.text << ";\n";
                }
                for (const auto& prop : pc->second->properties) {
                    const bool autoProp =
                        (prop.getter && prop.getter->semicolonToken.has_value()) ||
                        (prop.setter && prop.setter->semicolonToken.has_value());
                    if (!autoProp) continue;
                    enqueueLayoutDeps(prop.type);
                    ls << "ap:" << (prop.staticKeyword ? "S:" : "I:")
                       << canonicalizeTypeSyntax(prop.type, visibleTypes) << ':'
                       << prop.identifierToken.text << ';';
                    privateLayoutStubs << "    " << (prop.staticKeyword ? "static " : "")
                                       << renderTypeSyntaxForStub(prop.type) << ' '
                                       << prop.identifierToken.text << " { ";
                    if (prop.getter) privateLayoutStubs << "get; ";
                    if (prop.setter) privateLayoutStubs << "set; ";
                    privateLayoutStubs << "}\n";
                }
                ls << '}';
                privateLayoutStubs << "}\n\n";
                publicSurfaceItems.push_back(ls.str());
            }
        }

        std::sort(publicSurfaceItems.begin(), publicSurfaceItems.end());
        std::uint64_t pubFp = 14695981039346656037ull;
        mixString(pubFp, modName);
        mixU8(pubFp, info.deterministic ? 1u : 0u);
        mixU64(pubFp, static_cast<std::uint64_t>(publicSurfaceItems.size()));
        for (const auto& item : publicSurfaceItems) {
            mixString(pubFp, item);
        }
        info.publicFingerprint = pubFp;

        std::ostringstream fullStub;
        fullStub << "// Auto-generated RealScript Module Public Interface Stub (.d.rscript)\n";
        for (const auto& mAttr : moduleAttrStubs) fullStub << mAttr;
        if (modName != "$global") {
            fullStub << "module " << modName << ";\n";
        }
        for (const auto& imp : info.imports) {
            fullStub << "import " << imp << ";\n";
        }
        fullStub << '\n' << privateLayoutStubs.str() << stubBody.str();
        info.declarationStub = fullStub.str();

        result.modules.emplace(modName, std::move(info));
    }

    return result;
}

CachedBuildResult ModuleBuildCache::build(
    const std::vector<SourceFile>& sources,
    ModuleBuildCacheOptions options) const {
    CachedBuildResult out;
    if (sources.empty()) return out;

    const std::filesystem::path activeCacheDir =
        options.cacheDirectory.empty() ? cacheDirectory_ : options.cacheDirectory;
    const bool useDiskCache = !activeCacheDir.empty();

    auto graph = extractModuleGraph(sources, options.optimizationOptions, options.flags);
    if (graph.diagnostics.hasErrors()) {
        Compilation fallback(sources);
        auto fullRes = fallback.build();
        out.diagnostics = std::move(fullRes.diagnostics);
        if (!out.diagnostics.hasErrors()) {
            out.diagnostics = std::move(graph.diagnostics);
        }
        return out;
    }

    out.topologicalOrder = graph.topologicalOrder;
    out.statistics.totalModules = graph.topologicalOrder.size();

    // Determine required built-in modules not explicitly present in user sources
    std::vector<std::string> neededBuiltins;
    const auto ensureBuiltinListed = [&](const std::string& bName) {
        if (graph.modules.find(bName) != graph.modules.end()) return;
        if (std::find(neededBuiltins.begin(), neededBuiltins.end(), bName) == neededBuiltins.end()) {
            neededBuiltins.push_back(bName);
        }
    };
    const auto canonicalizeNeededBuiltins = [&]() {
        static constexpr const char* kCanonicalOrder[] = {
            "RealScript.Math",
            "RealScript.Fiber",
            "RealScript.Async",
            "RealScript.Resource",
        };
        std::vector<std::string> ordered;
        ordered.reserve(neededBuiltins.size());
        for (const char* bCand : kCanonicalOrder) {
            if (std::find(neededBuiltins.begin(), neededBuiltins.end(), bCand) !=
                neededBuiltins.end()) {
                ordered.emplace_back(bCand);
            }
        }
        for (const auto& extra : neededBuiltins) {
            if (std::find(ordered.begin(), ordered.end(), extra) == ordered.end()) {
                ordered.push_back(extra);
            }
        }
        neededBuiltins = std::move(ordered);
    };
    {
        bool wantMath = false;
        bool wantFiber = false;
        bool wantAsync = false;
        bool wantResource = false;
        for (const auto& modName : graph.topologicalOrder) {
            const auto& info = graph.modules.at(modName);
            for (const auto& imp : info.imports) {
                if (imp == "RealScript.Math") wantMath = true;
                if (imp == "RealScript.Fiber") wantFiber = true;
                if (imp == "RealScript.Async") wantAsync = true;
                if (imp == "RealScript.Resource") wantResource = true;
            }
            for (const auto& sf : info.sources) {
                if (!wantAsync && sourceMentionsAsyncPrimitives(sf.content)) {
                    wantAsync = true;
                }
                if (!wantResource && sourceMentionsResourcePrimitives(sf.content)) {
                    wantResource = true;
                }
            }
        }
        if (wantMath) ensureBuiltinListed("RealScript.Math");
        if (wantFiber) ensureBuiltinListed("RealScript.Fiber");
        if (wantAsync) ensureBuiltinListed("RealScript.Async");
        if (wantResource) ensureBuiltinListed("RealScript.Resource");
    }

    // Compute transitive dependency sets over user modules
    std::unordered_map<std::string, std::set<std::string>> transitiveDeps;
    for (const auto& modName : graph.topologicalOrder) {
        auto& deps = transitiveDeps[modName];
        for (const auto& imp : graph.modules.at(modName).imports) {
            if (imp == modName || graph.modules.find(imp) == graph.modules.end()) continue;
            deps.insert(imp);
            const auto& upDeps = transitiveDeps[imp];
            deps.insert(upDeps.begin(), upDeps.end());
        }
    }

    struct PerModuleWork {
        ModuleCacheEntryStatus status;
        bytecode::Module bcModule;
        mir::Module mirModule;
        mir::Module unoptMir;
        std::uint64_t loadedOptHash = 0;
        bool loaded = false;
        bool unoptDirty = false;
        std::filesystem::path metaPath;
        std::filesystem::path rsbcPath;
        std::filesystem::path rsmirPath;
        std::filesystem::path unoptPath;
    };
    std::unordered_map<std::string, PerModuleWork> workMap;
    workMap.reserve(graph.topologicalOrder.size());

    const auto tryLoadCachedModule = [&](
        const std::filesystem::path& metaPath,
        const std::filesystem::path& rsbcPath,
        const std::filesystem::path& rsmirPath,
        const std::filesystem::path& unoptPath,
        const std::string& expectedModuleName,
        std::uint64_t expectedKey,
        bool needOptMir,
        bytecode::Module& outBc,
        mir::Module& outOptMir,
        mir::Module& outUnoptMir,
        std::uint64_t& outOptHash,
        bool& outCorrupted) -> bool {
        outCorrupted = false;
        outOptHash = 0;
        std::error_code ec;
        const bool anyExists =
            std::filesystem::exists(metaPath, ec) ||
            std::filesystem::exists(rsbcPath, ec) ||
            std::filesystem::exists(unoptPath, ec) ||
            (needOptMir && std::filesystem::exists(rsmirPath, ec));
        if (!anyExists) return false;

        std::vector<std::uint8_t> metaBytes;
        ModuleMetaRecord meta;
        if (!readBinaryFile(metaPath, metaBytes) ||
            !decodeModuleMeta(metaBytes, meta) ||
            meta.moduleName != expectedModuleName ||
            meta.cacheKey != expectedKey) {
            outCorrupted = true;
            return false;
        }

        std::vector<std::uint8_t> rsbcBytes;
        if (!readBinaryFile(rsbcPath, rsbcBytes) ||
            rsbcBytes.size() != meta.rsbcSize ||
            checksum64(rsbcBytes) != meta.rsbcChecksum) {
            outCorrupted = true;
            return false;
        }

        diagnostics::DiagnosticBag bcDiag;
        if (!decodeCacheBytecodeModule(rsbcBytes, outBc, bcDiag) ||
            outBc.name != expectedModuleName) {
            outCorrupted = true;
            return false;
        }

        std::vector<std::uint8_t> unoptBytes;
        if (!readBinaryFile(unoptPath, unoptBytes) ||
            unoptBytes.size() != meta.unoptSize ||
            checksum64(unoptBytes) != meta.unoptChecksum ||
            !decodeStandaloneMirModule(unoptBytes, outUnoptMir) ||
            outUnoptMir.name != expectedModuleName) {
            outCorrupted = true;
            return false;
        }

        if (needOptMir) {
            std::vector<std::uint8_t> rsmirBytes;
            if (!readBinaryFile(rsmirPath, rsmirBytes) ||
                rsmirBytes.size() != meta.rsmirSize ||
                checksum64(rsmirBytes) != meta.rsmirChecksum ||
                !decodeMirFunctionsOverlay(rsmirBytes, outBc, outOptMir)) {
                outCorrupted = true;
                return false;
            }
        }
        outOptHash = meta.optMirHash;
        for (const auto& bName : meta.usedBuiltins) {
            if (isBuiltinModuleName(bName)) {
                ensureBuiltinListed(bName);
            }
        }
        return true;
    };

    const auto saveCachedModule = [&](
        const std::filesystem::path& metaPath,
        const std::filesystem::path& rsbcPath,
        const std::filesystem::path& rsmirPath,
        const std::filesystem::path& unoptPath,
        const ModuleCacheEntryStatus& st,
        std::uint64_t optMirHash,
        const bytecode::Module& bcMod,
        const mir::Module& optMirMod,
        const mir::Module& unoptMirMod) {
        const auto rsbcBytes = encodeCacheBytecodeModule(bcMod);
        const auto rsmirBytes = encodeMirFunctionsOverlay(optMirMod);
        const auto unoptBytes = encodeStandaloneMirModule(unoptMirMod);
        if (!writeBinaryAtomic(rsbcPath, rsbcBytes)) return;
        if (!writeBinaryAtomic(rsmirPath, rsmirBytes)) return;
        if (!writeBinaryAtomic(unoptPath, unoptBytes)) return;
        ModuleMetaRecord meta;
        meta.moduleName = st.moduleName;
        meta.cacheKey = st.cacheKey;
        meta.implementationFingerprint = st.implementationFingerprint;
        meta.publicFingerprint = st.publicFingerprint;
        meta.dependencyPublicFingerprint = st.dependencyPublicFingerprint;
        meta.optMirHash = optMirHash;
        meta.rsbcSize = rsbcBytes.size();
        meta.rsbcChecksum = checksum64(rsbcBytes);
        meta.rsmirSize = rsmirBytes.size();
        meta.rsmirChecksum = checksum64(rsmirBytes);
        meta.unoptSize = unoptBytes.size();
        meta.unoptChecksum = checksum64(unoptBytes);
        for (const char* bCand : {
                 "RealScript.Math",
                 "RealScript.Fiber",
                 "RealScript.Async",
                 "RealScript.Resource",
             }) {
            if (mirModuleReferencesBuiltin(unoptMirMod, bCand) ||
                mirModuleReferencesBuiltin(optMirMod, bCand)) {
                meta.usedBuiltins.emplace_back(bCand);
            }
        }
        (void)writeBinaryAtomic(metaPath, encodeModuleMeta(meta));
    };

    const auto tryLoadOptArtifact = [&](
        const std::string& modName,
        std::uint64_t optMirHash,
        bool needMir,
        bytecode::Module& outBc,
        mir::Module& outMir) -> bool {
        if (!useDiskCache) return false;
        const auto optDir = activeCacheDir / "opt";
        const auto stem = sanitizeFilename(modName) + "_" + hex64(optMirHash);
        std::vector<std::uint8_t> metaBytes;
        OptArtifactMeta meta;
        if (!readBinaryFile(optDir / (stem + ".ometa"), metaBytes) ||
            !decodeOptArtifactMeta(metaBytes, meta) ||
            meta.moduleName != modName ||
            meta.optMirHash != optMirHash) {
            return false;
        }
        std::vector<std::uint8_t> rsbcBytes;
        if (!readBinaryFile(optDir / (stem + ".rsbc"), rsbcBytes) ||
            rsbcBytes.size() != meta.rsbcSize ||
            checksum64(rsbcBytes) != meta.rsbcChecksum) {
            return false;
        }
        diagnostics::DiagnosticBag diag;
        if (!decodeCacheBytecodeModule(rsbcBytes, outBc, diag) ||
            outBc.name != modName) {
            return false;
        }
        if (needMir) {
            std::vector<std::uint8_t> rsmirBytes;
            if (!readBinaryFile(optDir / (stem + ".rsmir"), rsmirBytes) ||
                rsmirBytes.size() != meta.rsmirSize ||
                checksum64(rsmirBytes) != meta.rsmirChecksum ||
                !decodeMirFunctionsOverlay(rsmirBytes, outBc, outMir)) {
                return false;
            }
        }
        return true;
    };

    const auto saveOptArtifact = [&](
        const std::string& modName,
        std::uint64_t optMirHash,
        const bytecode::Module& bcMod,
        const mir::Module& optMirMod) {
        if (!useDiskCache) return;
        const auto optDir = activeCacheDir / "opt";
        const auto stem = sanitizeFilename(modName) + "_" + hex64(optMirHash);
        const auto rsbcBytes = encodeCacheBytecodeModule(bcMod);
        const auto rsmirBytes = encodeMirFunctionsOverlay(optMirMod);
        if (!writeBinaryAtomic(optDir / (stem + ".rsbc"), rsbcBytes)) return;
        if (!writeBinaryAtomic(optDir / (stem + ".rsmir"), rsmirBytes)) return;
        OptArtifactMeta meta;
        meta.moduleName = modName;
        meta.optMirHash = optMirHash;
        meta.rsbcSize = rsbcBytes.size();
        meta.rsbcChecksum = checksum64(rsbcBytes);
        meta.rsmirSize = rsmirBytes.size();
        meta.rsmirChecksum = checksum64(rsmirBytes);
        (void)writeBinaryAtomic(optDir / (stem + ".ometa"), encodeOptArtifactMeta(meta));
    };

    // Evaluate cache keys & probe disk cache for every user module in topological order
    std::unordered_set<std::string> dirtyModules;
    for (const auto& modName : graph.topologicalOrder) {
        const auto& info = graph.modules.at(modName);
        ModuleCacheKey keySpec;
        keySpec.moduleName = modName;
        keySpec.implementationFingerprint = info.implementationFingerprint;
        keySpec.optLevel = options.optimizationOptions.level;

        std::uint64_t optHash = 14695981039346656037ull;
        mixOptimizationOptions(optHash, options.optimizationOptions);
        mixU64(optHash, options.flags);
        keySpec.flags = optHash;

        std::uint64_t depFp = 14695981039346656037ull;
        for (const auto& depName : transitiveDeps[modName]) {
            const auto depPub = graph.modules.at(depName).publicFingerprint;
            keySpec.dependencyPublicFingerprints.emplace_back(depName, depPub);
            mixString(depFp, depName);
            mixU64(depFp, depPub);
        }

        PerModuleWork mw;
        mw.status.moduleName = modName;
        mw.status.imports = info.imports;
        mw.status.implementationFingerprint = info.implementationFingerprint;
        mw.status.publicFingerprint = info.publicFingerprint;
        mw.status.dependencyPublicFingerprint = depFp;
        mw.status.cacheKey = keySpec.digest();

        if (useDiskCache) {
            const auto safeMod = sanitizeFilename(modName);
            const auto modulesDir = activeCacheDir / "modules";
            const auto stubsDir = activeCacheDir / "stubs";
            const auto stem = safeMod + "_" + hex64(mw.status.cacheKey);
            mw.metaPath = modulesDir / (stem + ".meta");
            mw.rsbcPath = modulesDir / (stem + ".rsbc");
            mw.rsmirPath = modulesDir / (stem + ".rsmir");
            mw.unoptPath = modulesDir / (stem + ".unopt");
            mw.status.bytecodeCachePath = mw.rsbcPath;
            mw.status.stubCachePath =
                stubsDir / (safeMod + "_" + hex64(info.publicFingerprint) + ".d.rscript");

            bool corrupted = false;
            if (tryLoadCachedModule(
                    mw.metaPath,
                    mw.rsbcPath,
                    mw.rsmirPath,
                    mw.unoptPath,
                    modName,
                    mw.status.cacheKey,
                    options.retainMirModules,
                    mw.bcModule,
                    mw.mirModule,
                    mw.unoptMir,
                    mw.loadedOptHash,
                    corrupted)) {
                mw.loaded = true;
                mw.status.cacheHit = true;
                ++out.statistics.cacheHits;
            } else {
                if (corrupted) {
                    std::error_code rmEc;
                    std::filesystem::remove(mw.metaPath, rmEc);
                    std::filesystem::remove(mw.rsbcPath, rmEc);
                    std::filesystem::remove(mw.rsmirPath, rmEc);
                    std::filesystem::remove(mw.unoptPath, rmEc);
                    mw.status.healedCorruptedCache = true;
                    ++out.statistics.corruptedEntriesHealed;
                }
                mw.status.cacheHit = false;
                ++out.statistics.cacheMisses;
                dirtyModules.insert(modName);
            }
        } else {
            ++out.statistics.cacheMisses;
            dirtyModules.insert(modName);
        }

        workMap.emplace(modName, std::move(mw));
    }

    canonicalizeNeededBuiltins();

    const auto computeGraphKey = [&]() -> std::uint64_t {
        std::uint64_t gk = 14695981039346656037ull;
        mixU64(gk, compilerAndAbiVersionFingerprint());
        mixOptimizationOptions(gk, options.optimizationOptions);
        mixU64(gk, options.flags);
        mixU64(gk, static_cast<std::uint64_t>(neededBuiltins.size()));
        for (const auto& bName : neededBuiltins) mixString(gk, bName);
        mixU64(gk, static_cast<std::uint64_t>(graph.topologicalOrder.size()));
        for (const auto& modName : graph.topologicalOrder) {
            mixString(gk, modName);
            mixU64(gk, workMap.at(modName).status.cacheKey);
        }
        return gk;
    };

    // Fast path: 100% warm graph cache hit (bypasses frontend, optimizer, and lowerer completely!)
    if (useDiskCache && dirtyModules.empty()) {
        const auto graphKey = computeGraphKey();
        const auto graphPath = activeCacheDir / "graphs" / ("graph_" + hex64(graphKey) + ".gmeta");
        std::vector<std::uint8_t> gBytes;
        GraphMetaRecord gRec;
        if (readBinaryFile(graphPath, gBytes) &&
            decodeGraphMeta(gBytes, gRec) &&
            gRec.graphKey == graphKey &&
            gRec.moduleOptHashes.size() == neededBuiltins.size() + graph.topologicalOrder.size()) {
            bool allOptLoaded = true;
            std::vector<bytecode::Module> fastBc;
            std::vector<mir::Module> fastMir;
            fastBc.reserve(gRec.moduleOptHashes.size());
            if (options.retainMirModules) fastMir.reserve(gRec.moduleOptHashes.size());

            std::size_t idx = 0;
            for (const auto& bName : neededBuiltins) {
                if (gRec.moduleOptHashes[idx].first != bName) {
                    allOptLoaded = false;
                    break;
                }
                bytecode::Module bc;
                mir::Module mir;
                if (!tryLoadOptArtifact(
                        bName,
                        gRec.moduleOptHashes[idx].second,
                        options.retainMirModules,
                        bc,
                        mir)) {
                    allOptLoaded = false;
                    break;
                }
                fastBc.push_back(std::move(bc));
                if (options.retainMirModules) fastMir.push_back(std::move(mir));
                ++idx;
            }
            if (allOptLoaded) {
                for (const auto& modName : graph.topologicalOrder) {
                    if (gRec.moduleOptHashes[idx].first != modName) {
                        allOptLoaded = false;
                        break;
                    }
                    bytecode::Module bc;
                    mir::Module mir;
                    if (!tryLoadOptArtifact(
                            modName,
                            gRec.moduleOptHashes[idx].second,
                            options.retainMirModules,
                            bc,
                            mir)) {
                        allOptLoaded = false;
                        break;
                    }
                    fastBc.push_back(std::move(bc));
                    if (options.retainMirModules) fastMir.push_back(std::move(mir));
                    ++idx;
                }
            }
            if (allOptLoaded) {
                for (const auto& modName : graph.topologicalOrder) {
                    out.moduleStatuses.push_back(workMap.at(modName).status);
                }
                out.bytecodeModules = std::move(fastBc);
                if (options.retainMirModules) out.mirModules = std::move(fastMir);
                syncImportedTypeDescriptors(
                    out.bytecodeModules,
                    options.retainMirModules ? &out.mirModules : nullptr);
                return out;
            }
        }
    }

    // Load or probe base unoptimized MIR for built-in modules
    std::unordered_map<std::string, mir::Module> builtinUnopt;
    const auto loadOrBuildBaseBuiltin = [&](const std::string& bName) {
        if (builtinUnopt.find(bName) != builtinUnopt.end()) return;
        std::uint64_t bKey = 14695981039346656037ull;
        mixU64(bKey, compilerAndAbiVersionFingerprint());
        mixString(bKey, bName);
        const auto bDir = activeCacheDir / "_builtins";
        const auto unoptFile = bDir / (sanitizeFilename(bName) + "_" + hex64(bKey) + ".unopt");
        if (useDiskCache) {
            std::vector<std::uint8_t> bBytes;
            mir::Module cachedMir;
            if (readBinaryFile(unoptFile, bBytes) &&
                decodeStandaloneMirModule(bBytes, cachedMir) &&
                cachedMir.name == bName) {
                builtinUnopt.emplace(bName, std::move(cachedMir));
                return;
            }
            std::error_code rmEc;
            std::filesystem::remove(unoptFile, rmEc);
        }
        Compilation probe({SourceFile{
            "$builtin_probe.rscript",
            "module __RsBuiltinProbe;\nimport " + bName + ";\npublic int __p() { return 0; }\n",
        }});
        auto probeRes = probe.build();
        for (auto& m : probeRes.modules) {
            if (m.name != bName) continue;
            if (useDiskCache) {
                (void)writeBinaryAtomic(unoptFile, encodeStandaloneMirModule(m));
            }
            builtinUnopt.emplace(bName, std::move(m));
            break;
        }
    };

    for (const auto& bName : neededBuiltins) {
        loadOrBuildBaseBuiltin(bName);
    }

    // Compile dirty user modules (using .d.rscript stubs for clean non-generic upstream dependencies)
    if (!dirtyModules.empty()) {
        std::unordered_set<std::string> cleanUpstreamNeeded;
        for (const auto& dirtyName : dirtyModules) {
            for (const auto& depName : transitiveDeps[dirtyName]) {
                if (dirtyModules.find(depName) == dirtyModules.end()) {
                    cleanUpstreamNeeded.insert(depName);
                }
            }
        }

        const auto ensureStubText = [&](const std::string& cleanModName) -> std::string {
            const auto& info = graph.modules.at(cleanModName);
            auto& mw = workMap.at(cleanModName);
            if (useDiskCache && options.emitDeclarationStubs && !mw.status.stubCachePath.empty()) {
                std::string cachedStub;
                if (readTextFile(mw.status.stubCachePath, cachedStub) &&
                    cachedStub == info.declarationStub) {
                    return cachedStub;
                }
                if (writeTextAtomic(mw.status.stubCachePath, info.declarationStub)) {
                    ++out.statistics.declarationStubsGenerated;
                }
            }
            return info.declarationStub;
        };

        auto runCompilationSet = [&](bool allowUpstreamStubs, bool& outAnyStubAdded) -> BuildResult {
            outAnyStubAdded = false;
            Compilation compilation;
            for (const auto& modName : graph.topologicalOrder) {
                const auto& info = graph.modules.at(modName);
                if (dirtyModules.find(modName) != dirtyModules.end()) {
                    for (const auto& sf : info.sources) {
                        compilation.addSource(sf);
                    }
                } else if (cleanUpstreamNeeded.find(modName) != cleanUpstreamNeeded.end()) {
                    if (allowUpstreamStubs && !info.hasPublicGenerics) {
                        outAnyStubAdded = true;
                        compilation.addSource(SourceFile{
                            "$stub/" + sanitizeFilename(modName) + ".d.rscript",
                            ensureStubText(modName),
                        });
                    } else {
                        for (const auto& sf : info.sources) {
                            compilation.addSource(sf);
                        }
                    }
                }
            }
            return compilation.build();
        };

        const bool tryStubs = !cleanUpstreamNeeded.empty() && options.emitDeclarationStubs;
        bool stubsActuallyUsed = false;
        BuildResult buildRes = runCompilationSet(tryStubs, stubsActuallyUsed);
        if (stubsActuallyUsed && buildRes.diagnostics.hasErrors()) {
            bool dummyStubFlag = false;
            auto retryRes = runCompilationSet(false, dummyStubFlag);
            if (!retryRes.diagnostics.hasErrors()) {
                stubsActuallyUsed = false;
                buildRes = std::move(retryRes);
            } else {
                buildRes = std::move(retryRes);
            }
        }

        if (buildRes.diagnostics.hasErrors()) {
            out.diagnostics = std::move(buildRes.diagnostics);
            return out;
        }

        if (stubsActuallyUsed) {
            for (const auto& dirtyName : dirtyModules) {
                for (const auto& depName : transitiveDeps[dirtyName]) {
                    if (cleanUpstreamNeeded.find(depName) != cleanUpstreamNeeded.end() &&
                        !graph.modules.at(depName).hasPublicGenerics) {
                        workMap.at(dirtyName).status.usedDeclarationStubs = true;
                        break;
                    }
                }
            }
        }

        // Merge MIR outputs: replace dirty modules and merge any cross-module generic specializations
        // injected into clean upstream user modules or built-in modules (RealScript.Async / RealScript.Resource)
        for (auto& mirMod : buildRes.modules) {
            if (isBuiltinModuleName(mirMod.name) &&
                graph.modules.find(mirMod.name) == graph.modules.end()) {
                ensureBuiltinListed(mirMod.name);
                loadOrBuildBaseBuiltin(mirMod.name);
                auto it = builtinUnopt.find(mirMod.name);
                if (it != builtinUnopt.end()) {
                    (void)mergeAdditiveSpecializations(it->second, mirMod);
                } else {
                    builtinUnopt.emplace(mirMod.name, std::move(mirMod));
                }
                continue;
            }

            auto wIt = workMap.find(mirMod.name);
            if (wIt == workMap.end()) continue;

            if (dirtyModules.find(mirMod.name) != dirtyModules.end()) {
                wIt->second.unoptMir = std::move(mirMod);
                wIt->second.unoptDirty = true;
            } else {
                if (mergeAdditiveSpecializations(wIt->second.unoptMir, mirMod)) {
                    wIt->second.unoptDirty = true;
                }
            }
        }
    }

    // Check if any module's unoptMir transitively references built-in modules not yet listed
    for (const auto& modName : graph.topologicalOrder) {
        const auto& unopt = workMap.at(modName).unoptMir;
        for (const char* bCand : {"RealScript.Math", "RealScript.Fiber", "RealScript.Async", "RealScript.Resource"}) {
            if (mirModuleReferencesBuiltin(unopt, bCand)) {
                ensureBuiltinListed(bCand);
                loadOrBuildBaseBuiltin(bCand);
            }
        }
    }

    // Detect if any cached consumer module references cross-module generic specializations
    // (types or specialized `__` functions/methods) missing from an upstream or built-in module
    // (e.g., when upstream was recompiled standalone after a private edit while consumer stayed cached).
    {
        std::unordered_map<std::string, std::unordered_set<semantic::SymbolId>> definedOwnTypes;
        std::unordered_map<std::string, std::unordered_set<semantic::SymbolId>> definedFuncs;

        const auto indexMir = [&](const mir::Module& m) {
            auto& tSet = definedOwnTypes[m.name];
            auto& fSet = definedFuncs[m.name];
            for (const auto& t : m.types) {
                if (t.moduleName == m.name) tSet.insert(t.id);
            }
            for (const auto& fn : m.functions) {
                fSet.insert(fn.symbolId);
            }
        };
        for (const auto& bName : neededBuiltins) {
            if (const auto it = builtinUnopt.find(bName); it != builtinUnopt.end()) {
                indexMir(it->second);
            }
        }
        for (const auto& modName : graph.topologicalOrder) {
            indexMir(workMap.at(modName).unoptMir);
        }

        std::unordered_set<std::string> consumersNeedingExpansion;
        for (const auto& modName : graph.topologicalOrder) {
            const auto& m = workMap.at(modName).unoptMir;
            bool missing = false;
            for (const auto& t : m.types) {
                if (t.moduleName == modName || t.moduleName.empty()) continue;
                const auto dIt = definedOwnTypes.find(t.moduleName);
                if (dIt != definedOwnTypes.end() && dIt->second.find(t.id) == dIt->second.end()) {
                    missing = true;
                    break;
                }
            }
            if (!missing) {
                for (const auto& fn : m.functions) {
                    for (const auto& blk : fn.blocks) {
                        for (const auto& inst : blk.instructions) {
                            if (inst.symbolId == 0 || inst.symbolName.empty()) continue;
                            const auto sep = inst.symbolName.find("::");
                            if (sep == std::string::npos) continue;
                            const std::string targetMod = inst.symbolName.substr(0, sep);
                            if (targetMod == modName) continue;
                            const std::string targetLocal = inst.symbolName.substr(sep + 2);
                            if (targetLocal.find("__") == std::string::npos) continue;
                            const auto fIt = definedFuncs.find(targetMod);
                            if (fIt != definedFuncs.end() &&
                                fIt->second.find(inst.symbolId) == fIt->second.end()) {
                                missing = true;
                                break;
                            }
                        }
                        if (missing) break;
                    }
                    if (missing) break;
                }
            }
            if (missing) {
                consumersNeedingExpansion.insert(modName);
            }
        }

        if (!consumersNeedingExpansion.empty()) {
            std::unordered_set<std::string> expandSet = consumersNeedingExpansion;
            for (const auto& cName : consumersNeedingExpansion) {
                const auto& deps = transitiveDeps[cName];
                expandSet.insert(deps.begin(), deps.end());
            }
            Compilation expandComp;
            for (const auto& modName : graph.topologicalOrder) {
                if (expandSet.find(modName) != expandSet.end()) {
                    for (const auto& sf : graph.modules.at(modName).sources) {
                        expandComp.addSource(sf);
                    }
                }
            }
            auto expandRes = expandComp.build();
            if (expandRes.diagnostics.hasErrors()) {
                out.diagnostics = std::move(expandRes.diagnostics);
                return out;
            }
            for (auto& expMir : expandRes.modules) {
                if (isBuiltinModuleName(expMir.name) &&
                    graph.modules.find(expMir.name) == graph.modules.end()) {
                    ensureBuiltinListed(expMir.name);
                    loadOrBuildBaseBuiltin(expMir.name);
                    (void)mergeAdditiveSpecializations(builtinUnopt[expMir.name], expMir);
                    continue;
                }
                if (auto wIt = workMap.find(expMir.name); wIt != workMap.end()) {
                    if (mergeAdditiveSpecializations(wIt->second.unoptMir, expMir)) {
                        wIt->second.unoptDirty = true;
                    }
                }
            }
        }
    }

    // Assemble full unoptimized MIR module set in unified order (built-ins + topological user modules),
    // synchronize cross-module TypeSymbols, and run unified cross-module Optimizer when optLevel > 0!
    canonicalizeNeededBuiltins();
    std::vector<std::string> orderedAllNames;
    orderedAllNames.reserve(neededBuiltins.size() + graph.topologicalOrder.size());
    std::vector<mir::Module> allMir;
    allMir.reserve(neededBuiltins.size() + graph.topologicalOrder.size());

    for (const auto& bName : neededBuiltins) {
        const auto it = builtinUnopt.find(bName);
        if (it == builtinUnopt.end()) continue;
        orderedAllNames.push_back(bName);
        allMir.push_back(it->second);
    }
    for (const auto& modName : graph.topologicalOrder) {
        orderedAllNames.push_back(modName);
        allMir.push_back(workMap.at(modName).unoptMir);
    }

    syncMirImportedTypes(allMir);

    if (options.optimizationOptions.level != optimization::Level::None) {
        optimization::Optimizer optimizer;
        auto optResult = optimizer.optimize(
            std::move(allMir),
            out.diagnostics,
            options.optimizationOptions);
        if (out.diagnostics.hasErrors()) return out;
        allMir = std::move(optResult.modules);
        out.optimizationStatistics = optResult.statistics;
        syncMirImportedTypes(allMir);
    }

    // Lower or reuse cached .rsbc per optimized MIR hash, persist module & graph caches
    bytecode::Lowerer lowerer;
    GraphMetaRecord newGraphRec;
    newGraphRec.graphKey = computeGraphKey();
    newGraphRec.moduleOptHashes.reserve(allMir.size());

    std::vector<bytecode::Module> finalBcModules;
    finalBcModules.reserve(allMir.size());

    for (std::size_t i = 0; i < allMir.size(); ++i) {
        auto& optMir = allMir[i];
        const auto& modName = orderedAllNames[i];
        const auto optHash = hashMirModuleContent(optMir);
        newGraphRec.moduleOptHashes.emplace_back(modName, optHash);

        bytecode::Module bcMod;
        mir::Module unusedMir;
        if (!tryLoadOptArtifact(modName, optHash, false, bcMod, unusedMir)) {
            bcMod = lowerer.lower(optMir);
            if (!bytecode::verifyModule(bcMod, out.diagnostics) ||
                out.diagnostics.hasErrors()) {
                return out;
            }
            const auto encodedBytes = encodeCacheBytecodeModule(bcMod);
            bytecode::Module canonicalBc;
            if (decodeCacheBytecodeModule(encodedBytes, canonicalBc, out.diagnostics)) {
                bcMod = std::move(canonicalBc);
            }
            if (out.diagnostics.hasErrors()) return out;
            saveOptArtifact(modName, optHash, bcMod, optMir);
        }

        if (auto wIt = workMap.find(modName); wIt != workMap.end()) {
            auto& mw = wIt->second;
            mw.bcModule = bcMod;
            mw.mirModule = optMir;
            mw.loaded = true;
            if (useDiskCache &&
                (dirtyModules.find(modName) != dirtyModules.end() ||
                 mw.unoptDirty ||
                 mw.loadedOptHash != optHash)) {
                saveCachedModule(
                    mw.metaPath,
                    mw.rsbcPath,
                    mw.rsmirPath,
                    mw.unoptPath,
                    mw.status,
                    optHash,
                    mw.bcModule,
                    mw.mirModule,
                    mw.unoptMir);
                mw.loadedOptHash = optHash;
            }
        }

        finalBcModules.push_back(std::move(bcMod));
    }

    if (useDiskCache) {
        const auto graphPath =
            activeCacheDir / "graphs" / ("graph_" + hex64(newGraphRec.graphKey) + ".gmeta");
        (void)writeBinaryAtomic(graphPath, encodeGraphMeta(newGraphRec));
    }

    for (const auto& modName : graph.topologicalOrder) {
        out.moduleStatuses.push_back(workMap.at(modName).status);
    }
    out.bytecodeModules = std::move(finalBcModules);
    if (options.retainMirModules) {
        out.mirModules = std::move(allMir);
    }

    syncImportedTypeDescriptors(
        out.bytecodeModules,
        options.retainMirModules ? &out.mirModules : nullptr);

    return out;
}

CachedAotBuildResult ModuleBuildCache::buildAot(
    const std::vector<SourceFile>& sources,
    ModuleBuildCacheOptions cacheOptions,
    aot::GenerationOptions aotOptions) const {
    CachedAotBuildResult out;
    if (sources.empty()) return out;

    const std::filesystem::path activeCacheDir =
        cacheOptions.cacheDirectory.empty() ? cacheDirectory_ : cacheOptions.cacheDirectory;
    const bool useDiskCache = !activeCacheDir.empty();

    if (aotOptions.trustedRelease) {
        cacheOptions.flags |= kBuildCacheFlagTrustedRelease;
    }
    if (!aotOptions.emitLineDirectives) {
        cacheOptions.flags |= kBuildCacheFlagNoLineDirectives;
    }

    const auto computeAotKey = [&](const std::vector<std::pair<std::string, std::uint64_t>>& modKeys) {
        std::uint64_t aotKey = 14695981039346656037ull;
        mixU64(aotKey, compilerAndAbiVersionFingerprint());
        mixString(aotKey, aotOptions.programName);
        mixString(aotKey, aotOptions.cppNamespace);
        mixString(aotKey, aotOptions.querySymbol);
        mixU8(aotKey, aotOptions.emitLineDirectives ? 1u : 0u);
        mixU8(aotKey, aotOptions.trustedRelease ? 1u : 0u);
        mixU8(aotKey, aotOptions.enableSharedReferenceGenerics ? 1u : 0u);
        mixU64(aotKey, static_cast<std::uint64_t>(modKeys.size()));
        for (const auto& [mName, mKey] : modKeys) {
            mixString(aotKey, mName);
            mixU64(aotKey, mKey);
        }
        return aotKey;
    };

    // Fast-path check: extract module graph & verify module cache entries + AOT bundle on disk
    if (useDiskCache) {
        auto graph = extractModuleGraph(
            sources, cacheOptions.optimizationOptions, cacheOptions.flags);
        if (!graph.diagnostics.hasErrors()) {
            std::unordered_map<std::string, std::set<std::string>> transitiveDeps;
            for (const auto& modName : graph.topologicalOrder) {
                auto& deps = transitiveDeps[modName];
                for (const auto& imp : graph.modules.at(modName).imports) {
                    if (imp == modName || graph.modules.find(imp) == graph.modules.end()) continue;
                    deps.insert(imp);
                    const auto& upDeps = transitiveDeps[imp];
                    deps.insert(upDeps.begin(), upDeps.end());
                }
            }

            std::uint64_t optHash = 14695981039346656037ull;
            mixOptimizationOptions(optHash, cacheOptions.optimizationOptions);
            mixU64(optHash, cacheOptions.flags);

            bool allModulesIntactOnDisk = true;
            std::vector<ModuleCacheEntryStatus> fastStatuses;
            std::vector<std::pair<std::string, std::uint64_t>> modKeys;
            fastStatuses.reserve(graph.topologicalOrder.size());
            modKeys.reserve(graph.topologicalOrder.size());

            const auto modulesDir = activeCacheDir / "modules";
            const auto stubsDir = activeCacheDir / "stubs";

            for (const auto& modName : graph.topologicalOrder) {
                const auto& info = graph.modules.at(modName);
                ModuleCacheKey keySpec;
                keySpec.moduleName = modName;
                keySpec.implementationFingerprint = info.implementationFingerprint;
                keySpec.optLevel = cacheOptions.optimizationOptions.level;
                keySpec.flags = optHash;

                std::uint64_t depFp = 14695981039346656037ull;
                for (const auto& depName : transitiveDeps[modName]) {
                    const auto depPub = graph.modules.at(depName).publicFingerprint;
                    keySpec.dependencyPublicFingerprints.emplace_back(depName, depPub);
                    mixString(depFp, depName);
                    mixU64(depFp, depPub);
                }

                const auto mKey = keySpec.digest();
                modKeys.emplace_back(modName, mKey);

                const auto safeMod = sanitizeFilename(modName);
                const auto stem = safeMod + "_" + hex64(mKey);
                const auto metaPath = modulesDir / (stem + ".meta");
                const auto rsbcPath = modulesDir / (stem + ".rsbc");
                const auto rsmirPath = modulesDir / (stem + ".rsmir");
                const auto unoptPath = modulesDir / (stem + ".unopt");

                std::vector<std::uint8_t> metaBytes;
                std::vector<std::uint8_t> rsbcBytes;
                std::vector<std::uint8_t> rsmirBytes;
                std::vector<std::uint8_t> unoptBytes;
                ModuleMetaRecord meta;
                if (!readBinaryFile(metaPath, metaBytes) ||
                    !decodeModuleMeta(metaBytes, meta) ||
                    meta.moduleName != modName ||
                    meta.cacheKey != mKey ||
                    !readBinaryFile(rsbcPath, rsbcBytes) ||
                    rsbcBytes.size() != meta.rsbcSize ||
                    checksum64(rsbcBytes) != meta.rsbcChecksum ||
                    !readBinaryFile(rsmirPath, rsmirBytes) ||
                    rsmirBytes.size() != meta.rsmirSize ||
                    checksum64(rsmirBytes) != meta.rsmirChecksum ||
                    !readBinaryFile(unoptPath, unoptBytes) ||
                    unoptBytes.size() != meta.unoptSize ||
                    checksum64(unoptBytes) != meta.unoptChecksum) {
                    allModulesIntactOnDisk = false;
                    break;
                }

                ModuleCacheEntryStatus st;
                st.moduleName = modName;
                st.imports = info.imports;
                st.implementationFingerprint = info.implementationFingerprint;
                st.publicFingerprint = info.publicFingerprint;
                st.dependencyPublicFingerprint = depFp;
                st.cacheKey = mKey;
                st.cacheHit = true;
                st.bytecodeCachePath = rsbcPath;
                st.stubCachePath =
                    stubsDir / (safeMod + "_" + hex64(info.publicFingerprint) + ".d.rscript");
                fastStatuses.push_back(std::move(st));
            }

            if (allModulesIntactOnDisk) {
                const auto aotKey = computeAotKey(modKeys);
                const auto aotPath = activeCacheDir / "aot" / ("aot_" + hex64(aotKey) + ".aotcache");
                std::vector<std::uint8_t> aotBytes;
                AotCacheBundle bundle;
                if (readBinaryFile(aotPath, aotBytes)) {
                    if (decodeAotBundle(aotBytes, bundle) && bundle.aotKey == aotKey) {
                        out.program.header = std::move(bundle.header);
                        out.program.source = std::move(bundle.source);
                        out.program.manifest = std::move(bundle.manifest);
                        out.program.contentHash = bundle.contentHash;
                        out.program.sharedGenericBodyCount =
                            static_cast<std::size_t>(bundle.sharedGenericBodyCount);
                        out.program.foldedGenericFunctionCount =
                            static_cast<std::size_t>(bundle.foldedGenericFunctionCount);
                        out.topologicalOrder = std::move(graph.topologicalOrder);
                        out.moduleStatuses = std::move(fastStatuses);
                        out.statistics.totalModules = out.moduleStatuses.size();
                        out.statistics.cacheHits = out.moduleStatuses.size();
                        out.statistics.cacheMisses = 0;
                        out.statistics.aotOutputSkipped = true;
                        out.aotCacheHit = true;
                        return out;
                    }
                    std::error_code rmEc;
                    std::filesystem::remove(aotPath, rmEc);
                    ++out.statistics.corruptedEntriesHealed;
                }
            }
        }
    }

    cacheOptions.retainMirModules = true;
    auto modRes = build(sources, cacheOptions);
    const auto priorAotHealed = out.statistics.corruptedEntriesHealed;
    out.topologicalOrder = std::move(modRes.topologicalOrder);
    out.moduleStatuses = std::move(modRes.moduleStatuses);
    out.statistics = modRes.statistics;
    out.statistics.corruptedEntriesHealed += priorAotHealed;
    out.optimizationStatistics = modRes.optimizationStatistics;
    out.diagnostics = std::move(modRes.diagnostics);
    if (!out.succeeded()) return out;

    aot::CppGenerator generator;
    out.program = generator.generate(modRes.mirModules, out.diagnostics, aotOptions);
    if (!out.succeeded()) return out;

    if (useDiskCache) {
        std::vector<std::pair<std::string, std::uint64_t>> modKeys;
        modKeys.reserve(out.moduleStatuses.size());
        for (const auto& st : out.moduleStatuses) {
            modKeys.emplace_back(st.moduleName, st.cacheKey);
        }
        const auto aotKey = computeAotKey(modKeys);

        AotCacheBundle bundle;
        bundle.aotKey = aotKey;
        bundle.contentHash = out.program.contentHash;
        bundle.sharedGenericBodyCount = out.program.sharedGenericBodyCount;
        bundle.foldedGenericFunctionCount = out.program.foldedGenericFunctionCount;
        bundle.header = out.program.header;
        bundle.source = out.program.source;
        bundle.manifest = out.program.manifest;
        const auto aotPath = activeCacheDir / "aot" / ("aot_" + hex64(aotKey) + ".aotcache");
        (void)writeBinaryAtomic(aotPath, encodeAotBundle(bundle));
    }

    return out;
}

} // namespace realscript::compiler
