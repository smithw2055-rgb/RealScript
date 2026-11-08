#include "realscript/bytecode/Bytecode.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>


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

struct RegisterVecHash {
    std::size_t operator()(const std::vector<Register>& v) const noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        for (const auto r : v) {
            h ^= r;
            h *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

struct RegisterSlicePool {
    std::mutex mutex;
    std::deque<std::vector<Register>> entries;
    std::unordered_map<std::vector<Register>, std::uint32_t, RegisterVecHash> indexByVec;

    RegisterSlicePool() {
        entries.emplace_back(); // id 0 = empty
    }

    std::uint32_t intern(std::vector<Register>&& vec) {
        if (vec.empty()) return 0u;
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = indexByVec.find(vec);
        if (found != indexByVec.end()) return found->second;
        if (entries.size() >= 0xFFFFFu) {
            throw std::overflow_error("bytecode register slice pool exceeded 20-bit capacity");
        }
        const auto id = static_cast<std::uint32_t>(entries.size());
        entries.push_back(std::move(vec));
        indexByVec.emplace(entries.back(), id);
        return id;
    }

    const Register* data(std::uint32_t id) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (id == 0u || id >= entries.size() || entries[id].empty()) {
            static const Register kZero = 0;
            return &kZero;
        }
        return entries[id].data();
    }
};

RegisterSlicePool& registerSlicePool() {
    static RegisterSlicePool pool;
    return pool;
}

struct BytecodeStringPool {
    std::mutex mutex;
    std::deque<std::string> entries;
    std::unordered_map<std::string_view, std::uint16_t> indexByView;

    BytecodeStringPool() {
        entries.emplace_back("");
        indexByView.emplace(entries.front(), static_cast<std::uint16_t>(0u));
    }

    std::uint16_t intern(std::string_view text) {
        if (text.empty()) return 0u;
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = indexByView.find(text);
        if (found != indexByView.end()) return found->second;
        if (entries.size() >= 0xFFFFu) {
            throw std::overflow_error("bytecode string pool exceeded 16-bit capacity");
        }
        const auto id = static_cast<std::uint16_t>(entries.size());
        entries.emplace_back(text);
        indexByView.emplace(std::string_view(entries.back()), id);
        return id;
    }

    const std::string& get(std::uint16_t id) noexcept {
        if (id == 0u) return entries[0];
        std::lock_guard<std::mutex> lock(mutex);
        return id < entries.size() ? entries[id] : entries[0];
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

std::uint32_t SmallRegisterVector::appendToSlice(
    std::uint32_t id,
    Register firstInline,
    std::uint16_t currentSize,
    Register value) {
    std::vector<Register> next;
    next.reserve(static_cast<std::size_t>(currentSize) + 1u);
    if (id == 0u) {
        if (currentSize == 1u) next.push_back(firstInline);
    } else {
        const auto* src = sliceData(id);
        for (std::uint16_t i = 0; i < currentSize; ++i) next.push_back(src[i]);
    }
    next.push_back(value);
    return registerSlicePool().intern(std::move(next));
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

} // namespace realscript::bytecode
