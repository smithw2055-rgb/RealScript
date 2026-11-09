#include "realscript/bytecode/Bytecode.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace realscript::bytecode {

const char* opcodeName(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::LoadParameter: return "param";
    case Opcode::ConstantInt: return "const.int";
    case Opcode::ConstantDouble: return "const.f64";
    case Opcode::ConstantBool: return "const.bool";
    case Opcode::ConstantString: return "const.string";
    case Opcode::ConstantNull: return "const.null";
    case Opcode::LoadLocal: return "load.local";
    case Opcode::StoreLocal: return "store.local";
    case Opcode::ConvertNullToString: return "conv.null.string";
    case Opcode::ConvertNullToObject: return "conv.null.object";
    case Opcode::ConvertNullToArray: return "conv.null.array";
    case Opcode::ConvertIntToLong: return "conv.i32.i64";
    case Opcode::ConvertIntToDouble: return "conv.i32.f64";
    case Opcode::ConvertLongToDouble: return "conv.i64.f64";
    case Opcode::ConvertNumeric: return "conv.numeric";
    case Opcode::IsType: return "is.type";
    case Opcode::AsType: return "as.type";
    case Opcode::ConstantTypeId: return "const.typeid";
    case Opcode::NewObject: return "new.object";
    case Opcode::NewStruct: return "new.struct";
    case Opcode::NewArray: return "new.array";
    case Opcode::CheckNotNull: return "check.notnull";
    case Opcode::ArrayLength: return "array.length";
    case Opcode::LoadElement: return "load.element";
    case Opcode::StoreElement: return "store.element";
    case Opcode::LoadField: return "load.field";
    case Opcode::StoreField: return "store.field";
    case Opcode::LoadStructField: return "load.struct.field";
    case Opcode::StoreStructField: return "store.struct.field";
    case Opcode::Call: return "call";
    case Opcode::NegateInt: return "neg.i32";
    case Opcode::NegateLong: return "neg.i64";
    case Opcode::NegateDouble: return "neg.f64";
    case Opcode::LogicalNot: return "not.bool";
    case Opcode::AddInt: return "add.i32";
    case Opcode::SubtractInt: return "sub.i32";
    case Opcode::MultiplyInt: return "mul.i32";
    case Opcode::DivideInt: return "div.checked.i32";
    case Opcode::RemainderInt: return "rem.checked.i32";
    case Opcode::Equal: return "eq";
    case Opcode::NotEqual: return "ne";
    case Opcode::LessInt: return "lt.i32";
    case Opcode::LessOrEqualInt: return "le.i32";
    case Opcode::GreaterInt: return "gt.i32";
    case Opcode::GreaterOrEqualInt: return "ge.i32";
    case Opcode::AddLong: return "add.i64";
    case Opcode::SubtractLong: return "sub.i64";
    case Opcode::MultiplyLong: return "mul.i64";
    case Opcode::DivideLong: return "div.checked.i64";
    case Opcode::RemainderLong: return "rem.checked.i64";
    case Opcode::LessLong: return "lt.i64";
    case Opcode::LessOrEqualLong: return "le.i64";
    case Opcode::GreaterLong: return "gt.i64";
    case Opcode::GreaterOrEqualLong: return "ge.i64";
    case Opcode::AddDouble: return "add.f64";
    case Opcode::SubtractDouble: return "sub.f64";
    case Opcode::MultiplyDouble: return "mul.f64";
    case Opcode::DivideDouble: return "div.f64";
    case Opcode::LessDouble: return "lt.f64";
    case Opcode::LessOrEqualDouble: return "le.f64";
    case Opcode::GreaterDouble: return "gt.f64";
    case Opcode::GreaterOrEqualDouble: return "ge.f64";
    case Opcode::NewDelegate: return "new.delegate";
    case Opcode::InvokeDelegate: return "invoke.delegate";
    case Opcode::CombineDelegate: return "combine.delegate";
    case Opcode::RemoveDelegate: return "remove.delegate";
    }
    return "unknown";
}

const char* terminatorName(TerminatorKind kind) noexcept {
    switch (kind) {
    case TerminatorKind::None: return "none";
    case TerminatorKind::Jump: return "jmp";
    case TerminatorKind::Branch: return "br";
    case TerminatorKind::ReturnValue: return "ret";
    case TerminatorKind::ReturnVoid: return "ret.void";
    case TerminatorKind::Throw: return "throw";
    }
    return "unknown";
}

namespace {

[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct RegisterSliceView {
    const Register* data = nullptr;
    std::size_t size = 0;

    friend bool operator==(
        const RegisterSliceView& left,
        const RegisterSliceView& right) noexcept {
        if (left.size != right.size) return false;
        if (left.data == right.data || left.size == 0) return true;
        return std::memcmp(
                   left.data,
                   right.data,
                   left.size * sizeof(Register)) == 0;
    }
};

struct RegisterSliceViewHash {
    std::size_t operator()(const RegisterSliceView& view) const noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        for (std::size_t i = 0; i < view.size; ++i) {
            h ^= view.data[i];
            h *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

/**
 * Two-level lock-free paged RegisterSlicePool:
 * - Supports the full 20-bit slice ID space (1,048,575 non-empty slices)
 *   encoded in SmallRegisterVector::sizeAndSlice_.
 * - Readers (`data(id)`) execute wait-free via two atomic acquire loads
 *   without acquiring any mutex.
 * - Writers (`intern`) deduplicate using zero-copy `RegisterSliceView` keys
 *   pointing directly to pinned page storage.
 */
struct RegisterSlicePool {
    static constexpr std::uint32_t kPageShift = 10u;
    static constexpr std::uint32_t kPageSize = 1u << kPageShift; // 1,024 slices/page
    static constexpr std::uint32_t kPageMask = kPageSize - 1u;
    static constexpr std::uint32_t kMaxPages = 1024u;            // 1,048,576 total slots (20-bit)
    static constexpr std::uint32_t kMaxSlices = kMaxPages * kPageSize;

    struct SlicePage {
        std::atomic<const Register*> published[kPageSize]{};
        std::vector<Register> storage[kPageSize];
    };

    std::mutex writeMutex;
    std::atomic<SlicePage*> pages[kMaxPages]{};
    std::vector<std::unique_ptr<SlicePage>> ownedPages;
    std::unordered_map<RegisterSliceView, std::uint32_t, RegisterSliceViewHash> indexBySlice;
    std::uint32_t nextId = 1u; // id 0 is reserved for empty / inline-1

    RegisterSlicePool() = default;

    std::uint32_t intern(const Register* rawData, std::size_t count) {
        if (!rawData || count == 0u) return 0u;
        const RegisterSliceView probe{rawData, count};
        std::lock_guard<std::mutex> lock(writeMutex);
        const auto found = indexBySlice.find(probe);
        if (found != indexBySlice.end()) return found->second;
        if (nextId >= kMaxSlices) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            throw std::overflow_error(
                "bytecode register slice pool exceeded 20-bit capacity");
#else
            std::abort();
#endif
        }
        const std::uint32_t id = nextId;
        const std::uint32_t pageIdx = id >> kPageShift;
        const std::uint32_t offset = id & kPageMask;

        SlicePage* page = pages[pageIdx].load(std::memory_order_relaxed);
        if (!page) {
            auto newPage = std::make_unique<SlicePage>();
            page = newPage.get();
            ownedPages.push_back(std::move(newPage));
            pages[pageIdx].store(page, std::memory_order_release);
        }

        page->storage[offset].assign(rawData, rawData + count);
        const Register* pinnedPtr = page->storage[offset].data();
        const std::size_t pinnedSize = page->storage[offset].size();
        indexBySlice.emplace(RegisterSliceView{pinnedPtr, pinnedSize}, id);
        page->published[offset].store(pinnedPtr, std::memory_order_release);
        ++nextId;
        return id;
    }

    [[nodiscard]] std::size_t count() const noexcept {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(writeMutex));
        return nextId > 0u ? static_cast<std::size_t>(nextId - 1u) : 0u;
    }

    void clear() noexcept {
        std::lock_guard<std::mutex> lock(writeMutex);
        indexBySlice.clear();
        for (std::uint32_t i = 0; i < kMaxPages; ++i) {
            pages[i].store(nullptr, std::memory_order_release);
        }
        ownedPages.clear();
        nextId = 1u;
    }

    [[nodiscard]] const Register* data(std::uint32_t id) const noexcept {
        static const Register kZero = 0;
        if (id == 0u || id >= kMaxSlices) return &kZero;
        const SlicePage* page = pages[id >> kPageShift].load(std::memory_order_acquire);
        if (!page) return &kZero;
        const Register* ptr = page->published[id & kPageMask].load(std::memory_order_acquire);
        return ptr ? ptr : &kZero;
    }
};

RegisterSlicePool& registerSlicePool() {
    static RegisterSlicePool pool;
    return pool;
}

/**
 * 32-bit Sharded & Lock-Free Paged BytecodeStringPool:
 * - Eliminates the legacy 16-bit (65,535) capacity ceiling; supports up to
 *   67,108,864 (64M) unique bytecode strings across 16 shards.
 * - Readers (`get(id)`) execute wait-free via atomic acquire page/slot loads
 *   with zero mutex contention.
 * - Writers (`intern(text)`) shard lock contention 16-way by FNV-1a hash.
 */
struct BytecodeStringPool {
    static constexpr std::uint32_t kShardBits = 4u;
    static constexpr std::uint32_t kShardCount = 1u << kShardBits; // 16 shards
    static constexpr std::uint32_t kShardMask = kShardCount - 1u;
    static constexpr std::uint32_t kLocalBits = 28u;
    static constexpr std::uint32_t kLocalMask = (1u << kLocalBits) - 1u;

    static constexpr std::uint32_t kPageShift = 10u;
    static constexpr std::uint32_t kPageSize = 1u << kPageShift;   // 1,024 strings/page
    static constexpr std::uint32_t kPageMask = kPageSize - 1u;
    static constexpr std::uint32_t kMaxPagesPerShard = 4096u;      // 4,194,304 strings/shard (64M total)
    static constexpr std::uint32_t kMaxSlotsPerShard = kMaxPagesPerShard * kPageSize;

    struct StringPage {
        std::atomic<const std::string*> published[kPageSize]{};
        std::string storage[kPageSize];
    };

    struct alignas(64) Shard {
        std::mutex writeMutex;
        std::atomic<StringPage*> pages[kMaxPagesPerShard]{};
        std::vector<std::unique_ptr<StringPage>> ownedPages;
        std::unordered_map<std::string_view, std::uint32_t> indexByView;
        std::uint32_t nextSlot = 0u;
    };

    std::string emptyString_{};
    Shard shards_[kShardCount]{};

    BytecodeStringPool() = default;

    std::uint32_t intern(std::string_view text) {
        if (text.empty()) return 0u;
        const std::uint64_t hash = fnv1a64(text);
        const std::uint32_t shardIdx =
            static_cast<std::uint32_t>(hash) & kShardMask;
        Shard& shard = shards_[shardIdx];

        std::lock_guard<std::mutex> lock(shard.writeMutex);
        const auto found = shard.indexByView.find(text);
        if (found != shard.indexByView.end()) return found->second;
        if (shard.nextSlot >= kMaxSlotsPerShard) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            throw std::overflow_error(
                "bytecode string pool exceeded 64M capacity");
#else
            std::abort();
#endif
        }

        const std::uint32_t localSlot = shard.nextSlot;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        const std::uint32_t offset = localSlot & kPageMask;

        StringPage* page = shard.pages[pageIdx].load(std::memory_order_relaxed);
        if (!page) {
            auto newPage = std::make_unique<StringPage>();
            page = newPage.get();
            shard.ownedPages.push_back(std::move(newPage));
            shard.pages[pageIdx].store(page, std::memory_order_release);
        }

        page->storage[offset].assign(text.data(), text.size());
        const std::string* pinnedStr = &page->storage[offset];
        const std::uint32_t id = (shardIdx << kLocalBits) | (localSlot + 1u);
        shard.indexByView.emplace(std::string_view(*pinnedStr), id);
        page->published[offset].store(pinnedStr, std::memory_order_release);
        ++shard.nextSlot;
        return id;
    }

    [[nodiscard]] const std::string& get(std::uint32_t id) const noexcept {
        if (id == 0u) return emptyString_;
        const std::uint32_t rawLocal = id & kLocalMask;
        if (rawLocal == 0u) return emptyString_;
        const std::uint32_t shardIdx = (id >> kLocalBits) & kShardMask;
        const std::uint32_t localSlot = rawLocal - 1u;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx >= kMaxPagesPerShard) return emptyString_;

        const StringPage* page =
            shards_[shardIdx].pages[pageIdx].load(std::memory_order_acquire);
        if (!page) return emptyString_;
        const std::string* strPtr =
            page->published[localSlot & kPageMask].load(std::memory_order_acquire);
        return strPtr ? *strPtr : emptyString_;
    }

    [[nodiscard]] std::size_t count() const noexcept {
        std::size_t total = 0;
        for (std::uint32_t s = 0; s < kShardCount; ++s) {
            std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(shards_[s].writeMutex));
            total += shards_[s].nextSlot;
        }
        return total;
    }

    void clear() noexcept {
        for (std::uint32_t s = 0; s < kShardCount; ++s) {
            std::lock_guard<std::mutex> lock(shards_[s].writeMutex);
            shards_[s].indexByView.clear();
            for (std::uint32_t p = 0; p < kMaxPagesPerShard; ++p) {
                shards_[s].pages[p].store(nullptr, std::memory_order_release);
            }
            shards_[s].ownedPages.clear();
            shards_[s].nextSlot = 0u;
        }
    }
};

BytecodeStringPool& bytecodeStringPool() {
    static BytecodeStringPool pool;
    return pool;
}

} // namespace

const Register* SmallRegisterVector::sliceData(std::uint32_t id) noexcept {
    return registerSlicePool().data(id);
}

std::uint32_t SmallRegisterVector::internSlice(
    const Register* values,
    std::uint16_t count) {
    return registerSlicePool().intern(values, count);
}

std::uint32_t SmallRegisterVector::appendToSlice(
    std::uint32_t id,
    Register firstInline,
    std::uint16_t currentSize,
    Register value) {
    const std::size_t nextSize = static_cast<std::size_t>(currentSize) + 1u;
    if (nextSize <= 64u) {
        Register stackBuf[64];
        if (id == 0u) {
            if (currentSize == 1u) stackBuf[0] = firstInline;
        } else {
            const auto* src = sliceData(id);
            std::memcpy(stackBuf, src, currentSize * sizeof(Register));
        }
        stackBuf[currentSize] = value;
        return registerSlicePool().intern(stackBuf, nextSize);
    }
    std::vector<Register> next;
    next.reserve(nextSize);
    if (id == 0u) {
        if (currentSize == 1u) next.push_back(firstInline);
    } else {
        const auto* src = sliceData(id);
        next.insert(next.end(), src, src + currentSize);
    }
    next.push_back(value);
    return registerSlicePool().intern(next.data(), next.size());
}

std::size_t SmallRegisterVector::internedSliceCount() noexcept {
    return registerSlicePool().count();
}

void SmallRegisterVector::clearSlicePool() noexcept {
    registerSlicePool().clear();
}

BytecodeString::BytecodeString(const std::string& value)
    : poolId_(bytecodeStringPool().intern(value)) {}

BytecodeString::BytecodeString(std::string&& value)
    : poolId_(bytecodeStringPool().intern(value)) {}

BytecodeString::BytecodeString(std::string_view value)
    : poolId_(bytecodeStringPool().intern(value)) {}

BytecodeString::BytecodeString(const char* value)
    : poolId_(bytecodeStringPool().intern(value ? value : "")) {}

const std::string& BytecodeString::str() const noexcept {
    return bytecodeStringPool().get(poolId_);
}

std::size_t BytecodeString::internedStringCount() noexcept {
    return bytecodeStringPool().count();
}

void BytecodeString::clearStringPool() noexcept {
    bytecodeStringPool().clear();
}

} // namespace realscript::bytecode
