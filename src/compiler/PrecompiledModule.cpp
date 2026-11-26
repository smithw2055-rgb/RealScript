#include "realscript/compiler/PrecompiledModule.h"

#include "realscript/Version.h"
#include "realscript/bytecode/Lz4Codec.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace realscript::compiler {
namespace {

constexpr std::uint32_t kFormatVersionV3 = 3u;
constexpr std::uint8_t kMagic[4] = {'R', 'S', 'F', 'P'};
constexpr std::uint32_t kHeaderSizeV2 = 72u;
constexpr std::size_t kTypeChunkTargetBytes = 16384u;
constexpr std::size_t kMaxLazyTypeChunks = 8u;

constexpr std::uint8_t kDirFlagExtensionOwner = 0x01u;
constexpr std::uint8_t kDirFlagInterfaceType = 0x02u;

class StringInterner {
public:
    std::uint32_t add(const std::string& s) {
        if (strings_.empty()) {
            strings_.emplace_back();
            indices_[""] = 0u;
        }
        if (s.empty()) {
            return 0u;
        }
        const auto [it, inserted] = indices_.emplace(
            s, static_cast<std::uint32_t>(strings_.size()));
        if (inserted) {
            strings_.push_back(s);
        }
        return it->second;
    }

    [[nodiscard]] std::uint32_t indexOf(const std::string& s) const noexcept {
        if (s.empty()) return 0u;
        const auto it = indices_.find(s);
        return it == indices_.end() ? 0u : it->second;
    }

    [[nodiscard]] const std::vector<std::string>& strings() const noexcept {
        return strings_;
    }

private:
    std::vector<std::string> strings_;
    std::unordered_map<std::string, std::uint32_t> indices_;
};

class Writer {
public:
    void u8(std::uint8_t v) { data_.push_back(v); }
    void boolByte(bool v) { u8(v ? 1u : 0u); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xffu));
        u8(static_cast<std::uint8_t>((v >> 8u) & 0xffu));
    }
    void u32(std::uint32_t v) {
        for (int s = 0; s < 32; s += 8) u8(static_cast<std::uint8_t>(v >> s));
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void u64(std::uint64_t v) {
        for (int s = 0; s < 64; s += 8) u8(static_cast<std::uint8_t>(v >> s));
    }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void f64(double v) {
        std::uint64_t bits = 0u;
        std::memcpy(&bits, &v, sizeof(bits));
        u64(bits);
    }
    void rawStr(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        if (!s.empty()) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
            data_.insert(data_.end(), p, p + s.size());
        }
    }
    void bytes(const void* ptr, std::size_t len) {
        if (len == 0u || !ptr) return;
        const auto* p = static_cast<const std::uint8_t*>(ptr);
        data_.insert(data_.end(), p, p + len);
    }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(data_); }

private:
    std::vector<std::uint8_t> data_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept
        : ptr_(data), end_(data ? (data + size) : nullptr) {}

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return valid_ && ptr_ <= end_ ? static_cast<std::size_t>(end_ - ptr_) : 0u;
    }

    std::uint8_t u8() noexcept {
        if (!require(1u)) return 0u;
        return *ptr_++;
    }
    bool boolByte() noexcept {
        const std::uint8_t b = u8();
        if (b > 1u) valid_ = false;
        return b != 0u;
    }
    std::uint16_t u16() noexcept {
        if (!require(2u)) return 0u;
        const auto b0 = static_cast<std::uint16_t>(*ptr_++);
        const auto b1 = static_cast<std::uint16_t>(*ptr_++);
        return static_cast<std::uint16_t>(b0 | (b1 << 8u));
    }
    std::uint32_t u32() noexcept {
        if (!require(4u)) return 0u;
        std::uint32_t v = 0u;
        std::memcpy(&v, ptr_, 4u);
        ptr_ += 4u;
        return v;
    }
    std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() noexcept {
        if (!require(8u)) return 0u;
        std::uint64_t v = 0u;
        std::memcpy(&v, ptr_, 8u);
        ptr_ += 8u;
        return v;
    }
    std::int64_t i64() noexcept { return static_cast<std::int64_t>(u64()); }
    double f64() noexcept {
        const std::uint64_t bits = u64();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::string rawStr() {
        const std::uint32_t len = u32();
        if (!require(len) || len == 0u) return {};
        const auto* begin = reinterpret_cast<const char*>(ptr_);
        ptr_ += len;
        return std::string(begin, len);
    }
    std::string strFromTable(const std::vector<std::string>& table) {
        const std::uint32_t idx = u32();
        if (idx >= table.size()) {
            valid_ = false;
            return {};
        }
        return table[idx];
    }

private:
    bool require(std::size_t n) noexcept {
        if (!valid_ || n > static_cast<std::size_t>(end_ - ptr_)) {
            valid_ = false;
            return false;
        }
        return true;
    }

    const std::uint8_t* ptr_ = nullptr;
    const std::uint8_t* end_ = nullptr;
    bool valid_ = true;
};

[[nodiscard]] bool sameTokenValue(
    const syntax::TokenValue& a,
    const syntax::TokenValue& b) noexcept {
    if (a.index() != b.index()) return false;
    switch (a.index()) {
    case 0: return true;
    case 1: return std::get<std::int64_t>(a) == std::get<std::int64_t>(b);
    case 2: return std::get<double>(a) == std::get<double>(b);
    case 3: return std::get<bool>(a) == std::get<bool>(b);
    case 4: return std::get<std::string>(a) == std::get<std::string>(b);
    default: return false;
    }
}

[[nodiscard]] bool sameSemanticVariable(
    const semantic::VariableSymbol& a,
    const semantic::VariableSymbol& b) noexcept {
    return a.name == b.name &&
        a.type == b.type &&
        a.typeName == b.typeName &&
        a.storageType == b.storageType &&
        a.storageTypeName == b.storageTypeName &&
        a.modifier == b.modifier &&
        a.paramsArray == b.paramsArray &&
        a.hasDefaultValue == b.hasDefaultValue &&
        sameTokenValue(a.defaultValue, b.defaultValue) &&
        a.defaultValueType == b.defaultValueType &&
        a.index == b.index &&
        a.parameter == b.parameter &&
        a.isConst == b.isConst &&
        a.isUsingResource == b.isUsingResource &&
        a.hasConstantValue == b.hasConstantValue &&
        sameTokenValue(a.constantValue, b.constantValue) &&
        a.id == b.id;
}

[[nodiscard]] bool sameSemanticField(
    const semantic::FieldSymbol& a,
    const semantic::FieldSymbol& b) noexcept {
    return a.name == b.name &&
        a.accessibility == b.accessibility &&
        a.declaringTypeId == b.declaringTypeId &&
        a.declaringTypeName == b.declaringTypeName &&
        a.type == b.type &&
        a.typeName == b.typeName &&
        a.index == b.index &&
        a.synthetic == b.synthetic &&
        a.staticField == b.staticField &&
        a.constField == b.constField &&
        a.readonlyField == b.readonlyField &&
        a.isUsingResource == b.isUsingResource &&
        a.hasConstantValue == b.hasConstantValue &&
        sameTokenValue(a.constantValue, b.constantValue) &&
        a.fieldTag == b.fieldTag &&
        a.formerlySerializedAs == b.formerlySerializedAs &&
        a.id == b.id;
}

[[nodiscard]] bool sameSemanticFunction(
    const semantic::FunctionSymbol& a,
    const semantic::FunctionSymbol& b) noexcept {
    if (a.id != b.id ||
        a.accessibility != b.accessibility ||
        a.declaringTypeId != b.declaringTypeId ||
        a.declaringTypeName != b.declaringTypeName ||
        a.virtualMethod != b.virtualMethod ||
        a.overrideMethod != b.overrideMethod ||
        a.abstractMethod != b.abstractMethod ||
        a.sealedMethod != b.sealedMethod ||
        a.virtualSlot != b.virtualSlot ||
        a.interfaceMethod != b.interfaceMethod ||
        a.interfaceSlot != b.interfaceSlot ||
        a.moduleName != b.moduleName ||
        a.name != b.name ||
        a.ownerTypeName != b.ownerTypeName ||
        a.ownerTypeId != b.ownerTypeId ||
        a.returnType != b.returnType ||
        a.returnTypeName != b.returnTypeName ||
        a.returnModifier != b.returnModifier ||
        a.storageReturnType != b.storageReturnType ||
        a.storageReturnTypeName != b.storageReturnTypeName ||
        a.method != b.method ||
        a.staticMethod != b.staticMethod ||
        a.extensionMethod != b.extensionMethod ||
        a.constructor != b.constructor ||
        a.propertyGetter != b.propertyGetter ||
        a.propertySetter != b.propertySetter ||
        a.synthetic != b.synthetic ||
        a.asyncFunction != b.asyncFunction ||
        a.deterministic != b.deterministic ||
        a.nonDeterministic != b.nonDeterministic ||
        a.parameters.size() != b.parameters.size()) {
        return false;
    }
    for (std::size_t i = 0u; i < a.parameters.size(); ++i) {
        if (!sameSemanticVariable(a.parameters[i], b.parameters[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool sameSemanticProperty(
    const semantic::PropertySymbol& a,
    const semantic::PropertySymbol& b) noexcept {
    if (a.name != b.name ||
        a.accessibility != b.accessibility ||
        a.declaringTypeId != b.declaringTypeId ||
        a.declaringTypeName != b.declaringTypeName ||
        a.type != b.type ||
        a.typeName != b.typeName ||
        a.staticProperty != b.staticProperty ||
        a.backingFieldIndex != b.backingFieldIndex ||
        a.fieldTag != b.fieldTag ||
        a.formerlySerializedAs != b.formerlySerializedAs ||
        a.id != b.id ||
        a.getter.has_value() != b.getter.has_value() ||
        a.setter.has_value() != b.setter.has_value()) {
        return false;
    }
    if (a.getter && !sameSemanticFunction(*a.getter, *b.getter)) return false;
    if (a.setter && !sameSemanticFunction(*a.setter, *b.setter)) return false;
    return true;
}

[[nodiscard]] bool sameSemanticEvent(
    const semantic::EventSymbol& a,
    const semantic::EventSymbol& b) noexcept {
    if (a.id != b.id ||
        a.accessibility != b.accessibility ||
        a.declaringTypeId != b.declaringTypeId ||
        a.declaringTypeName != b.declaringTypeName ||
        a.name != b.name ||
        a.delegateName != b.delegateName ||
        !sameSemanticField(a.storageField, b.storageField) ||
        a.addAccessor.has_value() != b.addAccessor.has_value() ||
        a.removeAccessor.has_value() != b.removeAccessor.has_value() ||
        a.parameters.size() != b.parameters.size() ||
        a.handlers.size() != b.handlers.size() ||
        a.subscriptions.size() != b.subscriptions.size()) {
        return false;
    }
    if (a.addAccessor && !sameSemanticFunction(*a.addAccessor, *b.addAccessor)) return false;
    if (a.removeAccessor && !sameSemanticFunction(*a.removeAccessor, *b.removeAccessor)) return false;
    for (std::size_t i = 0u; i < a.parameters.size(); ++i) {
        if (!sameSemanticVariable(a.parameters[i], b.parameters[i])) return false;
    }
    for (std::size_t i = 0u; i < a.handlers.size(); ++i) {
        if (!sameSemanticFunction(a.handlers[i].function, b.handlers[i].function) ||
            !sameSemanticField(a.handlers[i].enabledField, b.handlers[i].enabledField)) {
            return false;
        }
    }
    for (std::size_t i = 0u; i < a.subscriptions.size(); ++i) {
        if (a.subscriptions[i].enabled != b.subscriptions[i].enabled ||
            !sameSemanticField(a.subscriptions[i].enabledField, b.subscriptions[i].enabledField)) {
            return false;
        }
    }
    return true;
}

void internTokenValue(StringInterner& interner, const syntax::TokenValue& val) {
    if (const auto* s = std::get_if<std::string>(&val)) {
        interner.add(*s);
    }
}

void writeTokenValue(Writer& w, const StringInterner& interner, const syntax::TokenValue& val) {
    const std::uint8_t idx = static_cast<std::uint8_t>(val.index());
    w.u8(idx);
    switch (idx) {
    case 0: break;
    case 1: w.i64(std::get<std::int64_t>(val)); break;
    case 2: w.f64(std::get<double>(val)); break;
    case 3: w.boolByte(std::get<bool>(val)); break;
    case 4: w.u32(interner.indexOf(std::get<std::string>(val))); break;
    default: break;
    }
}

syntax::TokenValue readTokenValue(Reader& r, const std::vector<std::string>& strings, bool& ok) {
    const std::uint8_t kind = r.u8();
    switch (kind) {
    case 0: return std::monostate{};
    case 1: return r.i64();
    case 2: return r.f64();
    case 3: return r.boolByte();
    case 4: return r.strFromTable(strings);
    default:
        ok = false;
        return std::monostate{};
    }
}

void internVariable(StringInterner& s, const semantic::VariableSymbol& v) {
    s.add(v.name);
    s.add(v.typeName);
    s.add(v.storageTypeName);
    internTokenValue(s, v.defaultValue);
    internTokenValue(s, v.constantValue);
}

void writeVariableCompact(Writer& w, const StringInterner& s, const semantic::VariableSymbol& v) {
    w.u32(s.indexOf(v.name));
    w.u8(static_cast<std::uint8_t>(v.type));
    w.u32(s.indexOf(v.typeName));
    w.u8(static_cast<std::uint8_t>(v.storageType));
    w.u32(s.indexOf(v.storageTypeName));
    w.u8(static_cast<std::uint8_t>(v.modifier));
    std::uint8_t vFlags = 0u;
    if (v.paramsArray) vFlags |= 0x01u;
    if (v.hasDefaultValue) vFlags |= 0x02u;
    if (v.parameter) vFlags |= 0x04u;
    if (v.isConst) vFlags |= 0x08u;
    if (v.isUsingResource) vFlags |= 0x10u;
    if (v.hasConstantValue) vFlags |= 0x20u;
    w.u8(vFlags);
    if (v.hasDefaultValue) {
        writeTokenValue(w, s, v.defaultValue);
        w.u8(static_cast<std::uint8_t>(v.defaultValueType));
    }
    if (v.hasConstantValue) {
        writeTokenValue(w, s, v.constantValue);
    }
    w.u32(static_cast<std::uint32_t>(v.index));
    w.u64(v.id);
}

bool readVariableCompact(
    Reader& r,
    const std::vector<std::string>& strings,
    semantic::VariableSymbol& v) {
    v.name = r.strFromTable(strings);
    v.type = static_cast<semantic::PrimitiveType>(r.u8());
    v.typeName = r.strFromTable(strings);
    v.storageType = static_cast<semantic::PrimitiveType>(r.u8());
    v.storageTypeName = r.strFromTable(strings);
    v.modifier = static_cast<semantic::ParameterModifier>(r.u8());
    const std::uint8_t vFlags = r.u8();
    v.paramsArray = (vFlags & 0x01u) != 0u;
    v.hasDefaultValue = (vFlags & 0x02u) != 0u;
    v.parameter = (vFlags & 0x04u) != 0u;
    v.isConst = (vFlags & 0x08u) != 0u;
    v.isUsingResource = (vFlags & 0x10u) != 0u;
    v.hasConstantValue = (vFlags & 0x20u) != 0u;
    bool ok = true;
    if (v.hasDefaultValue) {
        v.defaultValue = readTokenValue(r, strings, ok);
        v.defaultValueType = static_cast<semantic::PrimitiveType>(r.u8());
    } else {
        v.defaultValue = std::monostate{};
        v.defaultValueType = semantic::PrimitiveType::Error;
    }
    if (v.hasConstantValue) {
        v.constantValue = readTokenValue(r, strings, ok);
    } else {
        v.constantValue = std::monostate{};
    }
    v.index = static_cast<std::size_t>(r.u32());
    v.declarationSpan = {};
    v.scopeSpan = {};
    v.id = r.u64();
    return ok && r.valid();
}

void internField(StringInterner& s, const semantic::FieldSymbol& f) {
    s.add(f.name);
    s.add(f.declaringTypeName);
    s.add(f.typeName);
    internTokenValue(s, f.constantValue);
    for (const auto& alias : f.formerlySerializedAs) s.add(alias);
}

void writeFieldCompact(Writer& w, const StringInterner& s, const semantic::FieldSymbol& f) {
    w.u32(s.indexOf(f.name));
    w.u8(static_cast<std::uint8_t>(f.accessibility));
    w.u64(f.declaringTypeId);
    w.u32(s.indexOf(f.declaringTypeName));
    w.u8(static_cast<std::uint8_t>(f.type));
    w.u32(s.indexOf(f.typeName));
    w.u32(static_cast<std::uint32_t>(f.index));
    std::uint8_t fFlags = 0u;
    if (f.synthetic) fFlags |= 0x01u;
    if (f.staticField) fFlags |= 0x02u;
    if (f.constField) fFlags |= 0x04u;
    if (f.readonlyField) fFlags |= 0x08u;
    if (f.isUsingResource) fFlags |= 0x10u;
    if (f.hasConstantValue) fFlags |= 0x20u;
    if (f.fieldTag.has_value()) fFlags |= 0x40u;
    w.u8(fFlags);
    if (f.hasConstantValue) writeTokenValue(w, s, f.constantValue);
    if (f.fieldTag) w.i32(*f.fieldTag);
    w.u32(static_cast<std::uint32_t>(f.formerlySerializedAs.size()));
    for (const auto& alias : f.formerlySerializedAs) w.u32(s.indexOf(alias));
    w.u64(f.id);
}

bool readFieldCompact(
    Reader& r,
    const std::vector<std::string>& strings,
    semantic::FieldSymbol& f) {
    f.name = r.strFromTable(strings);
    f.accessibility = static_cast<semantic::Accessibility>(r.u8());
    f.declaringTypeId = r.u64();
    f.declaringTypeName = r.strFromTable(strings);
    f.type = static_cast<semantic::PrimitiveType>(r.u8());
    f.typeName = r.strFromTable(strings);
    f.index = static_cast<std::size_t>(r.u32());
    const std::uint8_t fFlags = r.u8();
    f.synthetic = (fFlags & 0x01u) != 0u;
    f.staticField = (fFlags & 0x02u) != 0u;
    f.constField = (fFlags & 0x04u) != 0u;
    f.readonlyField = (fFlags & 0x08u) != 0u;
    f.isUsingResource = (fFlags & 0x10u) != 0u;
    f.hasConstantValue = (fFlags & 0x20u) != 0u;
    const bool hasTag = (fFlags & 0x40u) != 0u;
    bool ok = true;
    if (f.hasConstantValue) {
        f.constantValue = readTokenValue(r, strings, ok);
    } else {
        f.constantValue = std::monostate{};
    }
    f.initializerSyntax = nullptr;
    f.sourceName.clear();
    f.declarationSpan = {};
    if (hasTag) f.fieldTag = r.i32();
    else f.fieldTag.reset();
    const std::uint32_t aliasCount = r.u32();
    if (!r.valid() || aliasCount > r.remaining() / 4u) return false;
    f.formerlySerializedAs.reserve(aliasCount);
    for (std::uint32_t i = 0u; i < aliasCount; ++i) {
        f.formerlySerializedAs.push_back(r.strFromTable(strings));
    }
    f.id = r.u64();
    return ok && r.valid();
}

void internFunction(StringInterner& s, const semantic::FunctionSymbol& fn) {
    s.add(fn.declaringTypeName);
    s.add(fn.moduleName);
    s.add(fn.name);
    s.add(fn.ownerTypeName);
    s.add(fn.returnTypeName);
    s.add(fn.storageReturnTypeName);
    for (const auto& p : fn.parameters) internVariable(s, p);
}

void writeFunctionCompact(Writer& w, const StringInterner& s, const semantic::FunctionSymbol& fn) {
    w.u64(fn.id);
    w.u8(static_cast<std::uint8_t>(fn.accessibility));
    w.u64(fn.declaringTypeId);
    w.u32(s.indexOf(fn.declaringTypeName));
    std::uint16_t fnFlags = 0u;
    if (fn.virtualMethod) fnFlags |= (1u << 0u);
    if (fn.overrideMethod) fnFlags |= (1u << 1u);
    if (fn.abstractMethod) fnFlags |= (1u << 2u);
    if (fn.sealedMethod) fnFlags |= (1u << 3u);
    if (fn.interfaceMethod) fnFlags |= (1u << 4u);
    if (fn.method) fnFlags |= (1u << 5u);
    if (fn.staticMethod) fnFlags |= (1u << 6u);
    if (fn.extensionMethod) fnFlags |= (1u << 7u);
    if (fn.constructor) fnFlags |= (1u << 8u);
    if (fn.propertyGetter) fnFlags |= (1u << 9u);
    if (fn.propertySetter) fnFlags |= (1u << 10u);
    if (fn.synthetic) fnFlags |= (1u << 11u);
    if (fn.asyncFunction) fnFlags |= (1u << 12u);
    if (fn.deterministic) fnFlags |= (1u << 13u);
    if (fn.nonDeterministic) fnFlags |= (1u << 14u);
    w.u16(fnFlags);
    w.u32(fn.virtualSlot);
    w.u32(fn.interfaceSlot);
    w.u32(s.indexOf(fn.moduleName));
    w.u32(s.indexOf(fn.name));
    w.u32(s.indexOf(fn.ownerTypeName));
    w.u64(fn.ownerTypeId);
    w.u8(static_cast<std::uint8_t>(fn.returnType));
    w.u32(s.indexOf(fn.returnTypeName));
    w.u8(static_cast<std::uint8_t>(fn.returnModifier));
    w.u8(static_cast<std::uint8_t>(fn.storageReturnType));
    w.u32(s.indexOf(fn.storageReturnTypeName));
    w.u32(static_cast<std::uint32_t>(fn.parameters.size()));
    for (const auto& p : fn.parameters) writeVariableCompact(w, s, p);
}

bool readFunctionCompact(
    Reader& r,
    const std::vector<std::string>& strings,
    semantic::FunctionSymbol& fn) {
    fn.id = r.u64();
    fn.accessibility = static_cast<semantic::Accessibility>(r.u8());
    fn.declaringTypeId = r.u64();
    fn.declaringTypeName = r.strFromTable(strings);
    const std::uint16_t fnFlags = r.u16();
    fn.virtualMethod = (fnFlags & (1u << 0u)) != 0u;
    fn.overrideMethod = (fnFlags & (1u << 1u)) != 0u;
    fn.abstractMethod = (fnFlags & (1u << 2u)) != 0u;
    fn.sealedMethod = (fnFlags & (1u << 3u)) != 0u;
    fn.interfaceMethod = (fnFlags & (1u << 4u)) != 0u;
    fn.method = (fnFlags & (1u << 5u)) != 0u;
    fn.staticMethod = (fnFlags & (1u << 6u)) != 0u;
    fn.extensionMethod = (fnFlags & (1u << 7u)) != 0u;
    fn.constructor = (fnFlags & (1u << 8u)) != 0u;
    fn.propertyGetter = (fnFlags & (1u << 9u)) != 0u;
    fn.propertySetter = (fnFlags & (1u << 10u)) != 0u;
    fn.synthetic = (fnFlags & (1u << 11u)) != 0u;
    fn.asyncFunction = (fnFlags & (1u << 12u)) != 0u;
    fn.deterministic = (fnFlags & (1u << 13u)) != 0u;
    fn.nonDeterministic = (fnFlags & (1u << 14u)) != 0u;
    fn.virtualSlot = r.u32();
    fn.interfaceSlot = r.u32();
    fn.moduleName = r.strFromTable(strings);
    fn.name = r.strFromTable(strings);
    fn.ownerTypeName = r.strFromTable(strings);
    fn.ownerTypeId = r.u64();
    fn.returnType = static_cast<semantic::PrimitiveType>(r.u8());
    fn.returnTypeName = r.strFromTable(strings);
    fn.returnModifier = static_cast<semantic::ParameterModifier>(r.u8());
    fn.storageReturnType = static_cast<semantic::PrimitiveType>(r.u8());
    fn.storageReturnTypeName = r.strFromTable(strings);
    const std::uint32_t pCount = r.u32();
    if (!r.valid() || pCount > 4096u) return false;
    fn.parameters.resize(pCount);
    for (std::uint32_t i = 0u; i < pCount; ++i) {
        if (!readVariableCompact(r, strings, fn.parameters[i])) return false;
    }
    fn.sourceName.clear();
    fn.declarationSpan = {};
    fn.bodySpan = {};
    return r.valid();
}

void internProperty(StringInterner& s, const semantic::PropertySymbol& p) {
    s.add(p.name);
    s.add(p.declaringTypeName);
    s.add(p.typeName);
    if (p.getter) internFunction(s, *p.getter);
    if (p.setter) internFunction(s, *p.setter);
    for (const auto& alias : p.formerlySerializedAs) s.add(alias);
}

void internEvent(StringInterner& s, const semantic::EventSymbol& e) {
    s.add(e.declaringTypeName);
    s.add(e.name);
    s.add(e.delegateName);
    internField(s, e.storageField);
    if (e.addAccessor) internFunction(s, *e.addAccessor);
    if (e.removeAccessor) internFunction(s, *e.removeAccessor);
    for (const auto& p : e.parameters) internVariable(s, p);
    for (const auto& h : e.handlers) {
        internFunction(s, h.function);
        internField(s, h.enabledField);
    }
    for (const auto& sub : e.subscriptions) internField(s, sub.enabledField);
}

void writeEventCompact(Writer& w, const StringInterner& s, const semantic::EventSymbol& e) {
    w.u64(e.id);
    w.u8(static_cast<std::uint8_t>(e.accessibility));
    w.u64(e.declaringTypeId);
    w.u32(s.indexOf(e.declaringTypeName));
    w.u32(s.indexOf(e.name));
    w.u32(s.indexOf(e.delegateName));
    writeFieldCompact(w, s, e.storageField);
    w.boolByte(e.addAccessor.has_value());
    if (e.addAccessor) writeFunctionCompact(w, s, *e.addAccessor);
    w.boolByte(e.removeAccessor.has_value());
    if (e.removeAccessor) writeFunctionCompact(w, s, *e.removeAccessor);
    w.u32(static_cast<std::uint32_t>(e.parameters.size()));
    for (const auto& p : e.parameters) writeVariableCompact(w, s, p);
    w.u32(static_cast<std::uint32_t>(e.handlers.size()));
    for (const auto& h : e.handlers) {
        writeFunctionCompact(w, s, h.function);
        writeFieldCompact(w, s, h.enabledField);
    }
    w.u32(static_cast<std::uint32_t>(e.subscriptions.size()));
    for (const auto& sub : e.subscriptions) {
        writeFieldCompact(w, s, sub.enabledField);
        w.boolByte(sub.enabled);
    }
}

bool readEventCompact(
    Reader& r,
    const std::vector<std::string>& strings,
    semantic::EventSymbol& e) {
    e.id = r.u64();
    e.accessibility = static_cast<semantic::Accessibility>(r.u8());
    e.declaringTypeId = r.u64();
    e.declaringTypeName = r.strFromTable(strings);
    e.name = r.strFromTable(strings);
    e.delegateName = r.strFromTable(strings);
    if (!readFieldCompact(r, strings, e.storageField)) return false;
    if (r.boolByte()) {
        semantic::FunctionSymbol fn;
        if (!readFunctionCompact(r, strings, fn)) return false;
        e.addAccessor = std::move(fn);
    } else {
        e.addAccessor.reset();
    }
    if (r.boolByte()) {
        semantic::FunctionSymbol fn;
        if (!readFunctionCompact(r, strings, fn)) return false;
        e.removeAccessor = std::move(fn);
    } else {
        e.removeAccessor.reset();
    }
    const std::uint32_t pCount = r.u32();
    if (!r.valid() || pCount > 4096u) return false;
    e.parameters.resize(pCount);
    for (std::uint32_t i = 0u; i < pCount; ++i) {
        if (!readVariableCompact(r, strings, e.parameters[i])) return false;
    }
    const std::uint32_t hCount = r.u32();
    if (!r.valid() || hCount > 4096u) return false;
    e.handlers.resize(hCount);
    for (std::uint32_t i = 0u; i < hCount; ++i) {
        if (!readFunctionCompact(r, strings, e.handlers[i].function)) return false;
        if (!readFieldCompact(r, strings, e.handlers[i].enabledField)) return false;
    }
    const std::uint32_t subCount = r.u32();
    if (!r.valid() || subCount > 4096u) return false;
    e.subscriptions.resize(subCount);
    for (std::uint32_t i = 0u; i < subCount; ++i) {
        e.subscriptions[i].span = {};
        if (!readFieldCompact(r, strings, e.subscriptions[i].enabledField)) return false;
        e.subscriptions[i].enabled = r.boolByte();
    }
    e.sourceName.clear();
    e.declarationSpan = {};
    return r.valid();
}

void internEnumMember(StringInterner& s, const semantic::EnumMemberSymbol& m) {
    s.add(m.name);
    for (const auto& alias : m.formerlySerializedAs) s.add(alias);
}

void writeEnumMemberCompact(
    Writer& w,
    const StringInterner& s,
    const semantic::EnumMemberSymbol& m) {
    w.u32(s.indexOf(m.name));
    w.i64(m.value);
    w.boolByte(m.fieldTag.has_value());
    if (m.fieldTag) w.i32(*m.fieldTag);
    w.u32(static_cast<std::uint32_t>(m.formerlySerializedAs.size()));
    for (const auto& alias : m.formerlySerializedAs) w.u32(s.indexOf(alias));
    w.u64(m.id);
}

bool readEnumMemberCompact(
    Reader& r,
    const std::vector<std::string>& strings,
    semantic::EnumMemberSymbol& m) {
    m.name = r.strFromTable(strings);
    m.value = r.i64();
    m.sourceName.clear();
    m.declarationSpan = {};
    if (r.boolByte()) m.fieldTag = r.i32();
    else m.fieldTag.reset();
    const std::uint32_t aliasCount = r.u32();
    if (!r.valid() || aliasCount > r.remaining() / 4u) return false;
    m.formerlySerializedAs.reserve(aliasCount);
    for (std::uint32_t i = 0u; i < aliasCount; ++i) {
        m.formerlySerializedAs.push_back(r.strFromTable(strings));
    }
    m.id = r.u64();
    return r.valid();
}

void internType(StringInterner& s, const semantic::TypeSymbol& t) {
    s.add(semantic::canonicalTypeName(t));
    s.add(t.baseTypeName);
    for (const auto& b : t.baseInterfaceNames) s.add(b);
    s.add(t.enclosingOwnerTypeName);
    s.add(t.moduleName);
    s.add(t.name);
    for (const auto& f : t.fields) internField(s, f);
    for (const auto& m : t.methods) internFunction(s, m);
    for (const auto& c : t.constructors) internFunction(s, c);
    if (t.staticConstructor) internFunction(s, *t.staticConstructor);
    for (const auto& p : t.properties) internProperty(s, p);
    for (const auto& e : t.events) internEvent(s, e);
    for (const auto& em : t.enumMembers) internEnumMember(s, em);
    for (const auto& hm : t.hygienicAccessibleModules) s.add(hm);
}

void writeAccessorDelta(
    Writer& w,
    const StringInterner& s,
    const std::optional<semantic::FunctionSymbol>& accessor,
    const std::vector<semantic::FunctionSymbol>& ownAndInheritedMethods,
    const std::unordered_map<semantic::SymbolId, std::uint32_t>& methodIdxById) {
    if (!accessor.has_value()) {
        w.u8(0u);
        return;
    }
    if (accessor->id != 0u) {
        const auto it = methodIdxById.find(accessor->id);
        if (it != methodIdxById.end() &&
            sameSemanticFunction(ownAndInheritedMethods[it->second], *accessor)) {
            w.u8(1u);
            w.u32(it->second);
            return;
        }
    }
    w.u8(2u);
    writeFunctionCompact(w, s, *accessor);
}

bool readAccessorDelta(
    Reader& r,
    const std::vector<std::string>& strings,
    const std::vector<semantic::FunctionSymbol>& methods,
    std::optional<semantic::FunctionSymbol>& outAccessor) {
    const std::uint8_t tag = r.u8();
    if (tag == 0u) {
        outAccessor.reset();
        return r.valid();
    }
    if (tag == 1u) {
        const std::uint32_t mIdx = r.u32();
        if (!r.valid() || mIdx >= methods.size()) return false;
        outAccessor = methods[mIdx];
        return true;
    }
    if (tag == 2u) {
        semantic::FunctionSymbol fn;
        if (!readFunctionCompact(r, strings, fn)) return false;
        outAccessor = std::move(fn);
        return true;
    }
    return false;
}

void writeDeltaTypeRecord(
    Writer& w,
    const StringInterner& s,
    const semantic::TypeSymbol& t,
    const semantic::TypeSymbol* base) {
    w.u64(t.id);
    w.u8(static_cast<std::uint8_t>(t.kind));
    w.u8(static_cast<std::uint8_t>(t.accessibility));
    std::uint16_t flags = 0u;
    if (t.synthetic) flags |= (1u << 0u);
    if (t.delegateType) flags |= (1u << 1u);
    if (t.interfaceType) flags |= (1u << 2u);
    if (t.abstractType) flags |= (1u << 3u);
    if (t.sealedType) flags |= (1u << 4u);
    if (t.staticType) flags |= (1u << 5u);
    if (t.partialType) flags |= (1u << 6u);
    if (t.stackOnly) flags |= (1u << 7u);
    if (t.hasValueLifecycle) flags |= (1u << 8u);
    if (t.blittable) flags |= (1u << 9u);
    if (t.hasBlittableAttribute) flags |= (1u << 10u);
    if (t.awaitableProtocol) flags |= (1u << 11u);
    if (t.deterministic) flags |= (1u << 12u);
    w.u16(flags);
    w.u64(t.baseTypeId);
    w.u32(s.indexOf(t.baseTypeName));

    w.u32(static_cast<std::uint32_t>(t.baseInterfaceNames.size()));
    for (const auto& b : t.baseInterfaceNames) w.u32(s.indexOf(b));
    w.u32(s.indexOf(t.enclosingOwnerTypeName));
    w.u32(s.indexOf(t.moduleName));
    w.u32(s.indexOf(t.name));

    // Delta fields prefix relative to base
    std::uint32_t inheritedFieldPrefix = 0u;
    if (base && t.fields.size() >= base->fields.size()) {
        bool matchPrefix = true;
        for (std::size_t i = 0u; i < base->fields.size(); ++i) {
            if (!sameSemanticField(base->fields[i], t.fields[i])) {
                matchPrefix = false;
                break;
            }
        }
        if (matchPrefix) {
            inheritedFieldPrefix = static_cast<std::uint32_t>(base->fields.size());
        }
    }
    w.u32(inheritedFieldPrefix);
    const std::uint32_t ownFieldCount =
        static_cast<std::uint32_t>(t.fields.size()) - inheritedFieldPrefix;
    w.u32(ownFieldCount);
    for (std::uint32_t i = inheritedFieldPrefix; i < static_cast<std::uint32_t>(t.fields.size()); ++i) {
        writeFieldCompact(w, s, t.fields[i]);
    }

    // Delta methods relative to base
    std::unordered_map<semantic::SymbolId, std::uint32_t> baseMethodIdxById;
    if (base) {
        baseMethodIdxById.reserve(base->methods.size());
        for (std::uint32_t bi = 0u; bi < static_cast<std::uint32_t>(base->methods.size()); ++bi) {
            if (base->methods[bi].id != 0u) {
                baseMethodIdxById.try_emplace(base->methods[bi].id, bi);
            }
        }
    }
    w.u32(static_cast<std::uint32_t>(t.methods.size()));
    for (const auto& m : t.methods) {
        bool wroteRef = false;
        if (base && m.id != 0u) {
            const auto bit = baseMethodIdxById.find(m.id);
            if (bit != baseMethodIdxById.end() &&
                sameSemanticFunction(base->methods[bit->second], m)) {
                w.u8(1u);
                w.u32(bit->second);
                wroteRef = true;
            }
        }
        if (!wroteRef) {
            w.u8(2u);
            writeFunctionCompact(w, s, m);
        }
    }

    w.u32(static_cast<std::uint32_t>(t.virtualDispatchTable.size()));
    for (const auto vid : t.virtualDispatchTable) w.u64(vid);

    w.u32(static_cast<std::uint32_t>(t.interfaceDispatchMaps.size()));
    for (const auto& imap : t.interfaceDispatchMaps) {
        w.u64(imap.interfaceTypeId);
        w.u32(static_cast<std::uint32_t>(imap.slots.size()));
        for (const auto slotId : imap.slots) w.u64(slotId);
    }

    w.u32(static_cast<std::uint32_t>(t.constructors.size()));
    for (const auto& c : t.constructors) writeFunctionCompact(w, s, c);

    w.boolByte(t.staticConstructor.has_value());
    if (t.staticConstructor) writeFunctionCompact(w, s, *t.staticConstructor);

    // Delta properties relative to base + method references for getter/setter
    std::unordered_map<semantic::SymbolId, std::uint32_t> ownMethodIdxById;
    ownMethodIdxById.reserve(t.methods.size());
    for (std::uint32_t mi = 0u; mi < static_cast<std::uint32_t>(t.methods.size()); ++mi) {
        if (t.methods[mi].id != 0u) {
            ownMethodIdxById.try_emplace(t.methods[mi].id, mi);
        }
    }
    w.u32(static_cast<std::uint32_t>(t.properties.size()));
    for (const auto& p : t.properties) {
        bool wroteBaseProp = false;
        if (base) {
            for (std::uint32_t bi = 0u; bi < static_cast<std::uint32_t>(base->properties.size()); ++bi) {
                if (sameSemanticProperty(base->properties[bi], p)) {
                    w.u8(1u);
                    w.u32(bi);
                    wroteBaseProp = true;
                    break;
                }
            }
        }
        if (wroteBaseProp) continue;
        w.u8(2u);
        w.u32(s.indexOf(p.name));
        w.u8(static_cast<std::uint8_t>(p.accessibility));
        w.u64(p.declaringTypeId);
        w.u32(s.indexOf(p.declaringTypeName));
        w.u8(static_cast<std::uint8_t>(p.type));
        w.u32(s.indexOf(p.typeName));
        w.boolByte(p.staticProperty);
        writeAccessorDelta(w, s, p.getter, t.methods, ownMethodIdxById);
        writeAccessorDelta(w, s, p.setter, t.methods, ownMethodIdxById);
        w.u32(p.backingFieldIndex == std::numeric_limits<std::size_t>::max()
            ? 0xffffffffu
            : static_cast<std::uint32_t>(p.backingFieldIndex));
        w.boolByte(p.fieldTag.has_value());
        if (p.fieldTag) w.i32(*p.fieldTag);
        w.u32(static_cast<std::uint32_t>(p.formerlySerializedAs.size()));
        for (const auto& alias : p.formerlySerializedAs) w.u32(s.indexOf(alias));
        w.u64(p.id);
    }

    // Delta events relative to base
    w.u32(static_cast<std::uint32_t>(t.events.size()));
    for (const auto& e : t.events) {
        bool wroteBaseEvent = false;
        if (base) {
            for (std::uint32_t bi = 0u; bi < static_cast<std::uint32_t>(base->events.size()); ++bi) {
                if (sameSemanticEvent(base->events[bi], e)) {
                    w.u8(1u);
                    w.u32(bi);
                    wroteBaseEvent = true;
                    break;
                }
            }
        }
        if (wroteBaseEvent) continue;
        w.u8(2u);
        writeEventCompact(w, s, e);
    }

    w.u32(static_cast<std::uint32_t>(t.enumMembers.size()));
    for (const auto& em : t.enumMembers) writeEnumMemberCompact(w, s, em);

    w.u32(static_cast<std::uint32_t>(t.hygienicAccessibleModules.size()));
    for (const auto& hm : t.hygienicAccessibleModules) w.u32(s.indexOf(hm));
}

bool readDeltaTypeRecord(
    Reader& r,
    const std::vector<std::string>& strings,
    const semantic::TypeSymbol* base,
    semantic::TypeSymbol& t) {
    t.id = r.u64();
    t.kind = static_cast<semantic::TypeKind>(r.u8());
    t.accessibility = static_cast<semantic::Accessibility>(r.u8());
    const std::uint16_t flags = r.u16();
    t.synthetic = (flags & (1u << 0u)) != 0u;
    t.delegateType = (flags & (1u << 1u)) != 0u;
    t.interfaceType = (flags & (1u << 2u)) != 0u;
    t.abstractType = (flags & (1u << 3u)) != 0u;
    t.sealedType = (flags & (1u << 4u)) != 0u;
    t.staticType = (flags & (1u << 5u)) != 0u;
    t.partialType = (flags & (1u << 6u)) != 0u;
    t.stackOnly = (flags & (1u << 7u)) != 0u;
    t.hasValueLifecycle = (flags & (1u << 8u)) != 0u;
    t.blittable = (flags & (1u << 9u)) != 0u;
    t.hasBlittableAttribute = (flags & (1u << 10u)) != 0u;
    t.awaitableProtocol = (flags & (1u << 11u)) != 0u;
    t.deterministic = (flags & (1u << 12u)) != 0u;
    t.baseTypeId = r.u64();
    t.baseTypeName = r.strFromTable(strings);

    const std::uint32_t baseIfaceCount = r.u32();
    if (!r.valid() || baseIfaceCount > r.remaining() / 4u) return false;
    t.baseInterfaceNames.reserve(baseIfaceCount);
    for (std::uint32_t i = 0u; i < baseIfaceCount; ++i) {
        t.baseInterfaceNames.push_back(r.strFromTable(strings));
    }
    t.enclosingOwnerTypeName = r.strFromTable(strings);
    t.moduleName = r.strFromTable(strings);
    t.name = r.strFromTable(strings);

    const std::uint32_t inheritedFieldPrefix = r.u32();
    const std::uint32_t ownFieldCount = r.u32();
    if (!r.valid() || ownFieldCount > 65536u) return false;
    if (inheritedFieldPrefix > 0u) {
        if (!base || inheritedFieldPrefix > base->fields.size()) return false;
        t.fields.reserve(static_cast<std::size_t>(inheritedFieldPrefix) + ownFieldCount);
        t.fields.insert(
            t.fields.end(),
            base->fields.begin(),
            base->fields.begin() + static_cast<std::ptrdiff_t>(inheritedFieldPrefix));
    } else {
        t.fields.reserve(ownFieldCount);
    }
    for (std::uint32_t i = 0u; i < ownFieldCount; ++i) {
        semantic::FieldSymbol f;
        if (!readFieldCompact(r, strings, f)) return false;
        t.fields.push_back(std::move(f));
    }

    const std::uint32_t methodCount = r.u32();
    if (!r.valid() || methodCount > 65536u) return false;
    t.methods.reserve(methodCount);
    for (std::uint32_t i = 0u; i < methodCount; ++i) {
        const std::uint8_t tag = r.u8();
        if (tag == 1u) {
            const std::uint32_t bIdx = r.u32();
            if (!r.valid() || !base || bIdx >= base->methods.size()) return false;
            t.methods.push_back(base->methods[bIdx]);
        } else if (tag == 2u) {
            semantic::FunctionSymbol fn;
            if (!readFunctionCompact(r, strings, fn)) return false;
            t.methods.push_back(std::move(fn));
        } else {
            return false;
        }
    }

    const std::uint32_t vdtCount = r.u32();
    if (!r.valid() || vdtCount > r.remaining() / 8u) return false;
    t.virtualDispatchTable.resize(vdtCount);
    for (std::uint32_t i = 0u; i < vdtCount; ++i) t.virtualDispatchTable[i] = r.u64();

    const std::uint32_t imapCount = r.u32();
    if (!r.valid() || imapCount > r.remaining() / 12u) return false;
    t.interfaceDispatchMaps.resize(imapCount);
    for (std::uint32_t i = 0u; i < imapCount; ++i) {
        t.interfaceDispatchMaps[i].interfaceTypeId = r.u64();
        const std::uint32_t slotCount = r.u32();
        if (!r.valid() || slotCount > r.remaining() / 8u) return false;
        t.interfaceDispatchMaps[i].slots.resize(slotCount);
        for (std::uint32_t j = 0u; j < slotCount; ++j) {
            t.interfaceDispatchMaps[i].slots[j] = r.u64();
        }
    }

    const std::uint32_t ctorCount = r.u32();
    if (!r.valid() || ctorCount > 65536u) return false;
    t.constructors.resize(ctorCount);
    for (std::uint32_t i = 0u; i < ctorCount; ++i) {
        if (!readFunctionCompact(r, strings, t.constructors[i])) return false;
    }
    if (r.boolByte()) {
        semantic::FunctionSymbol cctor;
        if (!readFunctionCompact(r, strings, cctor)) return false;
        t.staticConstructor = std::move(cctor);
    } else {
        t.staticConstructor.reset();
    }

    const std::uint32_t propCount = r.u32();
    if (!r.valid() || propCount > 65536u) return false;
    t.properties.reserve(propCount);
    for (std::uint32_t i = 0u; i < propCount; ++i) {
        const std::uint8_t tag = r.u8();
        if (tag == 1u) {
            const std::uint32_t bIdx = r.u32();
            if (!r.valid() || !base || bIdx >= base->properties.size()) return false;
            t.properties.push_back(base->properties[bIdx]);
            continue;
        }
        if (tag != 2u) return false;
        semantic::PropertySymbol p;
        p.name = r.strFromTable(strings);
        p.accessibility = static_cast<semantic::Accessibility>(r.u8());
        p.declaringTypeId = r.u64();
        p.declaringTypeName = r.strFromTable(strings);
        p.type = static_cast<semantic::PrimitiveType>(r.u8());
        p.typeName = r.strFromTable(strings);
        p.staticProperty = r.boolByte();
        if (!readAccessorDelta(r, strings, t.methods, p.getter)) return false;
        if (!readAccessorDelta(r, strings, t.methods, p.setter)) return false;
        const std::uint32_t bfIdx = r.u32();
        p.backingFieldIndex = (bfIdx == 0xffffffffu)
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(bfIdx);
        p.sourceName.clear();
        p.declarationSpan = {};
        if (r.boolByte()) p.fieldTag = r.i32();
        else p.fieldTag.reset();
        const std::uint32_t aliasCount = r.u32();
        if (!r.valid() || aliasCount > r.remaining() / 4u) return false;
        p.formerlySerializedAs.reserve(aliasCount);
        for (std::uint32_t k = 0u; k < aliasCount; ++k) {
            p.formerlySerializedAs.push_back(r.strFromTable(strings));
        }
        p.id = r.u64();
        t.properties.push_back(std::move(p));
    }

    const std::uint32_t evCount = r.u32();
    if (!r.valid() || evCount > 65536u) return false;
    t.events.reserve(evCount);
    for (std::uint32_t i = 0u; i < evCount; ++i) {
        const std::uint8_t tag = r.u8();
        if (tag == 1u) {
            const std::uint32_t bIdx = r.u32();
            if (!r.valid() || !base || bIdx >= base->events.size()) return false;
            t.events.push_back(base->events[bIdx]);
            continue;
        }
        if (tag != 2u) return false;
        semantic::EventSymbol e;
        if (!readEventCompact(r, strings, e)) return false;
        t.events.push_back(std::move(e));
    }

    const std::uint32_t enumCount = r.u32();
    if (!r.valid() || enumCount > 65536u) return false;
    t.enumMembers.resize(enumCount);
    for (std::uint32_t i = 0u; i < enumCount; ++i) {
        if (!readEnumMemberCompact(r, strings, t.enumMembers[i])) return false;
    }

    t.sourceName.clear();
    t.declarationSpan = {};

    const std::uint32_t hygCount = r.u32();
    if (!r.valid() || hygCount > r.remaining() / 4u) return false;
    for (std::uint32_t i = 0u; i < hygCount; ++i) {
        t.hygienicAccessibleModules.insert(r.strFromTable(strings));
    }
    return r.valid();
}

struct LazyTypeDirEntry {
    semantic::SymbolId id = 0u;
    std::string name;
    std::string canonicalName;
    semantic::SymbolId baseTypeId = 0u;
    std::uint32_t chunkIndex = 0u;
    std::uint32_t offsetInChunk = 0u;
    std::uint32_t recordSize = 0u;
    std::uint8_t flags = 0u;
};

struct LazyModuleIndex {
    std::size_t globalTypeOffset = 0u;
    std::vector<LazyTypeDirEntry> dir;
    std::unordered_map<std::string, std::size_t> byCanonical;
    std::unordered_map<std::string, std::size_t> bySimple;
    std::unordered_map<semantic::SymbolId, std::size_t> byId;
    std::vector<std::size_t> extensionOwnerIndices;
};

} // namespace

namespace detail {

struct LazyFrontendPackStorage {
    struct ChunkMeta {
        std::uint32_t uncompSize = 0u;
        std::uint32_t compOffset = 0u;
        std::uint32_t compSize = 0u;
    };
    struct CachedChunk {
        std::uint32_t chunkIdx = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint8_t> bytes;
    };

    const std::uint8_t* rawData = nullptr;
    std::size_t rawSize = 0u;
    std::vector<std::uint8_t> ownedBlob;
    std::vector<std::string> strings;
    std::vector<ChunkMeta> typeChunks;
    std::uint32_t genericCompOffset = 0u;
    std::uint32_t genericCompSize = 0u;
    std::uint32_t genericUncompSize = 0u;

    std::vector<LazyModuleIndex> modules;
    std::unordered_map<semantic::SymbolId, std::pair<std::size_t, std::size_t>> idToModDir;
    std::vector<semantic::SharedTypeRef> materializedSlots;
    std::atomic<std::size_t> materializedCount{0u};
    std::size_t totalIndexedTypes = 0u;

    std::vector<CachedChunk> chunkLru;
    bool genericLoaded = false;
    std::vector<std::vector<PrecompiledGenericTemplateSource>> cachedGenerics;
    mutable std::mutex mutex;

    [[nodiscard]] const std::vector<std::uint8_t>* getChunkLocked(std::uint32_t chunkIndex) {
        if (chunkIndex >= typeChunks.size() || !rawData) return nullptr;
        for (std::size_t i = 0u; i < chunkLru.size(); ++i) {
            if (chunkLru[i].chunkIdx == chunkIndex) {
                if (i != 0u) {
                    auto hit = std::move(chunkLru[i]);
                    chunkLru.erase(chunkLru.begin() + static_cast<std::ptrdiff_t>(i));
                    chunkLru.insert(chunkLru.begin(), std::move(hit));
                }
                return &chunkLru.front().bytes;
            }
        }
        const auto& meta = typeChunks[chunkIndex];
        if (meta.compOffset > rawSize || meta.compSize > rawSize - meta.compOffset) {
            return nullptr;
        }
        CachedChunk entry;
        entry.chunkIdx = chunkIndex;
        if (!bytecode::lz4DecompressBlock(
                rawData + meta.compOffset,
                meta.compSize,
                meta.uncompSize,
                entry.bytes)) {
            return nullptr;
        }
        if (chunkLru.size() >= kMaxLazyTypeChunks) {
            chunkLru.pop_back();
        }
        chunkLru.insert(chunkLru.begin(), std::move(entry));
        return &chunkLru.front().bytes;
    }

    [[nodiscard]] semantic::SharedTypeRef materialize(std::size_t modIdx, std::size_t dirIdx) {
        std::lock_guard<std::mutex> lock(mutex);
        if (modIdx >= modules.size() || dirIdx >= modules[modIdx].dir.size()) {
            return {};
        }
        const std::size_t targetSlot = modules[modIdx].globalTypeOffset + dirIdx;
        if (materializedSlots[targetSlot].hasValue()) {
            return materializedSlots[targetSlot];
        }

        struct ChainStep {
            std::size_t mIdx;
            std::size_t dIdx;
            std::size_t slot;
        };
        std::vector<ChainStep> chain;
        std::unordered_set<std::size_t> visiting;
        std::size_t curMod = modIdx;
        std::size_t curDir = dirIdx;
        while (chain.size() < 256u) {
            const auto& mMeta = modules[curMod];
            const std::size_t slot = mMeta.globalTypeOffset + curDir;
            if (materializedSlots[slot].hasValue() || !visiting.insert(slot).second) {
                break;
            }
            chain.push_back(ChainStep{curMod, curDir, slot});
            const semantic::SymbolId baseId = mMeta.dir[curDir].baseTypeId;
            if (baseId == 0u) break;
            const auto git = idToModDir.find(baseId);
            if (git == idToModDir.end()) break;
            curMod = git->second.first;
            curDir = git->second.second;
        }

        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto& entry = modules[it->mIdx].dir[it->dIdx];
            const semantic::TypeSymbol* basePtr = nullptr;
            if (entry.baseTypeId != 0u) {
                const auto git = idToModDir.find(entry.baseTypeId);
                if (git != idToModDir.end()) {
                    const std::size_t baseSlot =
                        modules[git->second.first].globalTypeOffset + git->second.second;
                    if (materializedSlots[baseSlot].hasValue()) {
                        basePtr = &(*materializedSlots[baseSlot]);
                    }
                }
            }
            const auto* chunkBytes = getChunkLocked(entry.chunkIndex);
            if (!chunkBytes ||
                entry.offsetInChunk > chunkBytes->size() ||
                entry.recordSize > chunkBytes->size() - entry.offsetInChunk) {
                return {};
            }
            Reader tr(chunkBytes->data() + entry.offsetInChunk, entry.recordSize);
            semantic::TypeSymbol decoded;
            if (!readDeltaTypeRecord(tr, strings, basePtr, decoded) ||
                !tr.valid() || tr.remaining() != 0u) {
                return {};
            }
            materializedSlots[it->slot] = semantic::SharedTypeRef(
                std::make_shared<const semantic::TypeSymbol>(std::move(decoded)));
            materializedCount.fetch_add(1u, std::memory_order_release);
        }
        return materializedSlots[targetSlot];
    }
};

} // namespace detail

namespace {

class LazyRsfpTypeSymbolLayer final : public semantic::TypeSymbolLayer {
public:
    LazyRsfpTypeSymbolLayer(
        std::shared_ptr<detail::LazyFrontendPackStorage> storage,
        std::size_t modIdx) noexcept
        : storage_(std::move(storage)), modIdx_(modIdx) {}

    semantic::SharedTypeRef findByName(
        std::string_view key,
        bool canonicalOnly) const override {
        if (!storage_ || modIdx_ >= storage_->modules.size()) return {};
        const auto& m = storage_->modules[modIdx_];
        const std::string keyStr(key);
        if (const auto it = m.byCanonical.find(keyStr); it != m.byCanonical.end()) {
            return storage_->materialize(modIdx_, it->second);
        }
        if (!canonicalOnly) {
            if (const auto it = m.bySimple.find(keyStr); it != m.bySimple.end()) {
                return storage_->materialize(modIdx_, it->second);
            }
        }
        return {};
    }

    semantic::SharedTypeRef findById(semantic::SymbolId id) const override {
        if (id == 0u || !storage_ || modIdx_ >= storage_->modules.size()) return {};
        const auto& m = storage_->modules[modIdx_];
        const auto it = m.byId.find(id);
        return it != m.byId.end() ? storage_->materialize(modIdx_, it->second) : semantic::SharedTypeRef{};
    }

    bool containsName(
        std::string_view key,
        bool canonicalOnly) const override {
        if (!storage_ || modIdx_ >= storage_->modules.size()) return false;
        const auto& m = storage_->modules[modIdx_];
        const std::string keyStr(key);
        if (m.byCanonical.find(keyStr) != m.byCanonical.end()) return true;
        if (!canonicalOnly && m.bySimple.find(keyStr) != m.bySimple.end()) return true;
        return false;
    }

    std::size_t entryCount() const noexcept override {
        if (!storage_ || modIdx_ >= storage_->modules.size()) return 0u;
        return storage_->modules[modIdx_].dir.size() * 2u;
    }

    void forEachExtensionOwner(
        const std::function<void(const semantic::TypeSymbol&)>& fn) const override {
        if (!storage_ || modIdx_ >= storage_->modules.size()) return;
        const auto& ownerIndices = storage_->modules[modIdx_].extensionOwnerIndices;
        for (const std::size_t dirIdx : ownerIndices) {
            if (auto ref = storage_->materialize(modIdx_, dirIdx); ref.hasValue()) {
                fn(*ref);
            }
        }
    }

    void forEachType(
        const std::function<void(const semantic::SharedTypeRef&)>& fn) const override {
        if (!storage_ || modIdx_ >= storage_->modules.size()) return;
        const std::size_t count = storage_->modules[modIdx_].dir.size();
        for (std::size_t i = 0u; i < count; ++i) {
            if (auto ref = storage_->materialize(modIdx_, i); ref.hasValue()) {
                fn(ref);
            }
        }
    }

private:
    std::shared_ptr<detail::LazyFrontendPackStorage> storage_;
    std::size_t modIdx_ = 0u;
};

} // namespace

const PrecompiledModuleFrontend* PrecompiledFrontendPack::findModule(
    std::string_view moduleName) const noexcept {
    for (const auto& mod : modules) {
        if (mod.name == moduleName) {
            return &mod;
        }
    }
    return nullptr;
}

std::vector<std::string> PrecompiledFrontendPack::moduleNames() const {
    std::vector<std::string> out;
    out.reserve(modules.size());
    for (const auto& mod : modules) {
        out.push_back(mod.name);
    }
    return out;
}

bool PrecompiledFrontendPack::loadGenericTemplates(
    std::size_t moduleIndex,
    std::vector<PrecompiledGenericTemplateSource>& outTemplates) const {
    outTemplates.clear();
    if (moduleIndex >= modules.size()) return false;
    if (!modules[moduleIndex].genericTemplateSources.empty()) {
        outTemplates = modules[moduleIndex].genericTemplateSources;
        return true;
    }
    if (!lazyStorage) return false;
    auto& st = *lazyStorage;
    std::lock_guard<std::mutex> lock(st.mutex);
    if (!st.genericLoaded) {
        st.genericLoaded = true;
        st.cachedGenerics.resize(modules.size());
        if (st.genericCompSize > 0u && st.genericUncompSize > 0u &&
            st.rawData && st.genericCompOffset <= st.rawSize &&
            st.genericCompSize <= st.rawSize - st.genericCompOffset) {
            std::vector<std::uint8_t> rawGen;
            if (bytecode::lz4DecompressBlock(
                    st.rawData + st.genericCompOffset,
                    st.genericCompSize,
                    st.genericUncompSize,
                    rawGen)) {
                Reader gr(rawGen.data(), rawGen.size());
                const std::uint32_t mCount = gr.u32();
                if (gr.valid() && mCount == modules.size()) {
                    for (std::uint32_t m = 0u; m < mCount; ++m) {
                        const std::uint32_t tCount = gr.u32();
                        if (!gr.valid() || tCount > 65536u) {
                            st.cachedGenerics.clear();
                            break;
                        }
                        auto& vec = st.cachedGenerics[m];
                        vec.resize(tCount);
                        for (std::uint32_t ti = 0u; ti < tCount; ++ti) {
                            vec[ti].virtualPath = gr.strFromTable(st.strings);
                            vec[ti].sourceText = gr.rawStr();
                            if (!gr.valid()) {
                                st.cachedGenerics.clear();
                                break;
                            }
                        }
                        if (st.cachedGenerics.empty()) break;
                    }
                }
            }
        }
    }
    if (moduleIndex < st.cachedGenerics.size()) {
        outTemplates = st.cachedGenerics[moduleIndex];
    }
    return true;
}

std::size_t PrecompiledFrontendPack::materializedTypeCount() const noexcept {
    if (!lazyStorage) {
        std::size_t n = 0u;
        for (const auto& m : modules) n += m.types.size();
        return n;
    }
    return lazyStorage->materializedCount.load(std::memory_order_acquire);
}

std::size_t PrecompiledFrontendPack::totalIndexedTypeCount() const noexcept {
    if (!lazyStorage) {
        std::size_t n = 0u;
        for (const auto& m : modules) n += m.types.size();
        return n;
    }
    return lazyStorage->totalIndexedTypes;
}

void PrecompiledFrontendPack::materializeAllTypes() {
    if (!lazyStorage) return;
    for (std::size_t mIdx = 0u; mIdx < modules.size(); ++mIdx) {
        if (mIdx >= lazyStorage->modules.size()) break;
        const std::size_t n = lazyStorage->modules[mIdx].dir.size();
        if (modules[mIdx].types.size() == n) continue;
        modules[mIdx].types.clear();
        modules[mIdx].types.reserve(n);
        for (std::size_t ti = 0u; ti < n; ++ti) {
            auto ref = lazyStorage->materialize(mIdx, ti);
            if (ref.hasValue()) {
                modules[mIdx].types.push_back(*ref);
            }
        }
    }
}

std::vector<std::uint8_t> encodePrecompiledFrontendPack(
    const PrecompiledFrontendPack& pack) {
    StringInterner interner;
    for (const auto& mod : pack.modules) {
        interner.add(mod.name);
        for (const auto& imp : mod.imports) interner.add(imp);
        for (const auto& hm : mod.hygienicModules) interner.add(hm);
        for (const auto& t : mod.types) internType(interner, t);
        for (const auto& iface : mod.interfaces) {
            interner.add(iface.moduleName);
            interner.add(iface.name);
            for (const auto& b : iface.baseInterfaces) interner.add(b);
            for (const auto& m : iface.methods) {
                interner.add(m.name);
                interner.add(m.returnType.typeName);
                for (const auto& p : m.parameters) interner.add(p.typeName);
                for (const auto& hm : m.hygienicModules) interner.add(hm);
            }
        }
        for (const auto& del : mod.delegates) {
            interner.add(del.moduleName);
            interner.add(del.name);
            interner.add(del.returnType.typeName);
            for (const auto& p : del.parameters) interner.add(p.typeName);
        }
        for (const auto& fn : mod.declarations) internFunction(interner, fn);
        for (const auto& attr : mod.nativeAttributes) {
            interner.add(attr.target);
            interner.add(attr.name);
            for (const auto& arg : attr.arguments) {
                interner.add(arg.name);
                interner.add(arg.value);
            }
        }
        for (const auto& seq : mod.nativeSequences) {
            interner.add(seq.typeName);
            interner.add(seq.name);
            interner.add(seq.resultTypeName);
            for (const auto& cb : seq.callbacks) interner.add(cb);
        }
        for (const auto& impl : mod.nativeInterfaces) {
            interner.add(impl.typeName);
            for (const auto& iname : impl.interfaces) interner.add(iname);
        }
        for (const auto& inst : mod.nativeGenericInstantiations) {
            interner.add(inst.genericName);
            for (const auto& a : inst.arguments) interner.add(a);
            interner.add(inst.generatedName);
        }
        for (const auto& src : mod.genericTemplateSources) {
            interner.add(src.virtualPath);
        }
    }

    // Build global type map and topological order (base classes before derived)
    struct FlatTypeRef {
        std::size_t modIdx = 0u;
        std::size_t typeIdx = 0u;
    };
    std::vector<FlatTypeRef> allTypes;
    std::unordered_map<semantic::SymbolId, std::size_t> flatIdxById;
    for (std::size_t mIdx = 0u; mIdx < pack.modules.size(); ++mIdx) {
        const auto& mod = pack.modules[mIdx];
        for (std::size_t tIdx = 0u; tIdx < mod.types.size(); ++tIdx) {
            const std::size_t flatIdx = allTypes.size();
            allTypes.push_back(FlatTypeRef{mIdx, tIdx});
            if (mod.types[tIdx].id != 0u) {
                flatIdxById.try_emplace(mod.types[tIdx].id, flatIdx);
            }
        }
    }
    std::vector<std::size_t> topoOrder;
    topoOrder.reserve(allTypes.size());
    std::vector<std::uint8_t> visitState(allTypes.size(), 0u);
    auto visitTopo = [&](auto& self, std::size_t flatIdx) -> void {
        if (visitState[flatIdx] != 0u) return;
        visitState[flatIdx] = 1u;
        const auto& tr = allTypes[flatIdx];
        const auto& ts = pack.modules[tr.modIdx].types[tr.typeIdx];
        if (ts.baseTypeId != 0u) {
            if (const auto bit = flatIdxById.find(ts.baseTypeId); bit != flatIdxById.end()) {
                self(self, bit->second);
            }
        }
        visitState[flatIdx] = 2u;
        topoOrder.push_back(flatIdx);
    };
    for (std::size_t i = 0u; i < allTypes.size(); ++i) {
        visitTopo(visitTopo, i);
    }

    struct EncodedTypeLoc {
        std::uint32_t chunkIndex = 0u;
        std::uint32_t offsetInChunk = 0u;
        std::uint32_t recordSize = 0u;
        std::uint8_t flags = 0u;
    };
    std::vector<EncodedTypeLoc> encodedLocs(allTypes.size());
    struct CompressedTypeChunk {
        std::uint32_t uncompSize = 0u;
        std::vector<std::uint8_t> compBytes;
    };
    std::vector<CompressedTypeChunk> encodedTypeChunks;
    Writer curTypeChunk;
    auto flushTypeChunk = [&]() {
        if (curTypeChunk.size() == 0u) return;
        const auto& raw = curTypeChunk.data();
        CompressedTypeChunk c;
        c.uncompSize = static_cast<std::uint32_t>(raw.size());
        if (!bytecode::lz4CompressBlock(raw.data(), raw.size(), c.compBytes)) {
            c.compBytes.clear();
        }
        encodedTypeChunks.push_back(std::move(c));
        curTypeChunk = Writer{};
    };

    for (const std::size_t flatIdx : topoOrder) {
        const auto& tr = allTypes[flatIdx];
        const auto& t = pack.modules[tr.modIdx].types[tr.typeIdx];
        const semantic::TypeSymbol* base = nullptr;
        if (t.baseTypeId != 0u) {
            if (const auto bit = flatIdxById.find(t.baseTypeId); bit != flatIdxById.end()) {
                const auto& btr = allTypes[bit->second];
                base = &pack.modules[btr.modIdx].types[btr.typeIdx];
            }
        }
        Writer rec;
        writeDeltaTypeRecord(rec, interner, t, base);
        if (curTypeChunk.size() > 0u &&
            curTypeChunk.size() + rec.size() > kTypeChunkTargetBytes) {
            flushTypeChunk();
        }
        EncodedTypeLoc loc;
        loc.chunkIndex = static_cast<std::uint32_t>(encodedTypeChunks.size());
        loc.offsetInChunk = static_cast<std::uint32_t>(curTypeChunk.size());
        loc.recordSize = static_cast<std::uint32_t>(rec.size());
        if (t.interfaceType) loc.flags |= kDirFlagInterfaceType;
        for (const auto& m : t.methods) {
            if (m.staticMethod && m.extensionMethod && !m.parameters.empty()) {
                loc.flags |= kDirFlagExtensionOwner;
                break;
            }
        }
        encodedLocs[flatIdx] = loc;
        curTypeChunk.bytes(rec.data().data(), rec.size());
    }
    flushTypeChunk();
    for (const auto& c : encodedTypeChunks) {
        if (c.compBytes.empty() && c.uncompSize > 0u) return {};
    }

    // Build generic template block
    Writer genWriter;
    bool hasAnyGenerics = false;
    for (const auto& mod : pack.modules) {
        if (!mod.genericTemplateSources.empty()) {
            hasAnyGenerics = true;
            break;
        }
    }
    std::vector<std::uint8_t> compressedGenerics;
    std::uint32_t uncompressedGenericsSize = 0u;
    if (hasAnyGenerics) {
        genWriter.u32(static_cast<std::uint32_t>(pack.modules.size()));
        for (const auto& mod : pack.modules) {
            genWriter.u32(static_cast<std::uint32_t>(mod.genericTemplateSources.size()));
            for (const auto& src : mod.genericTemplateSources) {
                genWriter.u32(interner.indexOf(src.virtualPath));
                genWriter.rawStr(src.sourceText);
            }
        }
        const auto rawGen = genWriter.take();
        uncompressedGenericsSize = static_cast<std::uint32_t>(rawGen.size());
        if (!bytecode::lz4CompressBlock(rawGen.data(), rawGen.size(), compressedGenerics)) {
            return {};
        }
    }

    // Build Directory block
    Writer dirPayload;
    const auto& strs = interner.strings();
    dirPayload.u32(static_cast<std::uint32_t>(strs.size()));
    for (const auto& s : strs) dirPayload.rawStr(s);

    std::size_t runningFlatIdx = 0u;
    dirPayload.u32(static_cast<std::uint32_t>(pack.modules.size()));
    for (const auto& mod : pack.modules) {
        dirPayload.u32(interner.indexOf(mod.name));
        dirPayload.u64(mod.sourceFingerprint);
        dirPayload.u64(mod.transitiveSourceFingerprint);
        dirPayload.u64(mod.publicFingerprint);
        dirPayload.u64(mod.dependencyFingerprint);
        dirPayload.boolByte(mod.deterministic);

        dirPayload.u32(static_cast<std::uint32_t>(mod.imports.size()));
        for (const auto& imp : mod.imports) dirPayload.u32(interner.indexOf(imp));

        dirPayload.u32(static_cast<std::uint32_t>(mod.hygienicModules.size()));
        for (const auto& hm : mod.hygienicModules) dirPayload.u32(interner.indexOf(hm));

        dirPayload.u32(static_cast<std::uint32_t>(mod.types.size()));
        for (const auto& t : mod.types) {
            const auto& loc = encodedLocs[runningFlatIdx++];
            dirPayload.u64(t.id);
            dirPayload.u32(interner.indexOf(t.name));
            dirPayload.u32(interner.indexOf(semantic::canonicalTypeName(t)));
            dirPayload.u64(t.baseTypeId);
            dirPayload.u32(loc.chunkIndex);
            dirPayload.u32(loc.offsetInChunk);
            dirPayload.u32(loc.recordSize);
            dirPayload.u8(loc.flags);
        }

        dirPayload.u32(static_cast<std::uint32_t>(mod.interfaces.size()));
        for (const auto& iface : mod.interfaces) {
            dirPayload.u32(interner.indexOf(iface.moduleName));
            dirPayload.u32(interner.indexOf(iface.name));
            dirPayload.u32(static_cast<std::uint32_t>(iface.baseInterfaces.size()));
            for (const auto& b : iface.baseInterfaces) dirPayload.u32(interner.indexOf(b));
            dirPayload.u32(static_cast<std::uint32_t>(iface.methods.size()));
            for (const auto& m : iface.methods) {
                dirPayload.u32(interner.indexOf(m.name));
                dirPayload.u8(static_cast<std::uint8_t>(m.returnType.type));
                dirPayload.u32(interner.indexOf(m.returnType.typeName));
                dirPayload.u32(static_cast<std::uint32_t>(m.parameters.size()));
                for (const auto& p : m.parameters) {
                    dirPayload.u8(static_cast<std::uint8_t>(p.type));
                    dirPayload.u32(interner.indexOf(p.typeName));
                }
                dirPayload.u32(static_cast<std::uint32_t>(m.parameterModifiers.size()));
                for (const auto& pm : m.parameterModifiers) {
                    dirPayload.u8(static_cast<std::uint8_t>(pm));
                }
                dirPayload.boolByte(m.hasDefaultImplementation);
                dirPayload.u32(static_cast<std::uint32_t>(m.hygienicModules.size()));
                for (const auto& hm : m.hygienicModules) dirPayload.u32(interner.indexOf(hm));
            }
        }

        dirPayload.u32(static_cast<std::uint32_t>(mod.delegates.size()));
        for (const auto& del : mod.delegates) {
            dirPayload.u32(interner.indexOf(del.moduleName));
            dirPayload.u32(interner.indexOf(del.name));
            dirPayload.u8(static_cast<std::uint8_t>(del.returnType.type));
            dirPayload.u32(interner.indexOf(del.returnType.typeName));
            dirPayload.u32(static_cast<std::uint32_t>(del.parameters.size()));
            for (const auto& p : del.parameters) {
                dirPayload.u8(static_cast<std::uint8_t>(p.type));
                dirPayload.u32(interner.indexOf(p.typeName));
            }
            dirPayload.u32(static_cast<std::uint32_t>(del.parameterModifiers.size()));
            for (const auto& pm : del.parameterModifiers) {
                dirPayload.u8(static_cast<std::uint8_t>(pm));
            }
        }

        dirPayload.u32(static_cast<std::uint32_t>(mod.declarations.size()));
        for (const auto& fn : mod.declarations) writeFunctionCompact(dirPayload, interner, fn);

        dirPayload.u32(static_cast<std::uint32_t>(mod.nativeAttributes.size()));
        for (const auto& attr : mod.nativeAttributes) {
            dirPayload.u32(interner.indexOf(attr.target));
            dirPayload.u32(interner.indexOf(attr.name));
            dirPayload.u32(static_cast<std::uint32_t>(attr.arguments.size()));
            for (const auto& arg : attr.arguments) {
                dirPayload.u32(interner.indexOf(arg.name));
                dirPayload.u32(interner.indexOf(arg.value));
            }
            dirPayload.u32(static_cast<std::uint32_t>(attr.offset));
        }

        dirPayload.u32(static_cast<std::uint32_t>(mod.nativeSequences.size()));
        for (const auto& seq : mod.nativeSequences) {
            dirPayload.u32(interner.indexOf(seq.typeName));
            dirPayload.u32(interner.indexOf(seq.name));
            dirPayload.u32(interner.indexOf(seq.resultTypeName));
            dirPayload.u32(static_cast<std::uint32_t>(seq.callbacks.size()));
            for (const auto& cb : seq.callbacks) dirPayload.u32(interner.indexOf(cb));
            dirPayload.u32(static_cast<std::uint32_t>(seq.offset));
        }

        dirPayload.u32(static_cast<std::uint32_t>(mod.nativeInterfaces.size()));
        for (const auto& impl : mod.nativeInterfaces) {
            dirPayload.u32(interner.indexOf(impl.typeName));
            dirPayload.u32(static_cast<std::uint32_t>(impl.interfaces.size()));
            for (const auto& iname : impl.interfaces) dirPayload.u32(interner.indexOf(iname));
        }

        dirPayload.u32(static_cast<std::uint32_t>(mod.nativeGenericInstantiations.size()));
        for (const auto& inst : mod.nativeGenericInstantiations) {
            dirPayload.u32(interner.indexOf(inst.genericName));
            dirPayload.u32(static_cast<std::uint32_t>(inst.arguments.size()));
            for (const auto& a : inst.arguments) dirPayload.u32(interner.indexOf(a));
            dirPayload.u32(interner.indexOf(inst.generatedName));
        }
    }

    const std::vector<std::uint8_t> rawDir = dirPayload.take();
    std::vector<std::uint8_t> compDir;
    if (!bytecode::lz4CompressBlock(rawDir.data(), rawDir.size(), compDir)) {
        return {};
    }

    const std::uint32_t typeChunkCount = static_cast<std::uint32_t>(encodedTypeChunks.size());
    const std::uint32_t typeChunkDirOffset = kHeaderSizeV2;
    const std::uint32_t dirCompOffset = typeChunkDirOffset + typeChunkCount * 12u;
    const std::uint32_t dirCompSize = static_cast<std::uint32_t>(compDir.size());
    const std::uint32_t dirUncompSize = static_cast<std::uint32_t>(rawDir.size());

    std::uint32_t nextOffset = dirCompOffset + dirCompSize;
    std::vector<std::uint32_t> chunkCompOffsets(typeChunkCount);
    for (std::uint32_t i = 0u; i < typeChunkCount; ++i) {
        chunkCompOffsets[i] = nextOffset;
        nextOffset += static_cast<std::uint32_t>(encodedTypeChunks[i].compBytes.size());
    }
    const std::uint32_t genericCompOffset = nextOffset;
    const std::uint32_t genericCompSize = static_cast<std::uint32_t>(compressedGenerics.size());

    Writer bodyAfterHdr;
    for (std::uint32_t i = 0u; i < typeChunkCount; ++i) {
        bodyAfterHdr.u32(encodedTypeChunks[i].uncompSize);
        bodyAfterHdr.u32(chunkCompOffsets[i]);
        bodyAfterHdr.u32(static_cast<std::uint32_t>(encodedTypeChunks[i].compBytes.size()));
    }
    bodyAfterHdr.bytes(compDir.data(), compDir.size());
    for (const auto& c : encodedTypeChunks) {
        bodyAfterHdr.bytes(c.compBytes.data(), c.compBytes.size());
    }
    bodyAfterHdr.bytes(compressedGenerics.data(), compressedGenerics.size());

    const auto bodyBytes = bodyAfterHdr.take();
    const std::uint64_t contentHash = pack.contentHash != 0u
        ? pack.contentHash
        : bytecode::fastChecksum64(bodyBytes.data(), bodyBytes.size(), 0u);
    const std::uint64_t payloadChecksum =
        bytecode::fastChecksum64(bodyBytes.data(), bodyBytes.size(), contentHash);

    Writer out;
    out.bytes(kMagic, 4u);
    out.u32(kFormatVersionV3);
    out.u32(realscript::kVersionMajor);
    out.u32(realscript::kVersionMinor);
    out.u32(realscript::kVersionPatch);
    out.u32(realscript::kSdkCompatibilityVersion);
    out.u64(contentHash);
    out.u32(dirCompOffset);
    out.u32(dirCompSize);
    out.u32(dirUncompSize);
    out.u32(typeChunkCount);
    out.u32(typeChunkDirOffset);
    out.u32(genericCompOffset);
    out.u32(genericCompSize);
    out.u32(uncompressedGenericsSize);
    out.u64(payloadChecksum);
    out.bytes(bodyBytes.data(), bodyBytes.size());
    return out.take();
}

bool decodePrecompiledFrontendPack(
    const std::uint8_t* data,
    std::size_t size,
    PrecompiledFrontendPack& outPack,
    std::string* outError,
    bool copyBlob,
    bool skipChecksum) {
    outPack = {};
    auto fail = [&](std::string_view msg) -> bool {
        if (outError) *outError = std::string(msg);
        outPack = {};
        return false;
    };
    if (!data || size < kHeaderSizeV2) {
        return fail("RSFP buffer is truncated or null");
    }
    if (std::memcmp(data, kMagic, 4u) != 0) {
        return fail("RSFP magic mismatch");
    }

    Reader hdr(data + 4u, size - 4u);
    const std::uint32_t fmtVer = hdr.u32();
    const std::uint32_t vMaj = hdr.u32();
    const std::uint32_t vMin = hdr.u32();
    const std::uint32_t vPat = hdr.u32();
    const std::uint32_t vSdk = hdr.u32();
    const std::uint64_t contentHash = hdr.u64();
    const std::uint32_t dirCompOffset = hdr.u32();
    const std::uint32_t dirCompSize = hdr.u32();
    const std::uint32_t dirUncompSize = hdr.u32();
    const std::uint32_t typeChunkCount = hdr.u32();
    const std::uint32_t typeChunkDirOffset = hdr.u32();
    const std::uint32_t genericCompOffset = hdr.u32();
    const std::uint32_t genericCompSize = hdr.u32();
    const std::uint32_t genericUncompSize = hdr.u32();
    const std::uint64_t storedChecksum = hdr.u64();

    if (!hdr.valid() || fmtVer != kFormatVersionV3) {
        return fail("unsupported RSFP format version");
    }
    if (vMaj != realscript::kVersionMajor ||
        vMin != realscript::kVersionMinor ||
        vPat != realscript::kVersionPatch ||
        vSdk != realscript::kSdkCompatibilityVersion) {
        return fail("RSFP compiler/SDK version mismatch");
    }
    if (typeChunkCount > (size - kHeaderSizeV2) / 12u ||
        typeChunkDirOffset != kHeaderSizeV2) {
        return fail("RSFP chunk directory bounds invalid");
    }
    const std::size_t dirTableEnd =
        static_cast<std::size_t>(typeChunkDirOffset) + static_cast<std::size_t>(typeChunkCount) * 12u;
    if (dirCompOffset < dirTableEnd ||
        dirCompOffset > size ||
        dirCompSize == 0u ||
        dirCompSize > size - dirCompOffset ||
        dirUncompSize == 0u ||
        dirUncompSize > 256u * 1024u * 1024u ||
        genericCompOffset > size ||
        genericCompSize > size - genericCompOffset) {
        return fail("RSFP section bounds invalid");
    }

    if (!skipChecksum) {
        const std::uint64_t actualChecksum = bytecode::fastChecksum64(
            data + kHeaderSizeV2, size - kHeaderSizeV2, contentHash);
        if (actualChecksum != storedChecksum) {
            return fail("RSFP payload checksum mismatch");
        }
    }

    auto storage = std::make_shared<detail::LazyFrontendPackStorage>();
    if (copyBlob) {
        storage->ownedBlob.assign(data, data + size);
        storage->rawData = storage->ownedBlob.data();
        storage->rawSize = storage->ownedBlob.size();
    } else {
        storage->rawData = data;
        storage->rawSize = size;
    }
    storage->genericCompOffset = genericCompOffset;
    storage->genericCompSize = genericCompSize;
    storage->genericUncompSize = genericUncompSize;

    storage->typeChunks.resize(typeChunkCount);
    for (std::uint32_t i = 0u; i < typeChunkCount; ++i) {
        auto& cm = storage->typeChunks[i];
        cm.uncompSize = hdr.u32();
        cm.compOffset = hdr.u32();
        cm.compSize = hdr.u32();
        if (!hdr.valid() ||
            cm.uncompSize == 0u ||
            cm.uncompSize > 128u * 1024u * 1024u ||
            cm.compSize == 0u ||
            cm.compOffset < dirCompOffset + dirCompSize ||
            cm.compOffset > size ||
            cm.compSize > size - cm.compOffset) {
            return fail("RSFP type chunk metadata invalid");
        }
    }

    std::vector<std::uint8_t> rawDir;
    if (!bytecode::lz4DecompressBlock(
            data + dirCompOffset, dirCompSize, dirUncompSize, rawDir)) {
        return fail("RSFP directory LZ4 decompression failed");
    }

    Reader r(rawDir.data(), rawDir.size());
    const std::uint32_t strCount = r.u32();
    if (!r.valid() || strCount > 2000000u) {
        return fail("RSFP string table header invalid");
    }
    storage->strings.resize(strCount);
    for (std::uint32_t i = 0u; i < strCount; ++i) {
        storage->strings[i] = r.rawStr();
        if (!r.valid()) return fail("RSFP truncated string table entry");
    }
    const auto& strings = storage->strings;

    const std::uint32_t modCount = r.u32();
    if (!r.valid() || modCount > 65536u) {
        return fail("RSFP module count invalid");
    }
    outPack.contentHash = contentHash;
    outPack.modules.resize(modCount);
    outPack.lazyStorage = storage;
    storage->modules.resize(modCount);

    std::size_t runningSlotOffset = 0u;
    for (std::uint32_t m = 0u; m < modCount; ++m) {
        auto& mod = outPack.modules[m];
        auto& mIdxMeta = storage->modules[m];
        mIdxMeta.globalTypeOffset = runningSlotOffset;

        mod.name = r.strFromTable(strings);
        mod.sourceFingerprint = r.u64();
        mod.transitiveSourceFingerprint = r.u64();
        mod.publicFingerprint = r.u64();
        mod.dependencyFingerprint = r.u64();
        mod.deterministic = r.boolByte();

        const std::uint32_t impCount = r.u32();
        if (!r.valid() || impCount > r.remaining() / 4u) return fail("RSFP invalid imports");
        mod.imports.reserve(impCount);
        for (std::uint32_t i = 0u; i < impCount; ++i) {
            mod.imports.push_back(r.strFromTable(strings));
        }

        const std::uint32_t hygCount = r.u32();
        if (!r.valid() || hygCount > r.remaining() / 4u) return fail("RSFP invalid hygienicModules");
        mod.hygienicModules.reserve(hygCount);
        for (std::uint32_t i = 0u; i < hygCount; ++i) {
            mod.hygienicModules.push_back(r.strFromTable(strings));
        }

        const std::uint32_t typeCount = r.u32();
        if (!r.valid() || typeCount > 65536u) return fail("RSFP invalid types directory");
        mIdxMeta.dir.resize(typeCount);
        mIdxMeta.byCanonical.reserve(typeCount);
        mIdxMeta.bySimple.reserve(typeCount);
        mIdxMeta.byId.reserve(typeCount);
        for (std::uint32_t ti = 0u; ti < typeCount; ++ti) {
            auto& entry = mIdxMeta.dir[ti];
            entry.id = r.u64();
            entry.name = r.strFromTable(strings);
            entry.canonicalName = r.strFromTable(strings);
            entry.baseTypeId = r.u64();
            entry.chunkIndex = r.u32();
            entry.offsetInChunk = r.u32();
            entry.recordSize = r.u32();
            entry.flags = r.u8();
            if (!r.valid() ||
                (typeCount > 0u && entry.chunkIndex >= typeChunkCount)) {
                return fail("RSFP invalid type directory entry");
            }
            if (!entry.canonicalName.empty()) {
                mIdxMeta.byCanonical.emplace(entry.canonicalName, ti);
            }
            if (!entry.name.empty()) {
                mIdxMeta.bySimple.try_emplace(entry.name, ti);
            }
            if (entry.id != 0u) {
                mIdxMeta.byId.emplace(entry.id, ti);
                storage->idToModDir.try_emplace(entry.id, std::make_pair(std::size_t{m}, std::size_t{ti}));
            }
            if ((entry.flags & kDirFlagExtensionOwner) != 0u) {
                mIdxMeta.extensionOwnerIndices.push_back(ti);
            }
        }
        runningSlotOffset += typeCount;
        mod.typeLayer = std::make_shared<LazyRsfpTypeSymbolLayer>(storage, m);

        const std::uint32_t ifaceCount = r.u32();
        if (!r.valid() || ifaceCount > 65536u) return fail("RSFP invalid interfaces");
        mod.interfaces.resize(ifaceCount);
        for (std::uint32_t i = 0u; i < ifaceCount; ++i) {
            auto& iface = mod.interfaces[i];
            iface.moduleName = r.strFromTable(strings);
            iface.name = r.strFromTable(strings);
            const std::uint32_t biCount = r.u32();
            if (!r.valid() || biCount > r.remaining() / 4u) return fail("RSFP invalid baseInterfaces");
            iface.baseInterfaces.reserve(biCount);
            for (std::uint32_t j = 0u; j < biCount; ++j) {
                iface.baseInterfaces.push_back(r.strFromTable(strings));
            }
            iface.baseInterfaceSpans.resize(biCount);
            const std::uint32_t imCount = r.u32();
            if (!r.valid() || imCount > 65536u) return fail("RSFP invalid interface methods");
            iface.methods.resize(imCount);
            for (std::uint32_t j = 0u; j < imCount; ++j) {
                auto& im = iface.methods[j];
                im.name = r.strFromTable(strings);
                im.returnType.type = static_cast<semantic::PrimitiveType>(r.u8());
                im.returnType.typeName = r.strFromTable(strings);
                const std::uint32_t ipCount = r.u32();
                if (!r.valid() || ipCount > 4096u) return fail("RSFP invalid interface method params");
                im.parameters.resize(ipCount);
                for (std::uint32_t k = 0u; k < ipCount; ++k) {
                    im.parameters[k].type = static_cast<semantic::PrimitiveType>(r.u8());
                    im.parameters[k].typeName = r.strFromTable(strings);
                }
                const std::uint32_t ipmCount = r.u32();
                if (!r.valid() || ipmCount > 4096u) return fail("RSFP invalid interface method param mods");
                im.parameterModifiers.resize(ipmCount);
                for (std::uint32_t k = 0u; k < ipmCount; ++k) {
                    im.parameterModifiers[k] = static_cast<semantic::ParameterModifier>(r.u8());
                }
                im.hasDefaultImplementation = r.boolByte();
                const std::uint32_t imHygCount = r.u32();
                if (!r.valid() || imHygCount > r.remaining() / 4u) return fail("RSFP invalid im hyg");
                for (std::uint32_t k = 0u; k < imHygCount; ++k) {
                    im.hygienicModules.insert(r.strFromTable(strings));
                }
            }
        }

        const std::uint32_t delCount = r.u32();
        if (!r.valid() || delCount > 65536u) return fail("RSFP invalid delegates");
        mod.delegates.resize(delCount);
        for (std::uint32_t i = 0u; i < delCount; ++i) {
            auto& del = mod.delegates[i];
            del.moduleName = r.strFromTable(strings);
            del.name = r.strFromTable(strings);
            del.returnType.type = static_cast<semantic::PrimitiveType>(r.u8());
            del.returnType.typeName = r.strFromTable(strings);
            const std::uint32_t dpCount = r.u32();
            if (!r.valid() || dpCount > 4096u) return fail("RSFP invalid delegate params");
            del.parameters.resize(dpCount);
            for (std::uint32_t k = 0u; k < dpCount; ++k) {
                del.parameters[k].type = static_cast<semantic::PrimitiveType>(r.u8());
                del.parameters[k].typeName = r.strFromTable(strings);
            }
            const std::uint32_t dpmCount = r.u32();
            if (!r.valid() || dpmCount > 4096u) return fail("RSFP invalid delegate param mods");
            del.parameterModifiers.resize(dpmCount);
            for (std::uint32_t k = 0u; k < dpmCount; ++k) {
                del.parameterModifiers[k] = static_cast<semantic::ParameterModifier>(r.u8());
            }
        }

        const std::uint32_t declCount = r.u32();
        if (!r.valid() || declCount > 1000000u) return fail("RSFP invalid declarations");
        mod.declarations.resize(declCount);
        for (std::uint32_t i = 0u; i < declCount; ++i) {
            if (!readFunctionCompact(r, strings, mod.declarations[i])) return fail("RSFP truncated declaration");
        }

        const std::uint32_t attrCount = r.u32();
        if (!r.valid() || attrCount > 65536u) return fail("RSFP invalid attrs");
        mod.nativeAttributes.resize(attrCount);
        for (std::uint32_t i = 0u; i < attrCount; ++i) {
            auto& a = mod.nativeAttributes[i];
            a.target = r.strFromTable(strings);
            a.name = r.strFromTable(strings);
            const std::uint32_t argCount = r.u32();
            if (!r.valid() || argCount > 1024u) return fail("RSFP invalid attr args");
            a.arguments.resize(argCount);
            for (std::uint32_t k = 0u; k < argCount; ++k) {
                a.arguments[k].name = r.strFromTable(strings);
                a.arguments[k].value = r.strFromTable(strings);
            }
            a.offset = static_cast<std::size_t>(r.u32());
        }

        const std::uint32_t seqCount = r.u32();
        if (!r.valid() || seqCount > 65536u) return fail("RSFP invalid sequences");
        mod.nativeSequences.resize(seqCount);
        for (std::uint32_t i = 0u; i < seqCount; ++i) {
            auto& seq = mod.nativeSequences[i];
            seq.typeName = r.strFromTable(strings);
            seq.name = r.strFromTable(strings);
            seq.resultTypeName = r.strFromTable(strings);
            const std::uint32_t cbCount = r.u32();
            if (!r.valid() || cbCount > r.remaining() / 4u) return fail("RSFP invalid seq callbacks");
            seq.callbacks.reserve(cbCount);
            for (std::uint32_t k = 0u; k < cbCount; ++k) seq.callbacks.push_back(r.strFromTable(strings));
            seq.offset = static_cast<std::size_t>(r.u32());
        }

        const std::uint32_t implCount = r.u32();
        if (!r.valid() || implCount > 65536u) return fail("RSFP invalid interface impls");
        mod.nativeInterfaces.resize(implCount);
        for (std::uint32_t i = 0u; i < implCount; ++i) {
            auto& impl = mod.nativeInterfaces[i];
            impl.typeName = r.strFromTable(strings);
            const std::uint32_t inCount = r.u32();
            if (!r.valid() || inCount > r.remaining() / 4u) return fail("RSFP invalid impl interfaces");
            impl.interfaces.reserve(inCount);
            for (std::uint32_t k = 0u; k < inCount; ++k) impl.interfaces.push_back(r.strFromTable(strings));
        }

        const std::uint32_t instCount = r.u32();
        if (!r.valid() || instCount > 65536u) return fail("RSFP invalid generic instantiations");
        mod.nativeGenericInstantiations.resize(instCount);
        for (std::uint32_t i = 0u; i < instCount; ++i) {
            auto& inst = mod.nativeGenericInstantiations[i];
            inst.genericName = r.strFromTable(strings);
            const std::uint32_t gaCount = r.u32();
            if (!r.valid() || gaCount > r.remaining() / 4u) return fail("RSFP invalid inst args");
            inst.arguments.reserve(gaCount);
            for (std::uint32_t k = 0u; k < gaCount; ++k) inst.arguments.push_back(r.strFromTable(strings));
            inst.generatedName = r.strFromTable(strings);
        }
    }

    if (!r.valid() || r.remaining() != 0u) {
        return fail("RSFP directory trailing or corrupt data");
    }
    storage->totalIndexedTypes = runningSlotOffset;
    storage->materializedSlots.resize(runningSlotOffset);
    return true;
}

} // namespace realscript::compiler
