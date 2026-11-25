#include "realscript/bytecode/Bytecode.h"
#include "realscript/Version.h"
#include "realscript/bytecode/Lz4Codec.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace realscript::bytecode {
namespace {

constexpr std::uint32_t InvalidIndex = 0xffffffffu;
constexpr std::uint32_t SectionStrings = 1;
constexpr std::uint32_t SectionTypes = 2;
constexpr std::uint32_t SectionReferences = 3;
constexpr std::uint32_t SectionFunctions = 4;
constexpr std::uint32_t SectionCode = 5;
constexpr std::uint32_t SectionDebug = 6;
constexpr std::uint32_t SectionMetadata = 7;
constexpr std::uint32_t SectionObjectModel = 8;
constexpr std::uint32_t LegacySectionCount = 7;
constexpr std::uint32_t SectionCount = 8;

bool hasObjectModelSection(const Version& version) noexcept {
    return version.major == 0 && version.minor >= 7;
}

bool hasSequenceResultMetadata(const Version& version) noexcept {
    return version.major == 0 && version.minor >= 8;
}

constexpr std::uint32_t kInlineMetadataTrailerMagic = 0x52534944u; // "RSID"
constexpr std::uint32_t kFunctionAccessMetadataTrailerMagic = 0x52534641u; // "RSFA"
constexpr std::uint32_t kLineMapDebugTrailerMagic = 0x52534C4Du; // "RSLM"
constexpr std::uint32_t kTypeFieldSchemaTrailerMagic = 0x52534653u; // "RSFS"

[[nodiscard]] bool hasTypeFieldSchemaMetadata(const Module& module) {
    for (const auto& type : module.types) {
        if (type.blittable || type.hasBlittableAttribute) {
            return true;
        }
        for (const auto& field : type.fields) {
            if (field.fieldTag.has_value() || !field.formerlySerializedAs.empty()) {
                return true;
            }
        }
    }
    return false;
}

bool hasInlineDependencyMetadata(const Module& mod) noexcept {
    return mod.version.major == 0 && mod.version.minor >= 7 &&
        (!mod.languageMetadata.inlinedDependencies.empty() ||
         !mod.languageMetadata.functionInlineStamps.empty());
}

bool hasExceptionMetadata(const Version& version) noexcept {
    return version.major == 0 && version.minor >= 9;
}

class Writer {
public:
    void u8(std::uint8_t value) { data_.push_back(value); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void f64(double value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }
    void bytes(const char* values, std::size_t count) {
        data_.insert(data_.end(), values, values + count);
    }
    void bytes(const std::vector<std::uint8_t>& values) {
        data_.insert(data_.end(), values.begin(), values.end());
    }
    void patchU32(std::size_t offset, std::uint32_t value) {
        if (offset + 4 > data_.size()) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            throw std::logic_error("bytecode writer patch is outside buffer");
#else
            std::abort();
#endif
        }
        for (int shift = 0; shift < 32; shift += 8) {
            data_[offset++] = static_cast<std::uint8_t>(value >> shift);
        }
    }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(data_); }
private:
    std::vector<std::uint8_t> data_;
};

class Reader {
public:
    Reader(const std::uint8_t* base, std::size_t totalSize, std::size_t begin, std::size_t size) noexcept {
        if ((!base && (begin != 0u || size != 0u)) || begin > totalSize || size > totalSize - begin) {
            valid_ = false;
            ptr_ = nullptr;
            end_ = nullptr;
        } else {
            ptr_ = base ? (base + begin) : nullptr;
            end_ = base ? (base + begin + size) : nullptr;
        }
    }
    Reader(const std::vector<std::uint8_t>& data, std::size_t begin, std::size_t size) noexcept
        : Reader(data.data(), data.size(), begin, size) {}
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool empty() const noexcept { return ptr_ == end_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return valid_ && ptr_ <= end_ ? static_cast<std::size_t>(end_ - ptr_) : 0u;
    }
    [[nodiscard]] std::uint32_t peekU32() const noexcept {
        if (!valid_ || static_cast<std::size_t>(end_ - ptr_) < 4u) {
            return 0u;
        }
        return readLeU32(ptr_);
    }
    Reader subReader(std::size_t length) noexcept {
        if (!require(length)) {
            return Reader(nullptr, 0u, 1u, 0u);
        }
        const auto* begin = ptr_;
        ptr_ += length;
        return Reader(begin, length, 0u, length);
    }
    std::uint8_t u8() noexcept {
        if (!require(1u)) return 0u;
        return *ptr_++;
    }
    std::uint16_t u16() noexcept {
        if (!require(2u)) return 0u;
        const std::uint16_t result = readLeU16(ptr_);
        ptr_ += 2u;
        return result;
    }
    std::uint32_t u32() noexcept {
        if (!require(4u)) return 0u;
        const std::uint32_t result = readLeU32(ptr_);
        ptr_ += 4u;
        return result;
    }
    std::uint64_t u64() noexcept {
        if (!require(8u)) return 0u;
        const std::uint64_t result = readLeU64(ptr_);
        ptr_ += 8u;
        return result;
    }
    std::int64_t i64() noexcept { return static_cast<std::int64_t>(u64()); }
    double f64() noexcept {
        const auto bits = u64();
        double value = 0.0;
        static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    bool readBulkU32(std::uint32_t* dst, std::size_t count) noexcept {
        if (count == 0u) return valid_;
        if (!valid_ || count > remaining() / 4u) {
            valid_ = false;
            return false;
        }
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        std::memcpy(dst, ptr_, count * sizeof(std::uint32_t));
        ptr_ += count * sizeof(std::uint32_t);
#else
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = readLeU32(ptr_);
            ptr_ += 4u;
        }
#endif
        return true;
    }
    bool readBulkU64(std::uint64_t* dst, std::size_t count) noexcept {
        if (count == 0u) return valid_;
        if (!valid_ || count > remaining() / 8u) {
            valid_ = false;
            return false;
        }
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        std::memcpy(dst, ptr_, count * sizeof(std::uint64_t));
        ptr_ += count * sizeof(std::uint64_t);
#else
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = readLeU64(ptr_);
            ptr_ += 8u;
        }
#endif
        return true;
    }
    std::string string(std::size_t count) {
        if (!require(count)) return {};
        if (count == 0u) return {};
        const auto* begin = reinterpret_cast<const char*>(ptr_);
        ptr_ += count;
        return std::string(begin, count);
    }
private:
    static std::uint16_t readLeU16(const std::uint8_t* p) noexcept {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        std::uint16_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return v;
#else
        return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
#endif
    }
    static std::uint32_t readLeU32(const std::uint8_t* p) noexcept {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        std::uint32_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return v;
#else
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
#endif
    }
    static std::uint64_t readLeU64(const std::uint8_t* p) noexcept {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        std::uint64_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return v;
#else
        return static_cast<std::uint64_t>(readLeU32(p)) |
               (static_cast<std::uint64_t>(readLeU32(p + 4)) << 32);
#endif
    }
    bool require(std::size_t count) noexcept {
        if (!valid_ || count > static_cast<std::size_t>(end_ - ptr_)) {
            valid_ = false;
            return false;
        }
        return true;
    }
    const std::uint8_t* ptr_ = nullptr;
    const std::uint8_t* end_ = nullptr;
    bool valid_ = true;
};

std::uint8_t encodeType(semantic::PrimitiveType type) {
    switch (type) {
    case semantic::PrimitiveType::Void: return 0;
    case semantic::PrimitiveType::Bool: return 1;
    case semantic::PrimitiveType::Int: return 2;
    case semantic::PrimitiveType::String: return 3;
    case semantic::PrimitiveType::Object: return 4;
    case semantic::PrimitiveType::Null: return 5;
    case semantic::PrimitiveType::Array: return 6;
    case semantic::PrimitiveType::Handle: return 7;
    case semantic::PrimitiveType::Long: return 8;
    case semantic::PrimitiveType::Double: return 9;
    case semantic::PrimitiveType::Struct: return 10;
    case semantic::PrimitiveType::Enum: return 11;
    case semantic::PrimitiveType::Byte: return 12;
    case semantic::PrimitiveType::SByte: return 13;
    case semantic::PrimitiveType::Short: return 14;
    case semantic::PrimitiveType::UShort: return 15;
    case semantic::PrimitiveType::UInt: return 16;
    case semantic::PrimitiveType::ULong: return 17;
    case semantic::PrimitiveType::Float: return 18;
    case semantic::PrimitiveType::Char: return 19;
    case semantic::PrimitiveType::Error: break;
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::logic_error("invalid type in bytecode encoder");
#else
    std::abort();
#endif
}

bool decodeType(std::uint8_t tag, semantic::PrimitiveType& type) {
    switch (tag) {
    case 0: type = semantic::PrimitiveType::Void; return true;
    case 1: type = semantic::PrimitiveType::Bool; return true;
    case 2: type = semantic::PrimitiveType::Int; return true;
    case 3: type = semantic::PrimitiveType::String; return true;
    case 4: type = semantic::PrimitiveType::Object; return true;
    case 5: type = semantic::PrimitiveType::Null; return true;
    case 6: type = semantic::PrimitiveType::Array; return true;
    case 7: type = semantic::PrimitiveType::Handle; return true;
    case 8: type = semantic::PrimitiveType::Long; return true;
    case 9: type = semantic::PrimitiveType::Double; return true;
    case 10: type = semantic::PrimitiveType::Struct; return true;
    case 11: type = semantic::PrimitiveType::Enum; return true;
    case 12: type = semantic::PrimitiveType::Byte; return true;
    case 13: type = semantic::PrimitiveType::SByte; return true;
    case 14: type = semantic::PrimitiveType::Short; return true;
    case 15: type = semantic::PrimitiveType::UShort; return true;
    case 16: type = semantic::PrimitiveType::UInt; return true;
    case 17: type = semantic::PrimitiveType::ULong; return true;
    case 18: type = semantic::PrimitiveType::Float; return true;
    case 19: type = semantic::PrimitiveType::Char; return true;
    default: return false;
    }
}

std::uint8_t encodeOptionalType(semantic::PrimitiveType type) {
    return type == semantic::PrimitiveType::Error ? 0xffu : encodeType(type);
}

bool decodeOptionalType(std::uint8_t tag, semantic::PrimitiveType& type) {
    if (tag == 0xffu) {
        type = semantic::PrimitiveType::Error;
        return true;
    }
    return decodeType(tag, type);
}

class StringPool {
public:
    std::uint32_t add(const std::string& value) {
        const auto found = indices_.find(value);
        if (found != indices_.end()) return found->second;
        const auto index = static_cast<std::uint32_t>(values_.size());
        values_.push_back(value);
        indices_.emplace(value, index);
        return index;
    }
    [[nodiscard]] std::uint32_t indexOf(const std::string& value) const {
        const auto found = indices_.find(value);
        if (found == indices_.end()) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            throw std::logic_error("missing string in bytecode pool");
#else
            std::abort();
#endif
        }
        return found->second;
    }
    [[nodiscard]] const std::vector<std::string>& values() const noexcept {
        return values_;
    }
private:
    std::vector<std::string> values_;
    std::unordered_map<std::string, std::uint32_t> indices_;
};

void writeTypes(Writer& writer, const std::vector<semantic::PrimitiveType>& types) {
    writer.u32(static_cast<std::uint32_t>(types.size()));
    for (const auto type : types) writer.u8(encodeType(type));
}

void writeTypeIds(
    Writer& writer,
    const std::vector<semantic::SymbolId>& typeIds) {
    writer.u32(static_cast<std::uint32_t>(typeIds.size()));
    for (const auto typeId : typeIds) writer.u64(typeId);
}

bool readTypes(
    Reader& reader,
    std::vector<semantic::PrimitiveType>& types,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining()) {
        diagnostics.report("RS5003", "truncated bytecode type list", {});
        return false;
    }
    types.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        semantic::PrimitiveType type;
        if (!decodeType(reader.u8(), type)) {
            diagnostics.report("RS5004", "invalid bytecode type tag", {});
            return false;
        }
        types.push_back(type);
    }
    return reader.valid();
}

bool readTypeIds(
    Reader& reader,
    std::vector<semantic::SymbolId>& typeIds,
    diagnostics::DiagnosticBag& diagnostics) {
    static_assert(
        sizeof(semantic::SymbolId) == sizeof(std::uint64_t) &&
        std::is_trivially_copyable_v<semantic::SymbolId>,
        "SymbolId must be 64-bit trivially copyable");
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining() / 8) {
        diagnostics.report("RS5003", "truncated bytecode type ID list", {});
        return false;
    }
    if (count == 0u) return reader.valid();
    const std::size_t oldSize = typeIds.size();
    typeIds.resize(oldSize + count);
    return reader.readBulkU64(
        reinterpret_cast<std::uint64_t*>(typeIds.data() + oldSize),
        count);
}

template <typename RegContainer>
void writeRegisters(Writer& writer, const RegContainer& values) {
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (const auto value : values) writer.u32(value);
}

template <typename RegContainer>
bool readRegisters(Reader& reader, RegContainer& values) {
    static_assert(
        sizeof(Register) == sizeof(std::uint32_t) &&
        std::is_trivially_copyable_v<Register>,
        "Register must be 32-bit trivially copyable");
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining() / 4) return false;
    if (count == 0u) return reader.valid();
    const std::size_t oldSize = values.size();
    values.resize(oldSize + count);
    return reader.readBulkU32(
        reinterpret_cast<std::uint32_t*>(values.data() + oldSize),
        count);
}

bool readRegisters(Reader& reader, SmallRegisterVector& values) {
    static_assert(
        sizeof(Register) == sizeof(std::uint32_t) &&
        std::is_trivially_copyable_v<Register>,
        "Register must be 32-bit trivially copyable");
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining() / 4 || count > 0xFFFu) return false;
    if (count == 0u) {
        values.clear();
        return reader.valid();
    }
    if (count <= 64u) {
        Register stackRegs[64];
        if (!reader.readBulkU32(reinterpret_cast<std::uint32_t*>(stackRegs), count)) {
            return false;
        }
        values.assign(stackRegs, count);
        return true;
    }
    std::vector<Register> heapRegs(count);
    if (!reader.readBulkU32(reinterpret_cast<std::uint32_t*>(heapRegs.data()), count)) {
        return false;
    }
    values.assign(heapRegs.data(), heapRegs.size());
    return true;
}

void writeRange(Writer& writer, const debug::SourceRange& range) {
    writer.u32(range.fileId);
    writer.u64(static_cast<std::uint64_t>(range.span.start));
    writer.u64(static_cast<std::uint64_t>(range.span.length));
    writer.u32(static_cast<std::uint32_t>(range.start.line));
    writer.u32(static_cast<std::uint32_t>(range.start.column));
    writer.u32(static_cast<std::uint32_t>(range.end.line));
    writer.u32(static_cast<std::uint32_t>(range.end.column));
}

bool readRange(Reader& reader, debug::SourceRange& range) {
    range.fileId = reader.u32();
    range.span.start = static_cast<std::size_t>(reader.u64());
    range.span.length = static_cast<std::size_t>(reader.u64());
    range.start.line = reader.u32();
    range.start.column = reader.u32();
    range.end.line = reader.u32();
    range.end.column = reader.u32();
    return reader.valid();
}

struct SectionEntry {
    std::uint32_t kind = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct FunctionRecord {
    Function function;
    std::uint32_t codeOffset = 0;
    std::uint32_t codeSize = 0;
};

struct StringLookup {
    const std::vector<std::string>* vec = nullptr;
    const LazyBytecodeBundle* bundle = nullptr;

    StringLookup() noexcept = default;
    StringLookup(const std::vector<std::string>& strings) noexcept : vec(&strings) {}
    StringLookup(const LazyBytecodeBundle& b) noexcept : bundle(&b) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return bundle ? bundle->stringCount() : (vec ? vec->size() : 0u);
    }

    [[nodiscard]] bool validIndex(std::uint32_t index) const noexcept {
        return static_cast<std::size_t>(index) < size();
    }

    [[nodiscard]] std::string copyAt(std::uint32_t index) const {
        if (bundle) {
            return bundle->stringCopyAt(index);
        }
        if (vec && index < vec->size()) {
            return (*vec)[index];
        }
        return {};
    }
};

bool validStringIndex(
    std::uint32_t index,
    const StringLookup& strings,
    diagnostics::DiagnosticBag& diagnostics,
    const char* message) {
    if (!strings.validIndex(index)) {
        diagnostics.report("RS5007", message, {});
        return false;
    }
    return true;
}

void addVariableStrings(
    StringPool& strings,
    const semantic::VariableSymbol& variable) {
    strings.add(variable.name);
    strings.add(variable.typeName);
    strings.add(variable.storageTypeName);
}

void addFunctionStrings(
    StringPool& strings,
    const semantic::FunctionSymbol& function) {
    strings.add(function.declaringTypeName);
    strings.add(function.moduleName);
    strings.add(function.name);
    strings.add(function.ownerTypeName);
    strings.add(function.returnTypeName);
    strings.add(function.sourceName);
    for (const auto& parameter : function.parameters) {
        addVariableStrings(strings, parameter);
    }
}

void addPropertyStrings(
    StringPool& strings,
    const semantic::PropertySymbol& property) {
    strings.add(property.name);
    strings.add(property.declaringTypeName);
    strings.add(property.typeName);
    strings.add(property.sourceName);
    if (property.getter) addFunctionStrings(strings, *property.getter);
    if (property.setter) addFunctionStrings(strings, *property.setter);
}

void writeFunctionSymbol(
    Writer& writer,
    const StringPool& strings,
    const semantic::FunctionSymbol& function) {
    writer.u64(function.id);
    writer.u8(static_cast<std::uint8_t>(function.accessibility));
    writer.u64(function.declaringTypeId);
    writer.u32(strings.indexOf(function.declaringTypeName));
    std::uint8_t flags = 0;
    if (function.virtualMethod) flags |= 0x01u;
    if (function.overrideMethod) flags |= 0x02u;
    if (function.abstractMethod) flags |= 0x04u;
    if (function.sealedMethod) flags |= 0x08u;
    if (function.interfaceMethod) flags |= 0x10u;
    if (function.method) flags |= 0x20u;
    if (function.staticMethod) flags |= 0x40u;
    if (function.constructor) flags |= 0x80u;
    writer.u8(flags);
    std::uint8_t secondaryFlags = 0;
    if (function.propertyGetter) secondaryFlags |= 0x01u;
    if (function.propertySetter) secondaryFlags |= 0x02u;
    if (function.synthetic) secondaryFlags |= 0x04u;
    writer.u8(secondaryFlags);
    writer.u32(function.virtualSlot);
    writer.u32(function.interfaceSlot);
    writer.u32(strings.indexOf(function.moduleName));
    writer.u32(strings.indexOf(function.name));
    writer.u32(strings.indexOf(function.ownerTypeName));
    writer.u64(function.ownerTypeId);
    writer.u8(encodeType(function.returnType));
    writer.u32(strings.indexOf(function.returnTypeName));
    writer.u32(strings.indexOf(function.sourceName));
    writer.u32(static_cast<std::uint32_t>(function.parameters.size()));
    for (const auto& parameter : function.parameters) {
        writer.u32(strings.indexOf(parameter.name));
        writer.u8(encodeType(parameter.type));
        writer.u32(strings.indexOf(parameter.typeName));
        writer.u8(encodeOptionalType(parameter.storageType));
        writer.u32(strings.indexOf(parameter.storageTypeName));
        writer.u8(static_cast<std::uint8_t>(parameter.modifier));
        writer.u64(static_cast<std::uint64_t>(parameter.index));
        writer.u8(parameter.parameter ? 1 : 0);
        writer.u64(parameter.id);
    }
}

bool readFunctionSymbol(
    Reader& reader,
    const std::vector<std::string>& strings,
    semantic::FunctionSymbol& function,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto readString = [&](std::string& value, const char* message) {
        const auto index = reader.u32();
        if (!validStringIndex(index, strings, diagnostics, message)) {
            return false;
        }
        value = strings[index];
        return true;
    };

    function.id = reader.u64();
    const auto accessibility = reader.u8();
    function.declaringTypeId = reader.u64();
    if (!readString(
            function.declaringTypeName,
            "invalid method declaring-type string index")) {
        return false;
    }
    const auto flags = reader.u8();
    const auto secondaryFlags = reader.u8();
    if (!reader.valid() ||
        accessibility > static_cast<std::uint8_t>(
            semantic::Accessibility::Private) ||
        (secondaryFlags & 0xf8u) != 0) {
        diagnostics.report("RS5004", "invalid method descriptor flags", {});
        return false;
    }
    function.accessibility = static_cast<semantic::Accessibility>(
        accessibility);
    function.virtualMethod = (flags & 0x01u) != 0;
    function.overrideMethod = (flags & 0x02u) != 0;
    function.abstractMethod = (flags & 0x04u) != 0;
    function.sealedMethod = (flags & 0x08u) != 0;
    function.interfaceMethod = (flags & 0x10u) != 0;
    function.method = (flags & 0x20u) != 0;
    function.staticMethod = (flags & 0x40u) != 0;
    function.constructor = (flags & 0x80u) != 0;
    function.propertyGetter = (secondaryFlags & 0x01u) != 0;
    function.propertySetter = (secondaryFlags & 0x02u) != 0;
    function.synthetic = (secondaryFlags & 0x04u) != 0;
    function.virtualSlot = reader.u32();
    function.interfaceSlot = reader.u32();
    if (!readString(function.moduleName, "invalid method module string index") ||
        !readString(function.name, "invalid method name string index") ||
        !readString(
            function.ownerTypeName,
            "invalid method owner-type string index")) {
        return false;
    }
    function.ownerTypeId = reader.u64();
    if (!decodeType(reader.u8(), function.returnType) ||
        !readString(
            function.returnTypeName,
            "invalid method return type-name string index") ||
        !readString(function.sourceName, "invalid method source string index")) {
        diagnostics.report("RS5004", "invalid method return descriptor", {});
        return false;
    }

    const auto parameterCount = reader.u32();
    if (!reader.valid() || parameterCount > reader.remaining() / 35) {
        diagnostics.report("RS5003", "truncated method parameter list", {});
        return false;
    }
    function.parameters.reserve(parameterCount);
    for (std::uint32_t index = 0; index < parameterCount; ++index) {
        semantic::VariableSymbol parameter;
        if (!readString(parameter.name, "invalid parameter name string index") ||
            !decodeType(reader.u8(), parameter.type) ||
            !readString(
                parameter.typeName,
                "invalid parameter type-name string index") ||
            !decodeOptionalType(reader.u8(), parameter.storageType) ||
            !readString(
                parameter.storageTypeName,
                "invalid parameter storage type-name string index")) {
            diagnostics.report("RS5004", "invalid method parameter descriptor", {});
            return false;
        }
        const auto modifier = reader.u8();
        parameter.index = static_cast<std::size_t>(reader.u64());
        const auto parameterFlag = reader.u8();
        parameter.id = reader.u64();
        if (!reader.valid() ||
            modifier > static_cast<std::uint8_t>(
                semantic::ParameterModifier::In) ||
            parameterFlag > 1) {
            diagnostics.report("RS5004", "invalid method parameter flags", {});
            return false;
        }
        parameter.modifier = static_cast<semantic::ParameterModifier>(modifier);
        parameter.parameter = parameterFlag != 0;
        function.parameters.push_back(std::move(parameter));
    }
    return reader.valid();
}

void writePropertySymbol(
    Writer& writer,
    const StringPool& strings,
    const semantic::PropertySymbol& property) {
    writer.u32(strings.indexOf(property.name));
    writer.u8(static_cast<std::uint8_t>(property.accessibility));
    writer.u64(property.declaringTypeId);
    writer.u32(strings.indexOf(property.declaringTypeName));
    writer.u8(encodeType(property.type));
    writer.u32(strings.indexOf(property.typeName));
    writer.u8(property.staticProperty ? 1 : 0);
    writer.u8(property.getter ? 1 : 0);
    if (property.getter) {
        writeFunctionSymbol(writer, strings, *property.getter);
    }
    writer.u8(property.setter ? 1 : 0);
    if (property.setter) {
        writeFunctionSymbol(writer, strings, *property.setter);
    }
    writer.u64(static_cast<std::uint64_t>(property.backingFieldIndex));
    writer.u32(strings.indexOf(property.sourceName));
    writer.u64(property.id);
}

bool readPropertySymbol(
    Reader& reader,
    const std::vector<std::string>& strings,
    semantic::PropertySymbol& property,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto readString = [&](std::string& value, const char* message) {
        const auto index = reader.u32();
        if (!validStringIndex(index, strings, diagnostics, message)) {
            return false;
        }
        value = strings[index];
        return true;
    };
    if (!readString(property.name, "invalid property name string index")) {
        return false;
    }
    const auto accessibility = reader.u8();
    property.declaringTypeId = reader.u64();
    if (!readString(
            property.declaringTypeName,
            "invalid property declaring-type string index") ||
        !decodeType(reader.u8(), property.type) ||
        !readString(
            property.typeName,
            "invalid property type-name string index")) {
        diagnostics.report("RS5004", "invalid property descriptor", {});
        return false;
    }
    const auto staticProperty = reader.u8();
    const auto hasGetter = reader.u8();
    if (!reader.valid() ||
        accessibility > static_cast<std::uint8_t>(
            semantic::Accessibility::Private) ||
        staticProperty > 1 || hasGetter > 1) {
        diagnostics.report("RS5004", "invalid property flags", {});
        return false;
    }
    property.accessibility = static_cast<semantic::Accessibility>(
        accessibility);
    property.staticProperty = staticProperty != 0;
    if (hasGetter != 0) {
        semantic::FunctionSymbol getter;
        if (!readFunctionSymbol(reader, strings, getter, diagnostics)) {
            return false;
        }
        property.getter = std::move(getter);
    }
    const auto hasSetter = reader.u8();
    if (!reader.valid() || hasSetter > 1) {
        diagnostics.report("RS5004", "invalid property setter flag", {});
        return false;
    }
    if (hasSetter != 0) {
        semantic::FunctionSymbol setter;
        if (!readFunctionSymbol(reader, strings, setter, diagnostics)) {
            return false;
        }
        property.setter = std::move(setter);
    }
    property.backingFieldIndex = static_cast<std::size_t>(reader.u64());
    if (!readString(
            property.sourceName,
            "invalid property source string index")) {
        return false;
    }
    property.id = reader.u64();
    return reader.valid();
}

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct DecodedFieldSchema {
    std::uint32_t fieldIndex = 0;
    std::optional<std::int32_t> fieldTag;
    std::vector<std::string> formerlySerializedAs;
};

struct DecodedTypeSchema {
    std::uint32_t typeIndex = 0;
    std::uint8_t flags = 0;
    std::vector<DecodedFieldSchema> fields;
};

void collectModuleStrings(StringPool& strings, const Module& module) {
    strings.add(module.name);
    for (const auto& type : module.types) {
        strings.add(type.moduleName);
        strings.add(type.name);
        if (!type.baseTypeName.empty()) strings.add(type.baseTypeName);
        for (const auto& bi : type.baseInterfaceNames) {
            if (!bi.empty()) strings.add(bi);
        }
        for (const auto& field : type.fields) {
            strings.add(field.declaringTypeName);
            strings.add(field.sourceName);
            if (field.hasConstantValue &&
                std::holds_alternative<std::string>(field.constantValue)) {
                strings.add(std::get<std::string>(field.constantValue));
            }
        }
        for (const auto& method : type.methods) addFunctionStrings(strings, method);
        for (const auto& constructor : type.constructors) addFunctionStrings(strings, constructor);
        if (type.staticConstructor) addFunctionStrings(strings, *type.staticConstructor);
        for (const auto& property : type.properties) addPropertyStrings(strings, property);
        for (const auto& field : type.fields) {
            strings.add(field.name);
            if (!field.typeName.empty()) strings.add(field.typeName);
        }
        for (const auto& member : type.enumMembers) strings.add(member.name);
    }
    for (const auto& source : module.sourceFiles) strings.add(source.path);
    for (const auto& reference : module.functionReferences) strings.add(reference.name);
    for (const auto& function : module.functions) {
        strings.add(function.name);
        for (const auto& local : function.debugInfo.locals) strings.add(local.name);
        for (const auto& point : function.debugInfo.sequencePoints) {
            if (!point.mappedPath.empty()) strings.add(point.mappedPath);
        }
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode == Opcode::ConstantString) {
                    strings.add(instruction.stringImmediate);
                }
            }
        }
    }
    for (const auto& attribute : module.languageMetadata.attributes) {
        strings.add(attribute.target);
        strings.add(attribute.name);
        strings.add(attribute.sourceName);
        for (const auto& argument : attribute.arguments) {
            strings.add(argument.name);
            strings.add(argument.value);
        }
    }
    for (const auto& implementation : module.languageMetadata.interfaces) {
        strings.add(implementation.typeName);
        for (const auto& name : implementation.interfaces) strings.add(name);
    }
    for (const auto& instantiation : module.languageMetadata.genericInstantiations) {
        strings.add(instantiation.genericName);
        strings.add(instantiation.generatedName);
        for (const auto& argument : instantiation.arguments) strings.add(argument);
    }
    for (const auto& sequence : module.languageMetadata.sequences) {
        strings.add(sequence.typeName);
        strings.add(sequence.name);
        if (hasSequenceResultMetadata(module.version)) {
            strings.add(sequence.resultTypeName);
        }
        strings.add(sequence.sourceName);
        for (const auto& callback : sequence.callbacks) strings.add(callback);
    }
    if (hasInlineDependencyMetadata(module)) {
        for (const auto& dep : module.languageMetadata.inlinedDependencies) {
            strings.add(dep.callerFunction);
            strings.add(dep.calleeModule);
            strings.add(dep.calleeFunction);
        }
    }
    if (hasTypeFieldSchemaMetadata(module)) {
        for (const auto& type : module.types) {
            for (const auto& field : type.fields) {
                for (const auto& alias : field.formerlySerializedAs) {
                    strings.add(alias);
                }
            }
        }
    }
}

void writeStringTable(Writer& out, const StringPool& strings) {
    out.u32(static_cast<std::uint32_t>(strings.values().size()));
    for (const auto& value : strings.values()) {
        out.u32(static_cast<std::uint32_t>(value.size()));
        out.bytes(value.data(), value.size());
    }
}

void writeStringTableV3(Writer& out, const StringPool& strings) {
    const auto& vals = strings.values();
    std::size_t totalChars = 0u;
    for (const auto& value : vals) {
        totalChars += value.size();
    }
    out.u32(static_cast<std::uint32_t>(vals.size()));
    out.u32(static_cast<std::uint32_t>(totalChars));
    for (const auto& value : vals) {
        out.u32(static_cast<std::uint32_t>(value.size()));
    }
    for (const auto& value : vals) {
        if (!value.empty()) {
            out.bytes(value.data(), value.size());
        }
    }
}

bool readFlatStringPoolV3(
    Reader& reader,
    LazyBytecodeBundle& outBundle,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto count = reader.u32();
    const auto totalChars = reader.u32();
    if (!reader.valid() ||
        count == 0u ||
        count > reader.remaining() / 4u ||
        static_cast<std::size_t>(totalChars) > reader.remaining() - static_cast<std::size_t>(count) * 4u) {
        diagnostics.report("RS5003", "truncated v3 flat string pool", {});
        return false;
    }
    outBundle.flatStringSpans.resize(count);
    std::size_t runningOffset = 0u;
    for (std::uint32_t index = 0u; index < count; ++index) {
        const auto len = reader.u32();
        if (!reader.valid() || static_cast<std::size_t>(len) > static_cast<std::size_t>(totalChars) - runningOffset) {
            diagnostics.report("RS5003", "invalid v3 flat string span", {});
            outBundle.flatStringSpans.clear();
            return false;
        }
        outBundle.flatStringSpans[index] = LazyStringSpan{
            static_cast<std::uint32_t>(runningOffset),
            len};
        runningOffset += len;
    }
    if (runningOffset != static_cast<std::size_t>(totalChars)) {
        diagnostics.report("RS5003", "v3 flat string byte total mismatch", {});
        outBundle.flatStringSpans.clear();
        return false;
    }
    outBundle.flatStringData.resize(totalChars);
    for (std::uint32_t i = 0u; i < totalChars; ++i) {
        outBundle.flatStringData[i] = static_cast<char>(reader.u8());
    }
    if (!reader.valid()) {
        diagnostics.report("RS5003", "truncated v3 flat string bytes", {});
        outBundle.flatStringSpans.clear();
        outBundle.flatStringData.clear();
        return false;
    }
    return true;
}

bool readStringTable(
    Reader& reader,
    std::vector<std::string>& strings,
    diagnostics::DiagnosticBag& diagnostics,
    bool requireNonEmptyAndFullConsume) {
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining() / 4) {
        diagnostics.report("RS5003", "truncated string section", {});
        return false;
    }
    strings.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto length = reader.u32();
        strings.push_back(reader.string(length));
        if (!reader.valid()) {
            diagnostics.report("RS5003", "truncated bytecode string", {});
            return false;
        }
    }
    if (requireNonEmptyAndFullConsume && !reader.empty()) {
        diagnostics.report("RS5002", "string section has trailing bytes", {});
        return false;
    }
    if (strings.empty()) {
        diagnostics.report("RS5007", "bytecode string table is empty", {});
        return false;
    }
    return true;
}

void writeTypeSectionBody(
    Writer& typeSection,
    const StringPool& strings,
    const Module& module) {
    typeSection.u32(static_cast<std::uint32_t>(module.types.size()));
    for (const auto& type : module.types) {
        typeSection.u64(type.id);
        typeSection.u8(static_cast<std::uint8_t>(type.kind));
        typeSection.u32(strings.indexOf(type.moduleName));
        typeSection.u32(strings.indexOf(type.name));
        typeSection.u32(static_cast<std::uint32_t>(type.fields.size()));
        for (const auto& field : type.fields) {
            typeSection.u32(strings.indexOf(field.name));
            typeSection.u8(encodeType(field.type));
            typeSection.u32(field.typeName.empty()
                ? InvalidIndex
                : strings.indexOf(field.typeName));
            typeSection.u32(static_cast<std::uint32_t>(field.index));
            std::uint8_t fieldFlags = 0;
            if (field.synthetic) fieldFlags |= 0x01u;
            if (field.staticField) fieldFlags |= 0x02u;
            if (field.constField) fieldFlags |= 0x04u;
            if (field.readonlyField) fieldFlags |= 0x08u;
            typeSection.u8(fieldFlags);
        }
        typeSection.u32(static_cast<std::uint32_t>(type.enumMembers.size()));
        for (const auto& member : type.enumMembers) {
            typeSection.u32(strings.indexOf(member.name));
            typeSection.i64(member.value);
        }
    }
}

bool readTypeSectionBody(
    Reader& reader,
    const std::vector<std::string>& strings,
    Module& module,
    const std::vector<DecodedTypeSchema>& decodedTypeSchemas,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining() / 24 + 1) {
        diagnostics.report("RS5003", "truncated type descriptor section", {});
        return false;
    }
    module.types.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        semantic::TypeSymbol type;
        type.id = reader.u64();
        const auto kind = reader.u8();
        if (kind > static_cast<std::uint8_t>(semantic::TypeKind::Enum)) {
            diagnostics.report("RS5004", "invalid type descriptor kind", {});
            return false;
        }
        type.kind = static_cast<semantic::TypeKind>(kind);
        const auto moduleNameIndex = reader.u32();
        const auto nameIndex = reader.u32();
        if (!validStringIndex(moduleNameIndex, strings, diagnostics,
                "invalid type module string index") ||
            !validStringIndex(nameIndex, strings, diagnostics,
                "invalid type name string index")) {
            return false;
        }
        type.moduleName = strings[moduleNameIndex];
        type.name = strings[nameIndex];
        const auto fieldCount = reader.u32();
        if (!reader.valid() || fieldCount > reader.remaining() / 13) {
            diagnostics.report("RS5003", "truncated field descriptor list", {});
            return false;
        }
        type.fields.reserve(fieldCount);
        for (std::uint32_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
            semantic::FieldSymbol field;
            const auto fieldNameIndex = reader.u32();
            if (!validStringIndex(fieldNameIndex, strings, diagnostics,
                    "invalid field name string index")) {
                return false;
            }
            field.name = strings[fieldNameIndex];
            if (!decodeType(reader.u8(), field.type)) {
                diagnostics.report("RS5004", "invalid field type tag", {});
                return false;
            }
            const auto typeNameIndex = reader.u32();
            if (typeNameIndex != InvalidIndex) {
                if (!validStringIndex(typeNameIndex, strings, diagnostics,
                        "invalid field type-name string index")) {
                    return false;
                }
                field.typeName = strings[typeNameIndex];
            }
            field.index = reader.u32();
            const auto fieldFlags = reader.u8();
            field.synthetic = (fieldFlags & 0x01u) != 0;
            field.staticField = (fieldFlags & 0x02u) != 0;
            field.constField = (fieldFlags & 0x04u) != 0;
            field.readonlyField = (fieldFlags & 0x08u) != 0;
            type.fields.push_back(std::move(field));
        }
        const auto enumCount = reader.u32();
        if (!reader.valid() || enumCount > reader.remaining() / 12) {
            diagnostics.report("RS5003", "truncated enum member descriptor list", {});
            return false;
        }
        type.enumMembers.reserve(enumCount);
        for (std::uint32_t memberIndex = 0; memberIndex < enumCount; ++memberIndex) {
            semantic::EnumMemberSymbol member;
            const auto memberNameIndex = reader.u32();
            if (!validStringIndex(memberNameIndex, strings, diagnostics,
                    "invalid enum member name string index")) {
                return false;
            }
            member.name = strings[memberNameIndex];
            member.value = reader.i64();
            type.enumMembers.push_back(std::move(member));
        }
        module.types.push_back(std::move(type));
    }
    if (!reader.valid()) {
        diagnostics.report("RS5003", "invalid type descriptor section", {});
        return false;
    }
    for (const auto& ts : decodedTypeSchemas) {
        if (ts.typeIndex >= module.types.size()) {
            diagnostics.report("RS5010", "out-of-range type index in schema trailer", {});
            return false;
        }
        auto& targetType = module.types[ts.typeIndex];
        targetType.blittable = (ts.flags & 0x01u) != 0;
        targetType.hasBlittableAttribute = (ts.flags & 0x02u) != 0;
        for (const auto& fs : ts.fields) {
            if (fs.fieldIndex >= targetType.fields.size()) {
                diagnostics.report("RS5010", "out-of-range field index in schema trailer", {});
                return false;
            }
            auto& targetField = targetType.fields[fs.fieldIndex];
            targetField.fieldTag = fs.fieldTag;
            targetField.formerlySerializedAs = fs.formerlySerializedAs;
        }
    }
    return true;
}

void writeReferenceSectionBody(
    Writer& referenceSection,
    const StringPool& strings,
    const Module& module) {
    referenceSection.u32(static_cast<std::uint32_t>(module.functionReferences.size()));
    for (const auto& reference : module.functionReferences) {
        referenceSection.u64(reference.symbolId);
        referenceSection.u32(strings.indexOf(reference.name));
        referenceSection.u8(encodeType(reference.returnType));
        referenceSection.u64(reference.returnTypeId);
        writeTypes(referenceSection, reference.parameterTypes);
        writeTypeIds(referenceSection, reference.parameterTypeIds);
    }
}

bool readReferenceSectionBody(
    Reader& reader,
    const StringLookup& strings,
    Module& module,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining() / 17) {
        diagnostics.report("RS5003", "truncated function reference section", {});
        return false;
    }
    module.functionReferences.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        FunctionReference reference;
        reference.symbolId = reader.u64();
        const auto nameIndex = reader.u32();
        if (!validStringIndex(nameIndex, strings, diagnostics,
                "invalid reference name string index")) {
            return false;
        }
        reference.name = strings.copyAt(nameIndex);
        if (!decodeType(reader.u8(), reference.returnType)) {
            diagnostics.report("RS5004", "invalid function reference return type", {});
            return false;
        }
        reference.returnTypeId = reader.u64();
        if (!readTypes(reader, reference.parameterTypes, diagnostics) ||
            !readTypeIds(reader, reference.parameterTypeIds, diagnostics)) {
            return false;
        }
        module.functionReferences.push_back(std::move(reference));
    }
    return reader.valid();
}

void writeObjectModelSectionBody(
    Writer& objectModelSection,
    const StringPool& strings,
    const Module& module) {
    objectModelSection.u32(static_cast<std::uint32_t>(module.types.size()));
    for (const auto& type : module.types) {
        objectModelSection.u64(type.id);
        objectModelSection.u8(static_cast<std::uint8_t>(type.accessibility));
        std::uint8_t flags = 0;
        if (type.synthetic) flags |= 0x01u;
        if (type.interfaceType) flags |= 0x02u;
        if (type.abstractType) flags |= 0x04u;
        if (type.sealedType) flags |= 0x08u;
        if (type.delegateType) flags |= 0x10u;
        if (type.staticType) flags |= 0x20u;
        if (!type.baseInterfaceNames.empty()) flags |= 0x40u;
        objectModelSection.u8(flags);
        objectModelSection.u64(type.baseTypeId);
        objectModelSection.u32(type.baseTypeName.empty()
            ? InvalidIndex
            : strings.indexOf(type.baseTypeName));
        if (!type.baseInterfaceNames.empty()) {
            objectModelSection.u32(static_cast<std::uint32_t>(
                type.baseInterfaceNames.size()));
            for (const auto& bi : type.baseInterfaceNames) {
                objectModelSection.u32(strings.indexOf(bi));
            }
        }
        objectModelSection.u32(static_cast<std::uint32_t>(type.fields.size()));
        for (const auto& field : type.fields) {
            objectModelSection.u8(static_cast<std::uint8_t>(field.accessibility));
            objectModelSection.u64(field.declaringTypeId);
            objectModelSection.u32(strings.indexOf(field.declaringTypeName));
            objectModelSection.u32(strings.indexOf(field.sourceName));
            objectModelSection.u64(field.id);
            if (!field.hasConstantValue) {
                objectModelSection.u8(0u);
            } else if (std::holds_alternative<std::int64_t>(field.constantValue)) {
                objectModelSection.u8(1u);
                objectModelSection.i64(std::get<std::int64_t>(field.constantValue));
            } else if (std::holds_alternative<double>(field.constantValue)) {
                objectModelSection.u8(2u);
                objectModelSection.f64(std::get<double>(field.constantValue));
            } else if (std::holds_alternative<bool>(field.constantValue)) {
                objectModelSection.u8(3u);
                objectModelSection.u8(std::get<bool>(field.constantValue) ? 1u : 0u);
            } else if (std::holds_alternative<std::string>(field.constantValue)) {
                objectModelSection.u8(4u);
                objectModelSection.u32(strings.indexOf(std::get<std::string>(field.constantValue)));
            } else {
                objectModelSection.u8(0u);
            }
        }
        objectModelSection.u32(static_cast<std::uint32_t>(type.methods.size()));
        for (const auto& method : type.methods) {
            writeFunctionSymbol(objectModelSection, strings, method);
        }
        objectModelSection.u32(static_cast<std::uint32_t>(
            type.constructors.size() + (type.staticConstructor ? 1u : 0u)));
        for (const auto& constructor : type.constructors) {
            writeFunctionSymbol(objectModelSection, strings, constructor);
        }
        if (type.staticConstructor) {
            writeFunctionSymbol(objectModelSection, strings, *type.staticConstructor);
        }
        objectModelSection.u32(static_cast<std::uint32_t>(type.properties.size()));
        for (const auto& property : type.properties) {
            writePropertySymbol(objectModelSection, strings, property);
        }
        objectModelSection.u32(static_cast<std::uint32_t>(type.virtualDispatchTable.size()));
        for (const auto symbolId : type.virtualDispatchTable) {
            objectModelSection.u64(symbolId);
        }
        objectModelSection.u32(static_cast<std::uint32_t>(type.interfaceDispatchMaps.size()));
        for (const auto& map : type.interfaceDispatchMaps) {
            objectModelSection.u64(map.interfaceTypeId);
            objectModelSection.u32(static_cast<std::uint32_t>(map.slots.size()));
            for (const auto symbolId : map.slots) {
                objectModelSection.u64(symbolId);
            }
        }
    }
    objectModelSection.u32(static_cast<std::uint32_t>(module.functionReferences.size()));
    for (const auto& reference : module.functionReferences) {
        std::uint8_t flags = 0;
        if (reference.virtualDispatch) flags |= 0x01u;
        if (reference.interfaceDispatch) flags |= 0x02u;
        objectModelSection.u8(flags);
        objectModelSection.u32(reference.virtualSlot);
        objectModelSection.u64(reference.interfaceTypeId);
        objectModelSection.u32(reference.interfaceSlot);
    }
}

bool readObjectModelSectionBody(
    Reader& reader,
    const std::vector<std::string>& strings,
    Module& module,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto typeCount = reader.u32();
    if (!reader.valid() || typeCount != module.types.size()) {
        diagnostics.report(
            "RS5004", "object-model type count does not match type section", {});
        return false;
    }
    for (std::uint32_t index = 0; index < typeCount; ++index) {
        auto& type = module.types[index];
        if (reader.u64() != type.id) {
            diagnostics.report(
                "RS5004", "object-model type identity does not match type section", {});
            return false;
        }
        const auto accessibility = reader.u8();
        const auto flags = reader.u8();
        if (!reader.valid() ||
            accessibility > static_cast<std::uint8_t>(semantic::Accessibility::Private) ||
            (flags & 0x80u) != 0) {
            diagnostics.report("RS5004", "invalid object-model type flags", {});
            return false;
        }
        type.accessibility = static_cast<semantic::Accessibility>(accessibility);
        type.synthetic = (flags & 0x01u) != 0;
        type.interfaceType = (flags & 0x02u) != 0;
        type.abstractType = (flags & 0x04u) != 0;
        type.sealedType = (flags & 0x08u) != 0;
        type.delegateType = (flags & 0x10u) != 0;
        type.staticType = (flags & 0x20u) != 0;
        type.baseTypeId = reader.u64();
        const auto baseTypeNameIndex = reader.u32();
        if (baseTypeNameIndex != InvalidIndex) {
            if (!validStringIndex(
                    baseTypeNameIndex, strings, diagnostics,
                    "invalid base type-name string index")) {
                return false;
            }
            type.baseTypeName = strings[baseTypeNameIndex];
        }
        if ((flags & 0x40u) != 0) {
            const auto baseIfaceCount = reader.u32();
            if (!reader.valid() || baseIfaceCount > reader.remaining() / 4) {
                diagnostics.report("RS5003", "truncated base interface list", {});
                return false;
            }
            type.baseInterfaceNames.reserve(baseIfaceCount);
            for (std::uint32_t ifaceIndex = 0; ifaceIndex < baseIfaceCount; ++ifaceIndex) {
                const auto ifaceNameIndex = reader.u32();
                if (!validStringIndex(
                        ifaceNameIndex, strings, diagnostics,
                        "invalid base interface string index")) {
                    return false;
                }
                type.baseInterfaceNames.push_back(strings[ifaceNameIndex]);
            }
        }
        const auto fieldMetadataCount = reader.u32();
        if (!reader.valid() || fieldMetadataCount != type.fields.size()) {
            diagnostics.report(
                "RS5004", "object-model field count does not match type section", {});
            return false;
        }
        for (std::uint32_t fieldIndex = 0; fieldIndex < fieldMetadataCount; ++fieldIndex) {
            auto& field = type.fields[fieldIndex];
            const auto fieldAccessibility = reader.u8();
            field.declaringTypeId = reader.u64();
            const auto declaringNameIndex = reader.u32();
            const auto sourceNameIndex = reader.u32();
            field.id = reader.u64();
            if (!reader.valid() ||
                fieldAccessibility > static_cast<std::uint8_t>(semantic::Accessibility::Private) ||
                !validStringIndex(
                    declaringNameIndex, strings, diagnostics,
                    "invalid field declaring-type string index") ||
                !validStringIndex(
                    sourceNameIndex, strings, diagnostics,
                    "invalid field source string index")) {
                diagnostics.report("RS5004", "invalid field object-model descriptor", {});
                return false;
            }
            field.accessibility = static_cast<semantic::Accessibility>(fieldAccessibility);
            field.declaringTypeName = strings[declaringNameIndex];
            field.sourceName = strings[sourceNameIndex];
            const auto constKind = reader.u8();
            if (!reader.valid() || constKind > 4u) {
                diagnostics.report("RS5004", "invalid field constant value tag", {});
                return false;
            }
            if (constKind == 1u) {
                field.hasConstantValue = true;
                field.constantValue = reader.i64();
            } else if (constKind == 2u) {
                field.hasConstantValue = true;
                field.constantValue = reader.f64();
            } else if (constKind == 3u) {
                const auto b = reader.u8();
                if (!reader.valid() || b > 1u) {
                    diagnostics.report("RS5004", "invalid field boolean constant value", {});
                    return false;
                }
                field.hasConstantValue = true;
                field.constantValue = (b != 0u);
            } else if (constKind == 4u) {
                const auto strIdx = reader.u32();
                if (!validStringIndex(
                        strIdx, strings, diagnostics, "invalid field constant string index")) {
                    return false;
                }
                field.hasConstantValue = true;
                field.constantValue = strings[strIdx];
            }
            if (!reader.valid()) {
                diagnostics.report("RS5004", "truncated field constant value", {});
                return false;
            }
        }
        const auto methodCount = reader.u32();
        if (!reader.valid() || methodCount > reader.remaining() / 59) {
            diagnostics.report("RS5003", "truncated method descriptor list", {});
            return false;
        }
        type.methods.reserve(methodCount);
        for (std::uint32_t methodIndex = 0; methodIndex < methodCount; ++methodIndex) {
            semantic::FunctionSymbol method;
            if (!readFunctionSymbol(reader, strings, method, diagnostics)) return false;
            type.methods.push_back(std::move(method));
        }
        const auto constructorCount = reader.u32();
        if (!reader.valid() || constructorCount > reader.remaining() / 59) {
            diagnostics.report("RS5003", "truncated constructor descriptor list", {});
            return false;
        }
        type.constructors.reserve(constructorCount);
        for (std::uint32_t constructorIndex = 0; constructorIndex < constructorCount; ++constructorIndex) {
            semantic::FunctionSymbol constructor;
            if (!readFunctionSymbol(reader, strings, constructor, diagnostics)) return false;
            if (constructor.staticMethod) {
                type.staticConstructor = std::move(constructor);
            } else {
                type.constructors.push_back(std::move(constructor));
            }
        }
        const auto propertyCount = reader.u32();
        if (!reader.valid() || propertyCount > reader.remaining() / 40) {
            diagnostics.report("RS5003", "truncated property descriptor list", {});
            return false;
        }
        type.properties.reserve(propertyCount);
        for (std::uint32_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex) {
            semantic::PropertySymbol property;
            if (!readPropertySymbol(reader, strings, property, diagnostics)) return false;
            type.properties.push_back(std::move(property));
        }
        const auto virtualCount = reader.u32();
        if (!reader.valid() || virtualCount > reader.remaining() / 8) {
            diagnostics.report("RS5003", "truncated virtual dispatch table", {});
            return false;
        }
        type.virtualDispatchTable.reserve(virtualCount);
        for (std::uint32_t slot = 0; slot < virtualCount; ++slot) {
            type.virtualDispatchTable.push_back(reader.u64());
        }
        const auto interfaceCount = reader.u32();
        if (!reader.valid() || interfaceCount > reader.remaining() / 12) {
            diagnostics.report("RS5003", "truncated interface dispatch map list", {});
            return false;
        }
        type.interfaceDispatchMaps.reserve(interfaceCount);
        for (std::uint32_t mapIndex = 0; mapIndex < interfaceCount; ++mapIndex) {
            semantic::InterfaceDispatchMap map;
            map.interfaceTypeId = reader.u64();
            const auto slotCount = reader.u32();
            if (!reader.valid() || slotCount > reader.remaining() / 8) {
                diagnostics.report("RS5003", "truncated interface dispatch slots", {});
                return false;
            }
            map.slots.reserve(slotCount);
            for (std::uint32_t slot = 0; slot < slotCount; ++slot) {
                map.slots.push_back(reader.u64());
            }
            type.interfaceDispatchMaps.push_back(std::move(map));
        }
    }
    const auto referenceCount = reader.u32();
    if (!reader.valid() || referenceCount != module.functionReferences.size()) {
        diagnostics.report(
            "RS5004", "object-model reference count does not match reference section", {});
        return false;
    }
    for (std::uint32_t index = 0; index < referenceCount; ++index) {
        auto& reference = module.functionReferences[index];
        const auto flags = reader.u8();
        if (!reader.valid() || (flags & 0xfcu) != 0) {
            diagnostics.report("RS5004", "invalid object-model call dispatch flags", {});
            return false;
        }
        reference.virtualDispatch = (flags & 0x01u) != 0;
        reference.interfaceDispatch = (flags & 0x02u) != 0;
        reference.virtualSlot = reader.u32();
        reference.interfaceTypeId = reader.u64();
        reference.interfaceSlot = reader.u32();
    }
    return reader.valid();
}

void writeLanguageMetadataSectionBody(
    Writer& metadataSection,
    const StringPool& strings,
    const Module& module) {
    metadataSection.u32(static_cast<std::uint32_t>(module.languageMetadata.attributes.size()));
    for (const auto& attribute : module.languageMetadata.attributes) {
        metadataSection.u32(strings.indexOf(attribute.target));
        metadataSection.u32(strings.indexOf(attribute.name));
        metadataSection.u32(strings.indexOf(attribute.sourceName));
        metadataSection.u64(static_cast<std::uint64_t>(attribute.offset));
        metadataSection.u32(static_cast<std::uint32_t>(attribute.arguments.size()));
        for (const auto& argument : attribute.arguments) {
            metadataSection.u32(strings.indexOf(argument.name));
            metadataSection.u32(strings.indexOf(argument.value));
        }
    }
    metadataSection.u32(static_cast<std::uint32_t>(module.languageMetadata.interfaces.size()));
    for (const auto& implementation : module.languageMetadata.interfaces) {
        metadataSection.u32(strings.indexOf(implementation.typeName));
        metadataSection.u32(static_cast<std::uint32_t>(implementation.interfaces.size()));
        for (const auto& name : implementation.interfaces) {
            metadataSection.u32(strings.indexOf(name));
        }
    }
    metadataSection.u32(static_cast<std::uint32_t>(module.languageMetadata.genericInstantiations.size()));
    for (const auto& instantiation : module.languageMetadata.genericInstantiations) {
        metadataSection.u32(strings.indexOf(instantiation.genericName));
        metadataSection.u32(strings.indexOf(instantiation.generatedName));
        metadataSection.u32(static_cast<std::uint32_t>(instantiation.arguments.size()));
        for (const auto& argument : instantiation.arguments) {
            metadataSection.u32(strings.indexOf(argument));
        }
    }
    metadataSection.u32(static_cast<std::uint32_t>(module.languageMetadata.sequences.size()));
    for (const auto& sequence : module.languageMetadata.sequences) {
        metadataSection.u32(strings.indexOf(sequence.typeName));
        metadataSection.u32(strings.indexOf(sequence.name));
        if (hasSequenceResultMetadata(module.version)) {
            metadataSection.u32(strings.indexOf(sequence.resultTypeName));
        }
        metadataSection.u32(strings.indexOf(sequence.sourceName));
        metadataSection.u64(static_cast<std::uint64_t>(sequence.offset));
        metadataSection.u32(static_cast<std::uint32_t>(sequence.callbacks.size()));
        for (const auto& callback : sequence.callbacks) {
            metadataSection.u32(strings.indexOf(callback));
        }
    }
    if (hasInlineDependencyMetadata(module)) {
        metadataSection.u32(kInlineMetadataTrailerMagic);
        metadataSection.u32(static_cast<std::uint32_t>(module.languageMetadata.inlinedDependencies.size()));
        for (const auto& dep : module.languageMetadata.inlinedDependencies) {
            metadataSection.u32(strings.indexOf(dep.callerFunction));
            metadataSection.u64(dep.callerSymbolId);
            metadataSection.u32(strings.indexOf(dep.calleeModule));
            metadataSection.u32(strings.indexOf(dep.calleeFunction));
            metadataSection.u64(dep.calleeSymbolId);
            metadataSection.u64(dep.calleeBodyFingerprint);
        }
        metadataSection.u32(static_cast<std::uint32_t>(module.languageMetadata.functionInlineStamps.size()));
        for (const auto& st : module.languageMetadata.functionInlineStamps) {
            metadataSection.u64(st.symbolId);
            metadataSection.u64(st.bodyFingerprint);
        }
    }
    if (!module.languageMetadata.nonPublicFunctions.empty()) {
        metadataSection.u32(kFunctionAccessMetadataTrailerMagic);
        metadataSection.u32(static_cast<std::uint32_t>(module.languageMetadata.nonPublicFunctions.size()));
        for (const auto& entry : module.languageMetadata.nonPublicFunctions) {
            metadataSection.u64(entry.symbolId);
            metadataSection.u8(entry.accessibility);
        }
    }
    if (hasTypeFieldSchemaMetadata(module)) {
        struct FieldSchemaOut {
            std::uint32_t fieldIndex = 0;
            const semantic::FieldSymbol* field = nullptr;
        };
        struct TypeSchemaOut {
            std::uint32_t typeIndex = 0;
            std::uint8_t flags = 0;
            std::vector<FieldSchemaOut> fields;
        };
        std::vector<TypeSchemaOut> schemaEntries;
        for (std::uint32_t ti = 0; ti < static_cast<std::uint32_t>(module.types.size()); ++ti) {
            const auto& t = module.types[ti];
            std::uint8_t flags = 0;
            if (t.blittable) flags |= 0x01u;
            if (t.hasBlittableAttribute) flags |= 0x02u;
            std::vector<FieldSchemaOut> fieldsOut;
            for (std::uint32_t fi = 0; fi < static_cast<std::uint32_t>(t.fields.size()); ++fi) {
                const auto& f = t.fields[fi];
                if (f.fieldTag.has_value() || !f.formerlySerializedAs.empty()) {
                    fieldsOut.push_back(FieldSchemaOut{fi, &f});
                }
            }
            if (flags != 0 || !fieldsOut.empty()) {
                schemaEntries.push_back(TypeSchemaOut{ti, flags, std::move(fieldsOut)});
            }
        }
        metadataSection.u32(kTypeFieldSchemaTrailerMagic);
        metadataSection.u32(static_cast<std::uint32_t>(schemaEntries.size()));
        for (const auto& ts : schemaEntries) {
            metadataSection.u32(ts.typeIndex);
            metadataSection.u8(ts.flags);
            metadataSection.u32(static_cast<std::uint32_t>(ts.fields.size()));
            for (const auto& fs : ts.fields) {
                metadataSection.u32(fs.fieldIndex);
                metadataSection.u8(fs.field->fieldTag.has_value() ? 1u : 0u);
                if (fs.field->fieldTag.has_value()) {
                    metadataSection.u32(static_cast<std::uint32_t>(*fs.field->fieldTag));
                }
                metadataSection.u32(static_cast<std::uint32_t>(fs.field->formerlySerializedAs.size()));
                for (const auto& alias : fs.field->formerlySerializedAs) {
                    metadataSection.u32(strings.indexOf(alias));
                }
            }
        }
    }
}

bool readLanguageMetadataSectionBody(
    Reader& reader,
    const StringLookup& strings,
    Module& module,
    std::vector<DecodedTypeSchema>& decodedTypeSchemas,
    diagnostics::DiagnosticBag& diagnostics,
    bool hasLengthPrefixForRslb = false) {
    (void)hasLengthPrefixForRslb;
    const auto readStringValue = [&](std::string& value, const char* message) -> bool {
        const auto index = reader.u32();
        if (!reader.valid() || !validStringIndex(index, strings, diagnostics, message)) {
            return false;
        }
        value = strings.copyAt(index);
        return true;
    };

    const auto attributeCount = reader.u32();
    if (!reader.valid() || attributeCount > reader.remaining() / 4) {
        diagnostics.report("RS5010", "truncated language attribute metadata", {});
        return false;
    }
    for (std::uint32_t index = 0; index < attributeCount; ++index) {
        compiler::LanguageAttributeRecord attribute;
        if (!readStringValue(attribute.target, "invalid attribute target string index") ||
            !readStringValue(attribute.name, "invalid attribute name string index") ||
            !readStringValue(attribute.sourceName, "invalid attribute source string index")) {
            return false;
        }
        attribute.offset = static_cast<std::size_t>(reader.u64());
        const auto argumentCount = reader.u32();
        if (!reader.valid() || argumentCount > reader.remaining() / 8) {
            diagnostics.report("RS5010", "truncated attribute arguments", {});
            return false;
        }
        for (std::uint32_t argument = 0; argument < argumentCount; ++argument) {
            compiler::LanguageAttributeArgument value;
            if (!readStringValue(value.name, "invalid attribute argument-name string index") ||
                !readStringValue(value.value, "invalid attribute argument-value string index")) {
                return false;
            }
            attribute.arguments.push_back(std::move(value));
        }
        module.languageMetadata.attributes.push_back(std::move(attribute));
    }

    const auto interfaceCount = reader.u32();
    if (!reader.valid() || interfaceCount > reader.remaining() / 4) {
        diagnostics.report("RS5010", "truncated interface metadata", {});
        return false;
    }
    for (std::uint32_t index = 0; index < interfaceCount; ++index) {
        compiler::LanguageInterfaceImplementation implementation;
        if (!readStringValue(implementation.typeName, "invalid interface type-name string index")) {
            return false;
        }
        const auto implementedCount = reader.u32();
        if (!reader.valid() || implementedCount > reader.remaining() / 4) {
            diagnostics.report("RS5010", "truncated implemented interfaces", {});
            return false;
        }
        for (std::uint32_t implemented = 0; implemented < implementedCount; ++implemented) {
            std::string name;
            if (!readStringValue(name, "invalid implemented-interface string index")) return false;
            implementation.interfaces.push_back(std::move(name));
        }
        module.languageMetadata.interfaces.push_back(std::move(implementation));
    }

    const auto genericCount = reader.u32();
    if (!reader.valid() || genericCount > reader.remaining() / 4) {
        diagnostics.report("RS5010", "truncated generic metadata", {});
        return false;
    }
    for (std::uint32_t index = 0; index < genericCount; ++index) {
        compiler::LanguageGenericInstantiation instantiation;
        if (!readStringValue(instantiation.genericName, "invalid generic name string index") ||
            !readStringValue(instantiation.generatedName, "invalid generic generated-name string index")) {
            return false;
        }
        const auto argumentCount = reader.u32();
        if (!reader.valid() || argumentCount > reader.remaining() / 4) {
            diagnostics.report("RS5010", "truncated generic arguments", {});
            return false;
        }
        for (std::uint32_t argument = 0; argument < argumentCount; ++argument) {
            std::string value;
            if (!readStringValue(value, "invalid generic argument string index")) return false;
            instantiation.arguments.push_back(std::move(value));
        }
        module.languageMetadata.genericInstantiations.push_back(std::move(instantiation));
    }

    const auto sequenceCount = reader.u32();
    if (!reader.valid() || sequenceCount > reader.remaining() / 4) {
        diagnostics.report("RS5010", "truncated sequence metadata", {});
        return false;
    }
    for (std::uint32_t index = 0; index < sequenceCount; ++index) {
        compiler::LanguageSequenceRecord sequence;
        if (!readStringValue(sequence.typeName, "invalid sequence type-name string index") ||
            !readStringValue(sequence.name, "invalid sequence name string index")) {
            return false;
        }
        if (hasSequenceResultMetadata(module.version) &&
            !readStringValue(sequence.resultTypeName, "invalid sequence result-type string index")) {
            return false;
        }
        if (!readStringValue(sequence.sourceName, "invalid sequence source string index")) {
            return false;
        }
        sequence.offset = static_cast<std::size_t>(reader.u64());
        const auto callbackCount = reader.u32();
        if (!reader.valid() || callbackCount > reader.remaining() / 4) {
            diagnostics.report("RS5010", "truncated sequence callbacks", {});
            return false;
        }
        for (std::uint32_t callback = 0; callback < callbackCount; ++callback) {
            std::string value;
            if (!readStringValue(value, "invalid sequence callback string index")) return false;
            sequence.callbacks.push_back(std::move(value));
        }
        module.languageMetadata.sequences.push_back(std::move(sequence));
    }
    if (reader.valid() && !reader.empty() &&
        module.version.major == 0 && module.version.minor >= 7 &&
        reader.remaining() >= 12 &&
        reader.peekU32() == kInlineMetadataTrailerMagic) {
        (void)reader.u32();
        const auto inlineDepCount = reader.u32();
        if (!reader.valid() || inlineDepCount > reader.remaining() / 36) {
            diagnostics.report("RS5010", "truncated inline dependency metadata", {});
            return false;
        }
        module.languageMetadata.inlinedDependencies.reserve(inlineDepCount);
        for (std::uint32_t index = 0; index < inlineDepCount; ++index) {
            compiler::LanguageInlinedDependency dep;
            if (!readStringValue(dep.callerFunction, "invalid inline caller string index")) return false;
            dep.callerSymbolId = reader.u64();
            if (!readStringValue(dep.calleeModule, "invalid inline callee-module string index") ||
                !readStringValue(dep.calleeFunction, "invalid inline callee string index")) {
                return false;
            }
            dep.calleeSymbolId = reader.u64();
            dep.calleeBodyFingerprint = reader.u64();
            if (!reader.valid()) {
                diagnostics.report("RS5010", "truncated inline dependency entry", {});
                return false;
            }
            module.languageMetadata.inlinedDependencies.push_back(std::move(dep));
        }
        const auto stampCount = reader.u32();
        if (!reader.valid() || stampCount > reader.remaining() / 16) {
            diagnostics.report("RS5010", "truncated inline stamp metadata", {});
            return false;
        }
        module.languageMetadata.functionInlineStamps.reserve(stampCount);
        for (std::uint32_t index = 0; index < stampCount; ++index) {
            compiler::LanguageFunctionInlineStamp st;
            st.symbolId = reader.u64();
            st.bodyFingerprint = reader.u64();
            if (!reader.valid()) {
                diagnostics.report("RS5010", "truncated inline stamp entry", {});
                return false;
            }
            module.languageMetadata.functionInlineStamps.push_back(st);
        }
    }
    if (reader.valid() && !reader.empty() &&
        reader.remaining() >= 8 &&
        reader.peekU32() == kFunctionAccessMetadataTrailerMagic) {
        (void)reader.u32();
        const auto fnAccessCount = reader.u32();
        if (!reader.valid() || fnAccessCount > reader.remaining() / 9) {
            diagnostics.report("RS5010", "truncated function accessibility metadata", {});
            return false;
        }
        module.languageMetadata.nonPublicFunctions.reserve(fnAccessCount);
        for (std::uint32_t index = 0; index < fnAccessCount; ++index) {
            compiler::LanguageFunctionAccessibility entry;
            entry.symbolId = reader.u64();
            entry.accessibility = reader.u8();
            if (!reader.valid() ||
                entry.accessibility > static_cast<std::uint8_t>(semantic::Accessibility::Private)) {
                diagnostics.report("RS5010", "invalid function accessibility entry", {});
                return false;
            }
            module.languageMetadata.nonPublicFunctions.push_back(entry);
        }
    }
    if (reader.valid() && !reader.empty() &&
        reader.remaining() >= 8 &&
        reader.peekU32() == kTypeFieldSchemaTrailerMagic) {
        (void)reader.u32();
        const auto entryCount = reader.u32();
        if (!reader.valid() || entryCount > reader.remaining() / 9) {
            diagnostics.report("RS5010", "truncated type/field schema metadata", {});
            return false;
        }
        decodedTypeSchemas.reserve(entryCount);
        for (std::uint32_t i = 0; i < entryCount; ++i) {
            DecodedTypeSchema ts;
            ts.typeIndex = reader.u32();
            ts.flags = reader.u8();
            const auto fieldEntryCount = reader.u32();
            if (!reader.valid() || fieldEntryCount > reader.remaining() / 9) {
                diagnostics.report("RS5010", "truncated field schema metadata", {});
                return false;
            }
            ts.fields.reserve(fieldEntryCount);
            for (std::uint32_t j = 0; j < fieldEntryCount; ++j) {
                DecodedFieldSchema fs;
                fs.fieldIndex = reader.u32();
                const auto hasTag = reader.u8();
                if (!reader.valid() || hasTag > 1u) {
                    diagnostics.report("RS5010", "invalid field tag marker", {});
                    return false;
                }
                if (hasTag != 0) fs.fieldTag = static_cast<std::int32_t>(reader.u32());
                const auto aliasCount = reader.u32();
                if (!reader.valid() || aliasCount > reader.remaining() / 4) {
                    diagnostics.report("RS5010", "truncated formerly-serialized-as metadata", {});
                    return false;
                }
                fs.formerlySerializedAs.reserve(aliasCount);
                for (std::uint32_t a = 0; a < aliasCount; ++a) {
                    std::string alias;
                    if (!readStringValue(alias, "invalid formerly-serialized-as string index")) return false;
                    fs.formerlySerializedAs.push_back(std::move(alias));
                }
                ts.fields.push_back(std::move(fs));
            }
            decodedTypeSchemas.push_back(std::move(ts));
        }
    }
    return reader.valid();
}

void writeFunctionCodeBody(
    Writer& codeSection,
    const StringPool& strings,
    const Function& function,
    bool includeExceptions) {
    codeSection.u32(static_cast<std::uint32_t>(function.blocks.size()));
    for (const auto& block : function.blocks) {
        codeSection.u32(block.id);
        codeSection.u32(static_cast<std::uint32_t>(block.parameters.size()));
        for (const auto& parameter : block.parameters) {
            codeSection.u32(parameter.target);
            codeSection.u8(encodeType(parameter.type));
            codeSection.u64(parameter.typeId);
        }
        codeSection.u32(static_cast<std::uint32_t>(block.instructions.size()));
        for (const auto& instruction : block.instructions) {
            codeSection.u8(static_cast<std::uint8_t>(instruction.opcode));
            codeSection.u32(instruction.result);
            writeRegisters(codeSection, instruction.operands);
            codeSection.u32(instruction.index);
            codeSection.u32(instruction.typeIndex);
            codeSection.u8(encodeOptionalType(instruction.elementType));
            codeSection.u64(instruction.activeElementTypeId());
            codeSection.i64(instruction.activeIntegerImmediate());
            codeSection.f64(instruction.activeDoubleImmediate());
            codeSection.u8(static_cast<std::uint8_t>(
                (instruction.boolImmediate ? 1u : 0u) |
                (instruction.checkedArithmetic ? 0u : 2u)));
            codeSection.u32(
                instruction.opcode == Opcode::ConstantString
                    ? strings.indexOf(instruction.stringImmediate)
                    : InvalidIndex);
        }
        codeSection.u8(static_cast<std::uint8_t>(block.terminator.kind));
        codeSection.u32(block.terminator.condition);
        codeSection.u32(block.terminator.value);
        codeSection.u32(block.terminator.target);
        codeSection.u32(block.terminator.falseTarget);
        writeRegisters(codeSection, block.terminator.arguments);
        writeRegisters(codeSection, block.terminator.falseArguments);
    }
    if (includeExceptions) {
        codeSection.u32(static_cast<std::uint32_t>(function.exceptionHandlers.size()));
        for (const auto& handler : function.exceptionHandlers) {
            codeSection.u32(static_cast<std::uint32_t>(handler.protectedBlocks.size()));
            for (const auto block : handler.protectedBlocks) codeSection.u32(block);
            codeSection.u32(handler.handlerBlock);
            codeSection.u64(handler.catchTypeId);
            codeSection.u32(static_cast<std::uint32_t>(handler.exceptionLocal));
        }
    }
}

bool readFunctionCodeBody(
    Reader& reader,
    const StringLookup& strings,
    Function& function,
    bool includeExceptions,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto blockCount = reader.u32();
    if (!reader.valid() || blockCount > reader.remaining() / 37 + 1) {
        diagnostics.report("RS5003", "invalid bytecode block count", {});
        return false;
    }
    function.blocks.reserve(blockCount);
    for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        BasicBlock block;
        block.id = reader.u32();
        const auto parameterCount = reader.u32();
        if (!reader.valid() || parameterCount > reader.remaining() / 13) {
            diagnostics.report("RS5003", "invalid block parameter count", {});
            return false;
        }
        block.parameters.reserve(parameterCount);
        for (std::uint32_t parameter = 0; parameter < parameterCount; ++parameter) {
            BlockParameter value;
            value.target = reader.u32();
            if (!decodeType(reader.u8(), value.type)) {
                diagnostics.report("RS5004", "invalid block parameter type", {});
                return false;
            }
            value.typeId = reader.u64();
            block.parameters.push_back(value);
        }
        const auto instructionCount = reader.u32();
        if (!reader.valid() || instructionCount > reader.remaining() / 39 + 1) {
            diagnostics.report("RS5003", "invalid bytecode instruction count", {});
            return false;
        }
        block.instructions.reserve(instructionCount);
        for (std::uint32_t instructionIndex = 0; instructionIndex < instructionCount; ++instructionIndex) {
            Instruction instruction;
            const auto opcode = reader.u8();
            if (opcode > static_cast<std::uint8_t>(Opcode::AsyncResolve)) {
                diagnostics.report("RS5005", "invalid bytecode opcode", {});
                return false;
            }
            instruction.opcode = static_cast<Opcode>(opcode);
            instruction.result = reader.u32();
            if (!readRegisters(reader, instruction.operands)) {
                diagnostics.report("RS5003", "truncated instruction operands", {});
                return false;
            }
            instruction.index = reader.u32();
            const auto rawTypeIdx = reader.u32();
            if (rawTypeIdx > 0xFFFFu) {
                diagnostics.report("RS5004", "instruction type index exceeds 16-bit limit", {});
                return false;
            }
            instruction.typeIndex = static_cast<std::uint16_t>(rawTypeIdx);
            if (!decodeOptionalType(reader.u8(), instruction.elementType)) {
                diagnostics.report("RS5004", "invalid instruction element type", {});
                return false;
            }
            const auto elemTypeId = reader.u64();
            const auto intImm = reader.i64();
            const auto dblImm = reader.f64();
            instruction.elementTypeId = 0;
            if (instruction.usesDoubleImmediate()) {
                instruction.doubleImmediate = dblImm;
            } else if (instruction.usesIntegerImmediate()) {
                instruction.integerImmediate = intImm;
            } else if (instruction.usesElementTypeId()) {
                instruction.elementTypeId = elemTypeId;
            }
            const auto arithmeticFlags = reader.u8();
            if (arithmeticFlags > 3u) {
                diagnostics.report("RS6016", "invalid instruction arithmetic flags", {});
                return false;
            }
            instruction.boolImmediate = (arithmeticFlags & 1u) != 0;
            instruction.checkedArithmetic = (arithmeticFlags & 2u) == 0;
            const auto stringIndex = reader.u32();
            if (instruction.opcode == Opcode::ConstantString) {
                if (!validStringIndex(stringIndex, strings, diagnostics,
                        "invalid constant string index")) {
                    return false;
                }
                instruction.stringImmediate = strings.copyAt(stringIndex);
            } else if (stringIndex != InvalidIndex) {
                diagnostics.report("RS5007", "unexpected instruction string index", {});
                return false;
            }
            block.instructions.push_back(std::move(instruction));
        }
        const auto terminator = reader.u8();
        if (terminator > static_cast<std::uint8_t>(TerminatorKind::Throw)) {
            diagnostics.report("RS5006", "invalid bytecode terminator", {});
            return false;
        }
        block.terminator.kind = static_cast<TerminatorKind>(terminator);
        block.terminator.condition = reader.u32();
        block.terminator.value = reader.u32();
        const auto rawTarget = reader.u32();
        const auto rawFalseTarget = reader.u32();
        if (rawTarget > 0xFFFFu || rawFalseTarget > 0xFFFFu) {
            diagnostics.report("RS5004", "terminator target block index exceeds 16-bit limit", {});
            return false;
        }
        block.terminator.target = static_cast<std::uint16_t>(rawTarget);
        block.terminator.falseTarget = static_cast<std::uint16_t>(rawFalseTarget);
        if (!readRegisters(reader, block.terminator.arguments) ||
            !readRegisters(reader, block.terminator.falseArguments)) {
            diagnostics.report("RS5003", "truncated bytecode terminator", {});
            return false;
        }
        function.blocks.push_back(std::move(block));
    }
    if (includeExceptions) {
        const auto handlerCount = reader.u32();
        if (!reader.valid() || handlerCount > reader.remaining() / 20 + 1) {
            diagnostics.report("RS5003", "invalid exception handler count", {});
            return false;
        }
        function.exceptionHandlers.reserve(handlerCount);
        for (std::uint32_t index = 0; index < handlerCount; ++index) {
            ExceptionHandler handler;
            const auto protectedCount = reader.u32();
            if (!reader.valid() || protectedCount > reader.remaining() / 4) {
                diagnostics.report("RS5003", "invalid protected block count", {});
                return false;
            }
            handler.protectedBlocks.resize(protectedCount);
            if (!reader.readBulkU32(handler.protectedBlocks.data(), protectedCount)) {
                diagnostics.report("RS5003", "truncated protected block list", {});
                return false;
            }
            handler.handlerBlock = reader.u32();
            handler.catchTypeId = reader.u64();
            handler.exceptionLocal = reader.u32();
            function.exceptionHandlers.push_back(std::move(handler));
        }
    }
    return reader.valid();
}

void writeSourceFilesTable(
    Writer& out,
    const StringPool& strings,
    const std::vector<debug::SourceFileInfo>& sourceFiles) {
    out.u32(static_cast<std::uint32_t>(sourceFiles.size()));
    for (const auto& source : sourceFiles) {
        out.u32(source.id);
        out.u32(strings.indexOf(source.path));
        out.u64(source.contentHash);
        out.u32(static_cast<std::uint32_t>(source.lineStarts.size()));
        for (const auto start : source.lineStarts) out.u32(start);
    }
}

bool readSourceFilesTable(
    Reader& reader,
    const StringLookup& strings,
    std::vector<debug::SourceFileInfo>& sourceFiles,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto sourceCount = reader.u32();
    if (!reader.valid() || sourceCount > reader.remaining() / 20 + 1) {
        diagnostics.report("RS5003", "invalid debug source-file count", {});
        return false;
    }
    sourceFiles.reserve(sourceCount);
    for (std::uint32_t index = 0; index < sourceCount; ++index) {
        debug::SourceFileInfo source;
        source.id = reader.u32();
        const auto pathIndex = reader.u32();
        if (!validStringIndex(pathIndex, strings, diagnostics,
                "invalid debug source-file path index")) {
            return false;
        }
        source.path = strings.copyAt(pathIndex);
        source.contentHash = reader.u64();
        const auto lineCount = reader.u32();
        if (!reader.valid() || lineCount > reader.remaining() / 4) {
            diagnostics.report("RS5003", "invalid debug line-start count", {});
            return false;
        }
        source.lineStarts.resize(lineCount);
        if (!reader.readBulkU32(source.lineStarts.data(), lineCount)) {
            diagnostics.report("RS5003", "truncated debug line-start table", {});
            return false;
        }
        sourceFiles.push_back(std::move(source));
    }
    return reader.valid();
}

void writeFunctionDebugCore(
    Writer& out,
    const StringPool& strings,
    const Function& function,
    bool inlineLineMap) {
    const auto& info = function.debugInfo;
    out.u32(info.sourceFileId);
    writeRange(out, info.declaration);
    writeRange(out, info.body);
    out.u32(static_cast<std::uint32_t>(info.sequencePoints.size()));
    for (const auto& point : info.sequencePoints) {
        out.u32(point.blockId);
        out.u32(point.instructionIndex);
        out.u8(point.terminator ? 1 : 0);
        writeRange(out, point.range);
        if (inlineLineMap) {
            out.u8(point.hidden ? 1u : 0u);
            out.u32(point.mappedPath.empty() ? InvalidIndex : strings.indexOf(point.mappedPath));
        }
    }
    out.u32(static_cast<std::uint32_t>(info.locals.size()));
    for (const auto& local : info.locals) {
        out.u32(strings.indexOf(local.name));
        out.u32(local.slot);
        out.u8(encodeType(local.type));
        out.u64(local.typeId);
        out.u8(local.parameter ? 1 : 0);
        writeRange(out, local.declaration);
        writeRange(out, local.scope);
    }
}

bool readFunctionDebugCore(
    Reader& reader,
    const StringLookup& strings,
    const std::vector<debug::SourceFileInfo>& sourceFiles,
    Function& function,
    bool inlineLineMap,
    diagnostics::DiagnosticBag& diagnostics) {
    auto& info = function.debugInfo;
    info.sourceFileId = reader.u32();
    if (!readRange(reader, info.declaration) || !readRange(reader, info.body)) {
        diagnostics.report("RS5003", "truncated function debug range", {});
        return false;
    }
    const auto pointCount = reader.u32();
    if (!reader.valid() || pointCount > reader.remaining() / 45 + 1) {
        diagnostics.report("RS5003", "invalid sequence-point count", {});
        return false;
    }
    info.sequencePoints.reserve(pointCount);
    for (std::uint32_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
        debug::SequencePoint point;
        point.blockId = reader.u32();
        point.instructionIndex = reader.u32();
        point.terminator = reader.u8() != 0;
        if (!readRange(reader, point.range)) {
            diagnostics.report("RS5003", "truncated sequence point", {});
            return false;
        }
        if (inlineLineMap) {
            point.hidden = reader.u8() != 0;
            const auto pathIdx = reader.u32();
            if (pathIdx != InvalidIndex) {
                if (!validStringIndex(pathIdx, strings, diagnostics,
                        "invalid sequence-point mapped path index")) {
                    return false;
                }
                point.mappedPath = strings.copyAt(pathIdx);
            }
        }
        info.sequencePoints.push_back(std::move(point));
    }
    const auto localCount = reader.u32();
    if (!reader.valid() || localCount > reader.remaining() / 90 + 1) {
        diagnostics.report("RS5003", "invalid local-debug count", {});
        return false;
    }
    info.locals.reserve(localCount);
    for (std::uint32_t localIndex = 0; localIndex < localCount; ++localIndex) {
        debug::LocalVariableInfo local;
        const auto nameIndex = reader.u32();
        if (!validStringIndex(nameIndex, strings, diagnostics,
                "invalid local-debug name index")) {
            return false;
        }
        local.name = strings.copyAt(nameIndex);
        local.slot = reader.u32();
        if (!decodeType(reader.u8(), local.type)) {
            diagnostics.report("RS5004", "invalid local-debug type", {});
            return false;
        }
        local.typeId = reader.u64();
        local.parameter = reader.u8() != 0;
        if (!readRange(reader, local.declaration) || !readRange(reader, local.scope)) {
            diagnostics.report("RS5003", "truncated local-debug range", {});
            return false;
        }
        info.locals.push_back(std::move(local));
    }
    for (const auto& source : sourceFiles) {
        if (source.id == info.sourceFileId) {
            info.sourceName = source.path;
            break;
        }
    }
    return reader.valid();
}

} // namespace

std::vector<std::uint8_t> encodeModule(const Module& module) {
    StringPool strings;
    collectModuleStrings(strings, module);

    Writer stringSection;
    writeStringTable(stringSection, strings);

    Writer typeSection;
    writeTypeSectionBody(typeSection, strings, module);

    Writer referenceSection;
    writeReferenceSectionBody(referenceSection, strings, module);

    Writer objectModelSection;
    writeObjectModelSectionBody(objectModelSection, strings, module);

    Writer codeSection;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> codeRanges;
    codeRanges.reserve(module.functions.size());
    const bool includeExceptions = hasExceptionMetadata(module.version);
    for (const auto& function : module.functions) {
        const auto offset = static_cast<std::uint32_t>(codeSection.size());
        writeFunctionCodeBody(codeSection, strings, function, includeExceptions);
        codeRanges.push_back({
            offset,
            static_cast<std::uint32_t>(codeSection.size()) - offset,
        });
    }

    Writer functionSection;
    functionSection.u32(static_cast<std::uint32_t>(module.functions.size()));
    for (std::size_t index = 0; index < module.functions.size(); ++index) {
        const auto& function = module.functions[index];
        functionSection.u64(function.symbolId);
        functionSection.u32(strings.indexOf(function.name));
        functionSection.u8(encodeType(function.returnType));
        functionSection.u64(function.returnTypeId);
        writeTypes(functionSection, function.parameterTypes);
        writeTypeIds(functionSection, function.parameterTypeIds);
        writeTypes(functionSection, function.localTypes);
        writeTypeIds(functionSection, function.localTypeIds);
        writeTypes(functionSection, function.registerTypes);
        writeTypeIds(functionSection, function.registerTypeIds);
        functionSection.u32(codeRanges[index].first);
        functionSection.u32(codeRanges[index].second);
    }

    Writer debugSection;
    writeSourceFilesTable(debugSection, strings, module.sourceFiles);
    debugSection.u32(static_cast<std::uint32_t>(module.functions.size()));
    for (const auto& function : module.functions) {
        debugSection.u64(function.symbolId);
        writeFunctionDebugCore(debugSection, strings, function, false);
    }
    const bool hasLineMapMetadata = std::any_of(
        module.functions.begin(),
        module.functions.end(),
        [](const Function& fn) {
            return std::any_of(
                fn.debugInfo.sequencePoints.begin(),
                fn.debugInfo.sequencePoints.end(),
                [](const debug::SequencePoint& sp) {
                    return sp.hidden || !sp.mappedPath.empty();
                });
        });
    if (hasLineMapMetadata) {
        debugSection.u32(kLineMapDebugTrailerMagic);
        for (const auto& function : module.functions) {
            debugSection.u64(function.symbolId);
            const auto& pts = function.debugInfo.sequencePoints;
            debugSection.u32(static_cast<std::uint32_t>(pts.size()));
            for (const auto& point : pts) {
                debugSection.u8(point.hidden ? 1u : 0u);
                debugSection.u32(
                    point.mappedPath.empty()
                        ? InvalidIndex
                        : strings.indexOf(point.mappedPath));
            }
        }
    }

    Writer metadataSection;
    writeLanguageMetadataSectionBody(metadataSection, strings, module);

    Writer writer;
    writer.bytes("RSBC", 4);
    writer.u16(module.version.major);
    writer.u16(module.version.minor);
    writer.u32(0);
    const auto sectionCount = hasObjectModelSection(module.version)
        ? SectionCount
        : LegacySectionCount;
    writer.u32(sectionCount);
    const auto directoryOffset = writer.size();
    for (std::uint32_t index = 0; index < sectionCount; ++index) {
        writer.u32(0);
        writer.u32(0);
        writer.u32(0);
    }

    std::vector<std::pair<std::uint32_t, const Writer*>> sections{
            {SectionStrings, &stringSection},
            {SectionTypes, &typeSection},
            {SectionReferences, &referenceSection},
            {SectionFunctions, &functionSection},
            {SectionCode, &codeSection},
            {SectionDebug, &debugSection},
            {SectionMetadata, &metadataSection},
        };
    if (hasObjectModelSection(module.version)) {
        sections.push_back({SectionObjectModel, &objectModelSection});
    }

    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto offset = static_cast<std::uint32_t>(writer.size());
        writer.bytes(sections[index].second->data());
        const auto size = static_cast<std::uint32_t>(sections[index].second->size());
        const auto entry = directoryOffset + index * 12;
        writer.patchU32(entry, sections[index].first);
        writer.patchU32(entry + 4, offset);
        writer.patchU32(entry + 8, size);
    }
    return writer.take();
}

bool decodeModule(
    const std::vector<std::uint8_t>& bytes,
    Module& module,
    diagnostics::DiagnosticBag& diagnostics) {
    return decodeModule(bytes.data(), bytes.size(), module, diagnostics);
}

bool decodeModule(
    const std::uint8_t* data,
    std::size_t dataSize,
    Module& module,
    diagnostics::DiagnosticBag& diagnostics) {
    module = {};
    if (!data || dataSize < 16) {
        diagnostics.report("RS5003", "truncated bytecode header", {});
        return false;
    }

    Reader header(data, dataSize, 0, dataSize);
    if (header.string(4) != "RSBC") {
        diagnostics.report("RS5000", "invalid bytecode magic", {});
        return false;
    }
    module.version.major = header.u16();
    module.version.minor = header.u16();
    (void)header.u32();
    const auto sectionCount = header.u32();
    if (!header.valid()) {
        diagnostics.report("RS5003", "truncated bytecode header", {});
        return false;
    }
    if (module.version.major != 0 ||
        (module.version.minor != 6 &&
         module.version.minor != 7 &&
         module.version.minor != 8 &&
         module.version.minor != 9)) {
        diagnostics.report("RS5001", "unsupported bytecode version", {});
        return false;
    }
    const auto expectedSectionCount = hasObjectModelSection(module.version)
        ? SectionCount
        : LegacySectionCount;
    if (sectionCount != expectedSectionCount) {
        diagnostics.report("RS5002", "invalid bytecode section count", {});
        return false;
    }

    std::unordered_map<std::uint32_t, SectionEntry> sections;
    for (std::uint32_t index = 0; index < sectionCount; ++index) {
        SectionEntry entry;
        entry.kind = header.u32();
        entry.offset = header.u32();
        entry.size = header.u32();
        if (!header.valid() || entry.offset > dataSize ||
            entry.size > dataSize - entry.offset ||
            !sections.emplace(entry.kind, entry).second) {
            diagnostics.report("RS5002", "invalid bytecode section directory", {});
            return false;
        }
    }
    std::vector<std::uint32_t> requiredSections{
             SectionStrings,
             SectionTypes,
             SectionReferences,
             SectionFunctions,
             SectionCode,
             SectionDebug,
             SectionMetadata,
         };
    if (hasObjectModelSection(module.version)) {
        requiredSections.push_back(SectionObjectModel);
    }
    for (const auto kind : requiredSections) {
        if (sections.find(kind) == sections.end()) {
            diagnostics.report("RS5002", "missing required bytecode section", {});
            return false;
        }
    }

    const auto directoryEnd = static_cast<std::uint32_t>(16 + sectionCount * 12);
    std::vector<SectionEntry> orderedSections;
    for (const auto& [kind, entry] : sections) {
        (void)kind;
        if (entry.offset < directoryEnd) {
            diagnostics.report("RS5002", "bytecode section overlaps the header", {});
            return false;
        }
        orderedSections.push_back(entry);
    }
    std::sort(orderedSections.begin(), orderedSections.end(),
        [](const SectionEntry& left, const SectionEntry& right) {
            return left.offset < right.offset;
        });
    for (std::size_t index = 1; index < orderedSections.size(); ++index) {
        const auto previousEnd =
            static_cast<std::uint64_t>(orderedSections[index - 1].offset) +
            orderedSections[index - 1].size;
        if (previousEnd > orderedSections[index].offset) {
            diagnostics.report("RS5002", "bytecode sections overlap", {});
            return false;
        }
    }

    std::vector<std::string> strings;
    {
        const auto section = sections.at(SectionStrings);
        Reader reader(data, dataSize, section.offset, section.size);
        if (!readStringTable(reader, strings, diagnostics, true)) return false;
    }
    module.name = strings.front();

    std::vector<DecodedTypeSchema> decodedTypeSchemas;
    {
        const auto section = sections.at(SectionMetadata);
        Reader reader(data, dataSize, section.offset, section.size);
        if (!readLanguageMetadataSectionBody(
                reader, strings, module, decodedTypeSchemas, diagnostics)) {
            return false;
        }
        if (!reader.valid() || !reader.empty()) {
            diagnostics.report("RS5010", "invalid language metadata section", {});
            return false;
        }
    }

    {
        const auto section = sections.at(SectionTypes);
        Reader reader(data, dataSize, section.offset, section.size);
        if (!readTypeSectionBody(reader, strings, module, decodedTypeSchemas, diagnostics)) {
            return false;
        }
        if (!reader.valid() || !reader.empty()) {
            diagnostics.report("RS5003", "invalid type descriptor section", {});
            return false;
        }
    }

    {
        const auto section = sections.at(SectionReferences);
        Reader reader(data, dataSize, section.offset, section.size);
        if (!readReferenceSectionBody(reader, strings, module, diagnostics) || !reader.empty()) {
            if (reader.valid() && !reader.empty()) {
                diagnostics.report("RS5003", "invalid function reference section", {});
            }
            return false;
        }
    }

    if (hasObjectModelSection(module.version)) {
        const auto section = sections.at(SectionObjectModel);
        Reader reader(data, dataSize, section.offset, section.size);
        if (!readObjectModelSectionBody(reader, strings, module, diagnostics) || !reader.empty()) {
            if (reader.valid() && !reader.empty()) {
                diagnostics.report("RS5003", "invalid object-model section", {});
            }
            return false;
        }
    }

    std::vector<FunctionRecord> records;
    {
        const auto section = sections.at(SectionFunctions);
        Reader reader(data, dataSize, section.offset, section.size);
        const auto count = reader.u32();
        if (!reader.valid() || count > reader.remaining() / 33) {
            diagnostics.report("RS5003", "truncated function metadata section", {});
            return false;
        }
        records.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            FunctionRecord record;
            record.function.symbolId = reader.u64();
            const auto nameIndex = reader.u32();
            if (!validStringIndex(nameIndex, strings, diagnostics,
                    "invalid function name string index")) {
                return false;
            }
            record.function.name = strings[nameIndex];
            if (!decodeType(reader.u8(), record.function.returnType)) {
                diagnostics.report("RS5004", "invalid function return type", {});
                return false;
            }
            record.function.returnTypeId = reader.u64();
            if (!readTypes(reader, record.function.parameterTypes, diagnostics) ||
                !readTypeIds(reader, record.function.parameterTypeIds, diagnostics) ||
                !readTypes(reader, record.function.localTypes, diagnostics) ||
                !readTypeIds(reader, record.function.localTypeIds, diagnostics) ||
                !readTypes(reader, record.function.registerTypes, diagnostics) ||
                !readTypeIds(reader, record.function.registerTypeIds, diagnostics)) {
                diagnostics.report("RS5004", "invalid function type metadata", {});
                return false;
            }
            record.codeOffset = reader.u32();
            record.codeSize = reader.u32();
            records.push_back(std::move(record));
        }
        if (!reader.valid() || !reader.empty()) {
            diagnostics.report("RS5003", "invalid function metadata section", {});
            return false;
        }
    }

    const auto codeSection = sections.at(SectionCode);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> codeRanges;
    for (const auto& record : records) {
        if (record.codeOffset > codeSection.size ||
            record.codeSize > codeSection.size - record.codeOffset) {
            diagnostics.report("RS5008", "function code range is outside code section", {});
            return false;
        }
        codeRanges.push_back({record.codeOffset, record.codeSize});
    }
    std::sort(codeRanges.begin(), codeRanges.end());
    for (std::size_t index = 1; index < codeRanges.size(); ++index) {
        if (static_cast<std::uint64_t>(codeRanges[index - 1].first) +
                codeRanges[index - 1].second >
            codeRanges[index].first) {
            diagnostics.report("RS5008", "function code ranges overlap", {});
            return false;
        }
    }

    module.functions.reserve(records.size());
    const bool includeExceptions = hasExceptionMetadata(module.version);
    for (auto& record : records) {
        Reader reader(
            data,
            dataSize,
            static_cast<std::size_t>(codeSection.offset) + static_cast<std::size_t>(record.codeOffset),
            record.codeSize);
        if (!readFunctionCodeBody(reader, strings, record.function, includeExceptions, diagnostics)) {
            return false;
        }
        if (!reader.valid() || !reader.empty()) {
            diagnostics.report("RS5003", "invalid bytecode function body", {});
            return false;
        }
        module.functions.push_back(std::move(record.function));
    }
    if (!module.languageMetadata.nonPublicFunctions.empty()) {
        std::unordered_map<semantic::SymbolId, semantic::Accessibility> fnAccessMap;
        fnAccessMap.reserve(module.languageMetadata.nonPublicFunctions.size());
        for (const auto& entry : module.languageMetadata.nonPublicFunctions) {
            fnAccessMap[entry.symbolId] = static_cast<semantic::Accessibility>(entry.accessibility);
        }
        for (auto& fn : module.functions) {
            if (const auto it = fnAccessMap.find(fn.symbolId); it != fnAccessMap.end()) {
                fn.accessibility = it->second;
            }
        }
    }

    {
        const auto section = sections.at(SectionDebug);
        Reader reader(data, dataSize, section.offset, section.size);
        if (!readSourceFilesTable(reader, strings, module.sourceFiles, diagnostics)) {
            return false;
        }
        std::unordered_map<semantic::SymbolId, Function*> functionsById;
        functionsById.reserve(module.functions.size());
        for (auto& function : module.functions) {
            functionsById.emplace(function.symbolId, &function);
        }
        const auto functionCount = reader.u32();
        if (!reader.valid() || functionCount != module.functions.size()) {
            diagnostics.report("RS5003", "debug function table does not match function metadata", {});
            return false;
        }
        for (std::uint32_t index = 0; index < functionCount; ++index) {
            const auto symbolId = reader.u64();
            const auto found = functionsById.find(symbolId);
            if (found == functionsById.end()) {
                diagnostics.report("RS5007", "debug info references an unknown function", {});
                return false;
            }
            if (!readFunctionDebugCore(
                    reader, strings, module.sourceFiles, *found->second, false, diagnostics)) {
                return false;
            }
        }
        if (reader.valid() && reader.remaining() >= 4 &&
            reader.peekU32() == kLineMapDebugTrailerMagic) {
            (void)reader.u32();
            for (std::uint32_t index = 0; index < functionCount; ++index) {
                const auto symbolId = reader.u64();
                const auto found = functionsById.find(symbolId);
                if (found == functionsById.end()) {
                    diagnostics.report("RS5007", "line-map debug trailer references an unknown function", {});
                    return false;
                }
                auto& pts = found->second->debugInfo.sequencePoints;
                const auto ptCount = reader.u32();
                if (!reader.valid() || ptCount != pts.size()) {
                    diagnostics.report("RS5003", "line-map debug trailer count mismatch", {});
                    return false;
                }
                for (std::uint32_t p = 0; p < ptCount; ++p) {
                    pts[p].hidden = reader.u8() != 0;
                    const auto pathIdx = reader.u32();
                    if (pathIdx != InvalidIndex) {
                        if (!validStringIndex(pathIdx, strings, diagnostics,
                                "invalid sequence-point mapped path index")) {
                            return false;
                        }
                        pts[p].mappedPath = strings[pathIdx];
                    }
                }
            }
        }
        if (!reader.valid() || !reader.empty()) {
            diagnostics.report("RS5003", "invalid debug information section", {});
            return false;
        }
    }
    return true;
}

namespace {

std::uint32_t classifyLazyFunctionTier(
    const std::string& modName,
    const std::string& fnName,
    std::string& outOwner) {
    outOwner.clear();
    const auto colonPos = fnName.find("::");
    if (colonPos != std::string::npos) {
        outOwner = fnName.substr(0u, colonPos);
    } else {
        const auto dotPos = fnName.rfind('.');
        if (dotPos != std::string::npos) {
            outOwner = fnName.substr(0u, dotPos);
        }
    }
    std::string shortOwner = outOwner;
    if (const auto dot = shortOwner.rfind('.'); dot != std::string::npos) {
        shortOwner = shortOwner.substr(dot + 1u);
    }

    if (modName == "RealScript.Math" ||
        modName == "Aero.Native" ||
        modName.rfind("RealScript.", 0u) == 0u) {
        return 0u;
    }

    static constexpr const char* kHotOwners[] = {
        "DependencyObject", "Visual", "UIElement", "FrameworkElement",
        "Panel", "Control", "ContentControl", "ItemsControl",
        "TextBlock", "Button", "Border", "StackPanel", "Grid", "Canvas",
        "ScrollViewer", "Binding", "Style", "Resource", "Application",
        "Window", "Input", "Layout", "Event", "VisualTree",
        "Observable", "Dispatcher", "Brush", "Color", "Thickness",
        "CornerRadius", "Rect", "Size", "Point", "Vector"
    };
    for (const char* kw : kHotOwners) {
        if (shortOwner == kw || shortOwner.find(kw) != std::string::npos) {
            return 0u;
        }
    }

    static constexpr const char* kColdOwners[] = {
        "DataGrid", "Chart", "Calendar", "DatePicker", "ColorPicker",
        "RichText", "TreeView", "Menu", "TabControl", "SplitView",
        "Markdown", "CodeEditor"
    };
    for (const char* kw : kColdOwners) {
        if (shortOwner.find(kw) != std::string::npos) {
            return 2u;
        }
    }
    return 1u;
}

void writeSparseTypeIds(Writer& w, const std::vector<semantic::SymbolId>& ids) {
    w.u32(static_cast<std::uint32_t>(ids.size()));
    std::uint32_t nonZero = 0u;
    for (const auto id : ids) {
        if (id != 0u) ++nonZero;
    }
    w.u32(nonZero);
    for (std::uint32_t i = 0u; i < static_cast<std::uint32_t>(ids.size()); ++i) {
        if (ids[i] != 0u) {
            w.u32(i);
            w.u64(ids[i]);
        }
    }
}

bool readSparseTypeIds(
    Reader& r,
    std::vector<semantic::SymbolId>& out,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto total = r.u32();
    const auto nonZero = r.u32();
    if (!r.valid() || total > 1000000u || nonZero > total || nonZero > r.remaining() / 12u) {
        diagnostics.report("RS5003", "invalid sparse type-id vector", {});
        return false;
    }
    out.assign(total, 0u);
    for (std::uint32_t i = 0u; i < nonZero; ++i) {
        const auto idx = r.u32();
        const auto tid = r.u64();
        if (!r.valid() || idx >= total) {
            diagnostics.report("RS5003", "out-of-range sparse type-id index", {});
            return false;
        }
        out[idx] = tid;
    }
    return r.valid();
}

void addLazyFunctionSymbolStringsV3(
    StringPool& strings,
    const semantic::FunctionSymbol& fn,
    semantic::SymbolId ownerTypeId,
    std::string_view ownerModName) {
    if (fn.declaringTypeId != ownerTypeId && !fn.declaringTypeName.empty()) {
        strings.add(fn.declaringTypeName);
    }
    if (fn.moduleName != ownerModName && !fn.moduleName.empty()) {
        strings.add(fn.moduleName);
    }
    strings.add(fn.name);
    if (!fn.returnTypeName.empty()) strings.add(fn.returnTypeName);
    if (!fn.storageReturnTypeName.empty() && fn.storageReturnTypeName != fn.returnTypeName) {
        strings.add(fn.storageReturnTypeName);
    }
    for (const auto& p : fn.parameters) {
        if (!p.typeName.empty()) strings.add(p.typeName);
        if (!p.storageTypeName.empty() && p.storageTypeName != p.typeName) {
            strings.add(p.storageTypeName);
        }
    }
}

void collectLazyStringsV3(
    StringPool& strings,
    const std::vector<Module>& modules) {
    for (const auto& module : modules) {
        strings.add(module.name);
        for (const auto& type : module.types) {
            strings.add(type.moduleName);
            strings.add(type.name);
            if (!type.baseTypeName.empty()) strings.add(type.baseTypeName);
            for (const auto& field : type.fields) {
                strings.add(field.name);
                if (!field.typeName.empty()) strings.add(field.typeName);
                if (field.declaringTypeId != type.id && !field.declaringTypeName.empty()) {
                    strings.add(field.declaringTypeName);
                }
                if (field.hasConstantValue &&
                    std::holds_alternative<std::string>(field.constantValue)) {
                    strings.add(std::get<std::string>(field.constantValue));
                }
                for (const auto& alias : field.formerlySerializedAs) {
                    strings.add(alias);
                }
            }
            const bool keepAllMethods = type.interfaceType || type.baseTypeId == 0u;
            for (const auto& method : type.methods) {
                if (keepAllMethods ||
                    method.declaringTypeId == 0u ||
                    method.declaringTypeId == type.id) {
                    addLazyFunctionSymbolStringsV3(strings, method, type.id, module.name);
                }
            }
            for (const auto& ctor : type.constructors) {
                addLazyFunctionSymbolStringsV3(strings, ctor, type.id, module.name);
            }
            if (type.staticConstructor) {
                addLazyFunctionSymbolStringsV3(strings, *type.staticConstructor, type.id, module.name);
            }
            for (const auto& prop : type.properties) {
                if (!type.interfaceType &&
                    type.baseTypeId != 0u &&
                    prop.declaringTypeId != 0u &&
                    prop.declaringTypeId != type.id) {
                    continue;
                }
                strings.add(prop.name);
                if (prop.declaringTypeId != type.id && !prop.declaringTypeName.empty()) {
                    strings.add(prop.declaringTypeName);
                }
                if (prop.getter) addLazyFunctionSymbolStringsV3(strings, *prop.getter, type.id, module.name);
                if (prop.setter) addLazyFunctionSymbolStringsV3(strings, *prop.setter, type.id, module.name);
            }
            for (const auto& member : type.enumMembers) strings.add(member.name);
        }
        for (const auto& source : module.sourceFiles) strings.add(source.path);
        for (const auto& reference : module.functionReferences) strings.add(reference.name);
        for (const auto& function : module.functions) {
            strings.add(function.name);
            for (const auto& local : function.debugInfo.locals) strings.add(local.name);
            for (const auto& point : function.debugInfo.sequencePoints) {
                if (!point.mappedPath.empty()) strings.add(point.mappedPath);
            }
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    if (instruction.opcode == Opcode::ConstantString) {
                        strings.add(instruction.stringImmediate);
                    }
                }
            }
        }
        for (const auto& attribute : module.languageMetadata.attributes) {
            strings.add(attribute.target);
            strings.add(attribute.name);
            strings.add(attribute.sourceName);
            for (const auto& argument : attribute.arguments) {
                strings.add(argument.name);
                strings.add(argument.value);
            }
        }
        for (const auto& implementation : module.languageMetadata.interfaces) {
            strings.add(implementation.typeName);
            for (const auto& name : implementation.interfaces) strings.add(name);
        }
        for (const auto& instantiation : module.languageMetadata.genericInstantiations) {
            strings.add(instantiation.genericName);
            strings.add(instantiation.generatedName);
            for (const auto& argument : instantiation.arguments) strings.add(argument);
        }
        for (const auto& sequence : module.languageMetadata.sequences) {
            strings.add(sequence.typeName);
            strings.add(sequence.name);
            if (hasSequenceResultMetadata(module.version)) {
                strings.add(sequence.resultTypeName);
            }
            strings.add(sequence.sourceName);
            for (const auto& callback : sequence.callbacks) strings.add(callback);
        }
        if (hasInlineDependencyMetadata(module)) {
            for (const auto& dep : module.languageMetadata.inlinedDependencies) {
                strings.add(dep.callerFunction);
                strings.add(dep.calleeModule);
                strings.add(dep.calleeFunction);
            }
        }
    }
}

void writeSlimFunctionSymbolV3(
    Writer& writer,
    const StringPool& strings,
    const semantic::FunctionSymbol& fn,
    semantic::SymbolId ownerTypeId,
    std::string_view ownerModName) {
    writer.u64(fn.id);
    writer.u8(static_cast<std::uint8_t>(fn.accessibility));
    writer.u64(fn.declaringTypeId);
    writer.u32((fn.declaringTypeId == ownerTypeId || fn.declaringTypeName.empty())
        ? InvalidIndex
        : strings.indexOf(fn.declaringTypeName));
    std::uint16_t flags = 0u;
    if (fn.virtualMethod) flags |= (1u << 0u);
    if (fn.overrideMethod) flags |= (1u << 1u);
    if (fn.abstractMethod) flags |= (1u << 2u);
    if (fn.sealedMethod) flags |= (1u << 3u);
    if (fn.interfaceMethod) flags |= (1u << 4u);
    if (fn.method) flags |= (1u << 5u);
    if (fn.staticMethod) flags |= (1u << 6u);
    if (fn.constructor) flags |= (1u << 7u);
    if (fn.propertyGetter) flags |= (1u << 8u);
    if (fn.propertySetter) flags |= (1u << 9u);
    if (fn.synthetic) flags |= (1u << 10u);
    writer.u16(flags);
    writer.u32(fn.virtualSlot);
    writer.u32(fn.interfaceSlot);
    writer.u32((fn.moduleName == ownerModName || fn.moduleName.empty())
        ? InvalidIndex
        : strings.indexOf(fn.moduleName));
    writer.u32(strings.indexOf(fn.name));
    writer.u8(encodeType(fn.returnType));
    writer.u32(fn.returnTypeName.empty() ? InvalidIndex : strings.indexOf(fn.returnTypeName));
    writer.u8(static_cast<std::uint8_t>(fn.returnModifier));
    writer.u8(encodeOptionalType(fn.storageReturnType));
    writer.u32(fn.storageReturnTypeName.empty()
        ? InvalidIndex
        : (fn.storageReturnTypeName == fn.returnTypeName
               ? static_cast<std::uint32_t>(InvalidIndex - 1u)
               : strings.indexOf(fn.storageReturnTypeName)));
    writer.u32(static_cast<std::uint32_t>(fn.parameters.size()));
    for (const auto& p : fn.parameters) {
        writer.u8(encodeType(p.type));
        writer.u32(p.typeName.empty() ? InvalidIndex : strings.indexOf(p.typeName));
        writer.u8(encodeOptionalType(p.storageType));
        writer.u32(p.storageTypeName.empty()
            ? InvalidIndex
            : (p.storageTypeName == p.typeName
                   ? static_cast<std::uint32_t>(InvalidIndex - 1u)
                   : strings.indexOf(p.storageTypeName)));
        writer.u8(static_cast<std::uint8_t>(p.modifier));
    }
}

bool readSlimFunctionSymbolV2(
    Reader& reader,
    const StringLookup& strings,
    semantic::FunctionSymbol& fn,
    diagnostics::DiagnosticBag& diagnostics,
    bool v3Format = false) {
    auto readString = [&](std::string& value, const char* msg, bool allowSentinel = false) -> bool {
        const auto idx = reader.u32();
        if (allowSentinel && idx == InvalidIndex) {
            value.clear();
            return true;
        }
        if (!validStringIndex(idx, strings, diagnostics, msg)) return false;
        value = strings.copyAt(idx);
        return true;
    };
    fn.id = reader.u64();
    const auto acc = reader.u8();
    fn.declaringTypeId = reader.u64();
    if (!readString(fn.declaringTypeName, "invalid slim method declaring-type string", v3Format)) return false;
    const auto flags = reader.u16();
    if (!reader.valid() ||
        acc > static_cast<std::uint8_t>(semantic::Accessibility::Private)) {
        diagnostics.report("RS5004", "invalid slim method flags", {});
        return false;
    }
    fn.accessibility = static_cast<semantic::Accessibility>(acc);
    fn.virtualMethod = (flags & (1u << 0u)) != 0u;
    fn.overrideMethod = (flags & (1u << 1u)) != 0u;
    fn.abstractMethod = (flags & (1u << 2u)) != 0u;
    fn.sealedMethod = (flags & (1u << 3u)) != 0u;
    fn.interfaceMethod = (flags & (1u << 4u)) != 0u;
    fn.method = (flags & (1u << 5u)) != 0u;
    fn.staticMethod = (flags & (1u << 6u)) != 0u;
    fn.constructor = (flags & (1u << 7u)) != 0u;
    fn.propertyGetter = (flags & (1u << 8u)) != 0u;
    fn.propertySetter = (flags & (1u << 9u)) != 0u;
    fn.synthetic = (flags & (1u << 10u)) != 0u;
    fn.virtualSlot = reader.u32();
    fn.interfaceSlot = reader.u32();
    if (!readString(fn.moduleName, "invalid slim method module string", v3Format) ||
        !readString(fn.name, "invalid slim method name string")) {
        return false;
    }
    if (!decodeType(reader.u8(), fn.returnType) ||
        !readString(fn.returnTypeName, "invalid slim method return type-name", v3Format)) {
        diagnostics.report("RS5004", "invalid slim method return", {});
        return false;
    }
    const auto retMod = reader.u8();
    if (retMod > static_cast<std::uint8_t>(semantic::ParameterModifier::In) ||
        !decodeOptionalType(reader.u8(), fn.storageReturnType)) {
        diagnostics.report("RS5004", "invalid slim method storage return", {});
        return false;
    }
    const auto srIdx = reader.u32();
    if (v3Format && srIdx == InvalidIndex) {
        fn.storageReturnTypeName.clear();
    } else if (v3Format && srIdx == static_cast<std::uint32_t>(InvalidIndex - 1u)) {
        fn.storageReturnTypeName = fn.returnTypeName;
    } else {
        if (!validStringIndex(srIdx, strings, diagnostics, "invalid slim method storage return name")) {
            return false;
        }
        fn.storageReturnTypeName = strings.copyAt(srIdx);
    }
    fn.returnModifier = static_cast<semantic::ParameterModifier>(retMod);
    const auto pCount = reader.u32();
    if (!reader.valid() || pCount > 4096u || pCount > reader.remaining() / 11u) {
        diagnostics.report("RS5003", "truncated slim method parameters", {});
        return false;
    }
    fn.parameters.reserve(pCount);
    for (std::uint32_t i = 0u; i < pCount; ++i) {
        semantic::VariableSymbol p;
        if (!decodeType(reader.u8(), p.type) ||
            !readString(p.typeName, "invalid slim param type-name", v3Format) ||
            !decodeOptionalType(reader.u8(), p.storageType)) {
            diagnostics.report("RS5004", "invalid slim parameter descriptor", {});
            return false;
        }
        const auto pstIdx = reader.u32();
        if (v3Format && pstIdx == InvalidIndex) {
            p.storageTypeName.clear();
        } else if (v3Format && pstIdx == static_cast<std::uint32_t>(InvalidIndex - 1u)) {
            p.storageTypeName = p.typeName;
        } else {
            if (!validStringIndex(pstIdx, strings, diagnostics, "invalid slim param storage type-name")) {
                return false;
            }
            p.storageTypeName = strings.copyAt(pstIdx);
        }
        const auto pMod = reader.u8();
        if (!reader.valid() || pMod > static_cast<std::uint8_t>(semantic::ParameterModifier::In)) {
            diagnostics.report("RS5004", "invalid slim parameter modifier", {});
            return false;
        }
        p.modifier = static_cast<semantic::ParameterModifier>(pMod);
        p.index = i;
        p.parameter = true;
        fn.parameters.push_back(std::move(p));
    }
    return reader.valid();
}

[[nodiscard]] std::uint32_t inheritedFieldPrefixCountForLazy(
    const semantic::TypeSymbol& type,
    const std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*>& allTypesById) noexcept {
    if (type.baseTypeId == 0u || type.interfaceType || type.kind != semantic::TypeKind::Class) {
        return 0u;
    }
    const auto it = allTypesById.find(type.baseTypeId);
    if (it == allTypesById.end() || !it->second) {
        return 0u;
    }
    const auto& base = *it->second;
    if (base.fields.size() > type.fields.size()) {
        return 0u;
    }
    for (std::size_t i = 0u; i < base.fields.size(); ++i) {
        const auto& a = base.fields[i];
        const auto& b = type.fields[i];
        if (a.id != b.id ||
            a.name != b.name ||
            a.type != b.type ||
            a.typeName != b.typeName ||
            a.index != b.index ||
            a.declaringTypeId != b.declaringTypeId ||
            a.synthetic != b.synthetic ||
            a.staticField != b.staticField ||
            a.constField != b.constField ||
            a.readonlyField != b.readonlyField ||
            a.accessibility != b.accessibility) {
            return 0u;
        }
    }
    return static_cast<std::uint32_t>(base.fields.size());
}

void writeLazyPropertySymbolV3(
    Writer& w,
    const StringPool& strings,
    const semantic::PropertySymbol& prop,
    const std::unordered_map<semantic::SymbolId, const semantic::FunctionSymbol*>& keptMethodById,
    semantic::SymbolId ownerTypeId,
    std::string_view ownerModName) {
    w.u32(strings.indexOf(prop.name));
    w.u8(static_cast<std::uint8_t>(prop.accessibility));
    w.u64(prop.declaringTypeId);
    w.u32((prop.declaringTypeId == ownerTypeId || prop.declaringTypeName.empty())
        ? InvalidIndex
        : strings.indexOf(prop.declaringTypeName));
    w.u8(encodeType(prop.type));
    w.u8(prop.staticProperty ? 1u : 0u);

    auto writeAccessor = [&](const std::optional<semantic::FunctionSymbol>& acc) {
        if (!acc.has_value()) {
            w.u8(0u);
            return;
        }
        if (acc->id != 0u && keptMethodById.find(acc->id) != keptMethodById.end()) {
            w.u8(1u);
            w.u64(acc->id);
            return;
        }
        w.u8(2u);
        writeSlimFunctionSymbolV3(w, strings, *acc, ownerTypeId, ownerModName);
    };
    writeAccessor(prop.getter);
    writeAccessor(prop.setter);

    w.u32(prop.backingFieldIndex == std::numeric_limits<std::size_t>::max()
        ? 0xffffffffu
        : static_cast<std::uint32_t>(prop.backingFieldIndex));
    w.u64(prop.id);
}

bool readLazyPropertySymbolV2(
    Reader& reader,
    const StringLookup& strings,
    const std::vector<semantic::FunctionSymbol>& keptMethods,
    semantic::PropertySymbol& prop,
    diagnostics::DiagnosticBag& diagnostics,
    bool v3Format = false) {
    auto readString = [&](std::string& value, const char* msg, bool allowSentinel = false) -> bool {
        const auto idx = reader.u32();
        if (allowSentinel && idx == InvalidIndex) {
            value.clear();
            return true;
        }
        if (!validStringIndex(idx, strings, diagnostics, msg)) return false;
        value = strings.copyAt(idx);
        return true;
    };
    if (!readString(prop.name, "invalid lazy property name")) return false;
    const auto acc = reader.u8();
    prop.declaringTypeId = reader.u64();
    if (!readString(prop.declaringTypeName, "invalid lazy property declaring-type", v3Format) ||
        !decodeType(reader.u8(), prop.type)) {
        diagnostics.report("RS5004", "invalid lazy property header", {});
        return false;
    }
    const auto staticFlag = reader.u8();
    if (!reader.valid() ||
        acc > static_cast<std::uint8_t>(semantic::Accessibility::Private) ||
        staticFlag > 1u) {
        diagnostics.report("RS5004", "invalid lazy property flags", {});
        return false;
    }
    prop.accessibility = static_cast<semantic::Accessibility>(acc);
    prop.staticProperty = staticFlag != 0u;

    auto readAccessor = [&](std::optional<semantic::FunctionSymbol>& outAcc) -> bool {
        const auto tag = reader.u8();
        if (tag == 0u) {
            outAcc.reset();
            return reader.valid();
        }
        if (tag == 1u) {
            const auto fnId = reader.u64();
            if (!reader.valid()) return false;
            for (const auto& m : keptMethods) {
                if (m.id == fnId) {
                    outAcc = m;
                    return true;
                }
            }
            diagnostics.report("RS5004", "lazy property accessor references unknown method id", {});
            return false;
        }
        if (tag == 2u) {
            semantic::FunctionSymbol fn;
            if (!readSlimFunctionSymbolV2(reader, strings, fn, diagnostics, v3Format)) return false;
            outAcc = std::move(fn);
            return true;
        }
        diagnostics.report("RS5004", "invalid lazy property accessor tag", {});
        return false;
    };
    if (!readAccessor(prop.getter) || !readAccessor(prop.setter)) return false;
    const auto bf = reader.u32();
    prop.backingFieldIndex = bf == 0xffffffffu
        ? std::numeric_limits<std::size_t>::max()
        : static_cast<std::size_t>(bf);
    prop.id = reader.u64();
    return reader.valid();
}

void writeLazyHeaderTypeAndObjectModelV3(
    Writer& out,
    const StringPool& strings,
    const Module& mod,
    const std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*>& allTypesById) {
    out.u32(static_cast<std::uint32_t>(mod.types.size()));
    for (const auto& type : mod.types) {
        out.u64(type.id);
        out.u8(static_cast<std::uint8_t>(type.kind));
        out.u8(static_cast<std::uint8_t>(type.accessibility));
        std::uint8_t flags = 0u;
        if (type.synthetic) flags |= 0x01u;
        if (type.interfaceType) flags |= 0x02u;
        if (type.abstractType) flags |= 0x04u;
        if (type.sealedType) flags |= 0x08u;
        if (type.delegateType) flags |= 0x10u;
        if (type.staticType) flags |= 0x20u;
        out.u8(flags);
        out.u32(strings.indexOf(type.moduleName));
        out.u32(strings.indexOf(type.name));
        out.u64(type.baseTypeId);
        out.u32(type.baseTypeName.empty() ? InvalidIndex : strings.indexOf(type.baseTypeName));

        const std::uint32_t inheritedFieldPrefix =
            inheritedFieldPrefixCountForLazy(type, allTypesById);
        out.u32(inheritedFieldPrefix);
        const std::uint32_t ownFieldCount =
            static_cast<std::uint32_t>(type.fields.size()) - inheritedFieldPrefix;
        out.u32(ownFieldCount);
        for (std::uint32_t fi = inheritedFieldPrefix;
             fi < static_cast<std::uint32_t>(type.fields.size()); ++fi) {
            const auto& field = type.fields[fi];
            out.u32(strings.indexOf(field.name));
            out.u8(encodeType(field.type));
            out.u32(field.typeName.empty() ? InvalidIndex : strings.indexOf(field.typeName));
            out.u32(static_cast<std::uint32_t>(field.index));
            std::uint8_t fFlags = 0u;
            if (field.synthetic) fFlags |= 0x01u;
            if (field.staticField) fFlags |= 0x02u;
            if (field.constField) fFlags |= 0x04u;
            if (field.readonlyField) fFlags |= 0x08u;
            out.u8(fFlags);
            out.u8(static_cast<std::uint8_t>(field.accessibility));
            out.u64(field.declaringTypeId);
            out.u32((field.declaringTypeId == type.id || field.declaringTypeName.empty())
                ? InvalidIndex
                : strings.indexOf(field.declaringTypeName));
            out.u64(field.id);
            if (!field.hasConstantValue) {
                out.u8(0u);
            } else if (std::holds_alternative<std::int64_t>(field.constantValue)) {
                out.u8(1u);
                out.i64(std::get<std::int64_t>(field.constantValue));
            } else if (std::holds_alternative<double>(field.constantValue)) {
                out.u8(2u);
                out.f64(std::get<double>(field.constantValue));
            } else if (std::holds_alternative<bool>(field.constantValue)) {
                out.u8(3u);
                out.u8(std::get<bool>(field.constantValue) ? 1u : 0u);
            } else if (std::holds_alternative<std::string>(field.constantValue)) {
                out.u8(4u);
                out.u32(strings.indexOf(std::get<std::string>(field.constantValue)));
            } else {
                out.u8(0u);
            }
        }

        out.u32(static_cast<std::uint32_t>(type.enumMembers.size()));
        for (const auto& member : type.enumMembers) {
            out.u32(strings.indexOf(member.name));
            out.i64(member.value);
        }

        const bool keepAllMethods = type.interfaceType || type.baseTypeId == 0u;
        std::vector<const semantic::FunctionSymbol*> keptMethods;
        std::unordered_map<semantic::SymbolId, const semantic::FunctionSymbol*> keptMethodById;
        keptMethods.reserve(type.methods.size());
        for (const auto& method : type.methods) {
            if (keepAllMethods ||
                method.declaringTypeId == 0u ||
                method.declaringTypeId == type.id) {
                keptMethods.push_back(&method);
                if (method.id != 0u) {
                    keptMethodById.try_emplace(method.id, &method);
                }
            }
        }
        out.u32(static_cast<std::uint32_t>(keptMethods.size()));
        for (const auto* m : keptMethods) {
            writeSlimFunctionSymbolV3(out, strings, *m, type.id, mod.name);
        }

        out.u32(static_cast<std::uint32_t>(
            type.constructors.size() + (type.staticConstructor ? 1u : 0u)));
        for (const auto& c : type.constructors) {
            writeSlimFunctionSymbolV3(out, strings, c, type.id, mod.name);
        }
        if (type.staticConstructor) {
            writeSlimFunctionSymbolV3(out, strings, *type.staticConstructor, type.id, mod.name);
        }

        std::vector<const semantic::PropertySymbol*> keptProps;
        keptProps.reserve(type.properties.size());
        for (const auto& p : type.properties) {
            if (keepAllMethods ||
                p.declaringTypeId == 0u ||
                p.declaringTypeId == type.id) {
                keptProps.push_back(&p);
            }
        }
        out.u32(static_cast<std::uint32_t>(keptProps.size()));
        for (const auto* p : keptProps) {
            writeLazyPropertySymbolV3(out, strings, *p, keptMethodById, type.id, mod.name);
        }

        out.u32(static_cast<std::uint32_t>(type.virtualDispatchTable.size()));
        for (const auto vid : type.virtualDispatchTable) out.u64(vid);

        out.u32(static_cast<std::uint32_t>(type.interfaceDispatchMaps.size()));
        for (const auto& imap : type.interfaceDispatchMaps) {
            out.u64(imap.interfaceTypeId);
            out.u32(static_cast<std::uint32_t>(imap.slots.size()));
            for (const auto slotId : imap.slots) out.u64(slotId);
        }
    }

    out.u32(static_cast<std::uint32_t>(mod.functionReferences.size()));
    for (const auto& reference : mod.functionReferences) {
        std::uint8_t flags = 0u;
        if (reference.virtualDispatch) flags |= 0x01u;
        if (reference.interfaceDispatch) flags |= 0x02u;
        out.u8(flags);
        out.u32(reference.virtualSlot);
        out.u64(reference.interfaceTypeId);
        out.u32(reference.interfaceSlot);
    }
}

bool readLazyHeaderTypeAndObjectModelV2(
    Reader& reader,
    const StringLookup& strings,
    Module& module,
    std::vector<std::uint32_t>& outInheritedPrefixes,
    diagnostics::DiagnosticBag& diagnostics,
    bool v3Format = false) {
    const auto count = reader.u32();
    if (!reader.valid() || count > 65536u) {
        diagnostics.report("RS5003", "invalid lazy v2 type count", {});
        return false;
    }
    module.types.reserve(count);
    outInheritedPrefixes.reserve(count);
    for (std::uint32_t ti = 0u; ti < count; ++ti) {
        semantic::TypeSymbol type;
        type.id = reader.u64();
        const auto kind = reader.u8();
        const auto acc = reader.u8();
        const auto flags = reader.u8();
        if (!reader.valid() ||
            kind > static_cast<std::uint8_t>(semantic::TypeKind::Enum) ||
            acc > static_cast<std::uint8_t>(semantic::Accessibility::Private)) {
            diagnostics.report("RS5004", "invalid lazy v2 type header", {});
            return false;
        }
        type.kind = static_cast<semantic::TypeKind>(kind);
        type.accessibility = static_cast<semantic::Accessibility>(acc);
        type.synthetic = (flags & 0x01u) != 0u;
        type.interfaceType = (flags & 0x02u) != 0u;
        type.abstractType = (flags & 0x04u) != 0u;
        type.sealedType = (flags & 0x08u) != 0u;
        type.delegateType = (flags & 0x10u) != 0u;
        type.staticType = (flags & 0x20u) != 0u;

        const auto modNameIdx = reader.u32();
        const auto nameIdx = reader.u32();
        if (!validStringIndex(modNameIdx, strings, diagnostics, "invalid lazy type module string") ||
            !validStringIndex(nameIdx, strings, diagnostics, "invalid lazy type name string")) {
            return false;
        }
        type.moduleName = strings.copyAt(modNameIdx);
        type.name = strings.copyAt(nameIdx);
        type.baseTypeId = reader.u64();
        const auto baseNameIdx = reader.u32();
        if (baseNameIdx != InvalidIndex) {
            if (!validStringIndex(baseNameIdx, strings, diagnostics, "invalid lazy baseTypeName")) {
                return false;
            }
            type.baseTypeName = strings.copyAt(baseNameIdx);
        }

        const auto inheritedPrefix = reader.u32();
        const auto ownFieldCount = reader.u32();
        if (!reader.valid() || ownFieldCount > 65536u) {
            diagnostics.report("RS5003", "invalid lazy v2 field counts", {});
            return false;
        }
        outInheritedPrefixes.push_back(inheritedPrefix);
        type.fields.reserve(ownFieldCount);
        for (std::uint32_t fi = 0u; fi < ownFieldCount; ++fi) {
            semantic::FieldSymbol f;
            const auto fNameIdx = reader.u32();
            if (!validStringIndex(fNameIdx, strings, diagnostics, "invalid lazy field name")) return false;
            f.name = strings.copyAt(fNameIdx);
            if (!decodeType(reader.u8(), f.type)) {
                diagnostics.report("RS5004", "invalid lazy field type", {});
                return false;
            }
            const auto ftNameIdx = reader.u32();
            if (ftNameIdx != InvalidIndex) {
                if (!validStringIndex(ftNameIdx, strings, diagnostics, "invalid lazy field typeName")) {
                    return false;
                }
                f.typeName = strings.copyAt(ftNameIdx);
            }
            f.index = reader.u32();
            const auto fFlags = reader.u8();
            f.synthetic = (fFlags & 0x01u) != 0u;
            f.staticField = (fFlags & 0x02u) != 0u;
            f.constField = (fFlags & 0x04u) != 0u;
            f.readonlyField = (fFlags & 0x08u) != 0u;
            const auto fAcc = reader.u8();
            f.declaringTypeId = reader.u64();
            const auto declNameIdx = reader.u32();
            f.id = reader.u64();
            if (!reader.valid() ||
                fAcc > static_cast<std::uint8_t>(semantic::Accessibility::Private)) {
                diagnostics.report("RS5004", "invalid lazy field metadata", {});
                return false;
            }
            f.accessibility = static_cast<semantic::Accessibility>(fAcc);
            if (v3Format && declNameIdx == InvalidIndex) {
                f.declaringTypeName.clear();
            } else {
                if (!validStringIndex(declNameIdx, strings, diagnostics, "invalid lazy field declaringTypeName")) {
                    diagnostics.report("RS5004", "invalid lazy field metadata", {});
                    return false;
                }
                f.declaringTypeName = strings.copyAt(declNameIdx);
            }
            const auto constKind = reader.u8();
            if (!reader.valid() || constKind > 4u) {
                diagnostics.report("RS5004", "invalid lazy field const kind", {});
                return false;
            }
            if (constKind == 1u) {
                f.hasConstantValue = true;
                f.constantValue = reader.i64();
            } else if (constKind == 2u) {
                f.hasConstantValue = true;
                f.constantValue = reader.f64();
            } else if (constKind == 3u) {
                const auto b = reader.u8();
                if (!reader.valid() || b > 1u) return false;
                f.hasConstantValue = true;
                f.constantValue = (b != 0u);
            } else if (constKind == 4u) {
                const auto strIdx = reader.u32();
                if (!validStringIndex(strIdx, strings, diagnostics, "invalid lazy field const string")) {
                    return false;
                }
                f.hasConstantValue = true;
                f.constantValue = strings.copyAt(strIdx);
            }
            type.fields.push_back(std::move(f));
        }

        const auto enumCount = reader.u32();
        if (!reader.valid() || enumCount > 65536u) {
            diagnostics.report("RS5003", "invalid lazy v2 enum count", {});
            return false;
        }
        type.enumMembers.reserve(enumCount);
        for (std::uint32_t ei = 0u; ei < enumCount; ++ei) {
            semantic::EnumMemberSymbol em;
            const auto emIdx = reader.u32();
            if (!validStringIndex(emIdx, strings, diagnostics, "invalid lazy enum member")) return false;
            em.name = strings.copyAt(emIdx);
            em.value = reader.i64();
            type.enumMembers.push_back(std::move(em));
        }

        const auto mCount = reader.u32();
        if (!reader.valid() || mCount > 65536u) {
            diagnostics.report("RS5003", "invalid lazy v2 method count", {});
            return false;
        }
        type.methods.reserve(mCount);
        for (std::uint32_t mi = 0u; mi < mCount; ++mi) {
            semantic::FunctionSymbol fn;
            if (!readSlimFunctionSymbolV2(reader, strings, fn, diagnostics, v3Format)) return false;
            type.methods.push_back(std::move(fn));
        }

        const auto cCount = reader.u32();
        if (!reader.valid() || cCount > 65536u) {
            diagnostics.report("RS5003", "invalid lazy v2 constructor count", {});
            return false;
        }
        type.constructors.reserve(cCount);
        for (std::uint32_t ci = 0u; ci < cCount; ++ci) {
            semantic::FunctionSymbol ctor;
            if (!readSlimFunctionSymbolV2(reader, strings, ctor, diagnostics, v3Format)) return false;
            if (ctor.staticMethod) type.staticConstructor = std::move(ctor);
            else type.constructors.push_back(std::move(ctor));
        }

        const auto pCount = reader.u32();
        if (!reader.valid() || pCount > 65536u) {
            diagnostics.report("RS5003", "invalid lazy v2 property count", {});
            return false;
        }
        type.properties.reserve(pCount);
        for (std::uint32_t pi = 0u; pi < pCount; ++pi) {
            semantic::PropertySymbol prop;
            if (!readLazyPropertySymbolV2(reader, strings, type.methods, prop, diagnostics, v3Format)) {
                return false;
            }
            type.properties.push_back(std::move(prop));
        }

        const auto vdtCount = reader.u32();
        if (!reader.valid() || vdtCount > reader.remaining() / 8u) {
            diagnostics.report("RS5003", "truncated lazy v2 vtable", {});
            return false;
        }
        type.virtualDispatchTable.reserve(vdtCount);
        for (std::uint32_t slot = 0u; slot < vdtCount; ++slot) {
            type.virtualDispatchTable.push_back(reader.u64());
        }

        const auto imapCount = reader.u32();
        if (!reader.valid() || imapCount > reader.remaining() / 12u) {
            diagnostics.report("RS5003", "truncated lazy v2 interface dispatch maps", {});
            return false;
        }
        type.interfaceDispatchMaps.reserve(imapCount);
        for (std::uint32_t imi = 0u; imi < imapCount; ++imi) {
            semantic::InterfaceDispatchMap imap;
            imap.interfaceTypeId = reader.u64();
            const auto sCount = reader.u32();
            if (!reader.valid() || sCount > reader.remaining() / 8u) {
                diagnostics.report("RS5003", "truncated lazy v2 interface slots", {});
                return false;
            }
            imap.slots.reserve(sCount);
            for (std::uint32_t s = 0u; s < sCount; ++s) {
                imap.slots.push_back(reader.u64());
            }
            type.interfaceDispatchMaps.push_back(std::move(imap));
        }

        module.types.push_back(std::move(type));
    }

    const auto refCount = reader.u32();
    if (!reader.valid() || refCount != module.functionReferences.size()) {
        diagnostics.report("RS5004", "lazy v2 reference dispatch metadata mismatch", {});
        return false;
    }
    for (std::uint32_t ri = 0u; ri < refCount; ++ri) {
        const auto rFlags = reader.u8();
        auto& ref = module.functionReferences[ri];
        ref.virtualDispatch = (rFlags & 0x01u) != 0u;
        ref.interfaceDispatch = (rFlags & 0x02u) != 0u;
        ref.virtualSlot = reader.u32();
        ref.interfaceTypeId = reader.u64();
        ref.interfaceSlot = reader.u32();
    }
    return reader.valid();
}

} // namespace

std::vector<std::uint8_t> encodeLazyBytecodeBundle(
    const std::vector<Module>& modules,
    std::size_t targetChunkBytes) {
    StringPool strings;
    collectLazyStringsV3(strings, modules);
    if (strings.values().empty()) {
        strings.add(std::string{});
    }

    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> allTypesById;
    for (const auto& mod : modules) {
        for (const auto& t : mod.types) {
            if (t.id != 0u) {
                allTypesById.try_emplace(t.id, &t);
            }
        }
    }

    struct FnWorkItem {
        std::uint32_t modIdx = 0u;
        std::uint32_t fnIdx = 0u;
        std::uint32_t tier = 1u;
        std::string owner;
    };
    std::vector<FnWorkItem> fnWork;
    for (std::uint32_t modIdx = 0u; modIdx < static_cast<std::uint32_t>(modules.size()); ++modIdx) {
        const auto& mod = modules[modIdx];
        fnWork.reserve(fnWork.size() + mod.functions.size());
        for (std::uint32_t fnIdx = 0u; fnIdx < static_cast<std::uint32_t>(mod.functions.size()); ++fnIdx) {
            FnWorkItem item;
            item.modIdx = modIdx;
            item.fnIdx = fnIdx;
            item.tier = classifyLazyFunctionTier(mod.name, mod.functions[fnIdx].name, item.owner);
            fnWork.push_back(std::move(item));
        }
    }
    std::stable_sort(
        fnWork.begin(),
        fnWork.end(),
        [](const FnWorkItem& a, const FnWorkItem& b) {
            if (a.tier != b.tier) return a.tier < b.tier;
            if (a.modIdx != b.modIdx) return a.modIdx < b.modIdx;
            if (a.owner != b.owner) return a.owner < b.owner;
            return a.fnIdx < b.fnIdx;
        });

    struct EncodedChunk {
        std::uint32_t uncompressedSize = 0u;
        std::vector<std::uint8_t> compressed;
        std::uint64_t checksum = 0u;
    };
    std::vector<EncodedChunk> encodedChunks;
    std::vector<LazyFunctionBodyLoc> functionLocs;
    functionLocs.reserve(fnWork.size());
    Writer currentChunk;

    auto flushChunk = [&]() {
        if (currentChunk.size() == 0u) return;
        const auto& raw = currentChunk.data();
        EncodedChunk chunk;
        chunk.uncompressedSize = static_cast<std::uint32_t>(raw.size());
        if (!lz4CompressBlock(raw.data(), raw.size(), chunk.compressed)) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            throw std::logic_error("failed to LZ4 compress lazy bytecode chunk");
#else
            std::abort();
#endif
        }
        chunk.checksum = fastChecksum64(chunk.compressed.data(), chunk.compressed.size());
        encodedChunks.push_back(std::move(chunk));
        currentChunk = Writer{};
    };

    for (const auto& item : fnWork) {
        const auto& mod = modules[item.modIdx];
        const auto& fn = mod.functions[item.fnIdx];
        const bool includeExceptions = hasExceptionMetadata(mod.version);
        Writer fnRecord;
        fnRecord.u64(fn.symbolId);
        writeTypes(fnRecord, fn.localTypes);
        writeSparseTypeIds(fnRecord, fn.localTypeIds);
        writeTypes(fnRecord, fn.registerTypes);
        writeSparseTypeIds(fnRecord, fn.registerTypeIds);
        writeFunctionCodeBody(fnRecord, strings, fn, includeExceptions);
        writeFunctionDebugCore(fnRecord, strings, fn, true);

        if (currentChunk.size() > 0u &&
            currentChunk.size() + fnRecord.size() > targetChunkBytes) {
            flushChunk();
        }
        const auto chunkIndex = static_cast<std::uint32_t>(encodedChunks.size());
        const auto offsetInChunk = static_cast<std::uint32_t>(currentChunk.size());
        const auto recSize = static_cast<std::uint32_t>(fnRecord.size());
        currentChunk.bytes(fnRecord.data());
        functionLocs.push_back(LazyFunctionBodyLoc{
            item.modIdx, item.fnIdx, chunkIndex, offsetInChunk, recSize});
        if (targetChunkBytes == 0u) {
            flushChunk();
        }
    }
    flushChunk();

    Writer headerPayload;
    writeStringTableV3(headerPayload, strings);
    headerPayload.u32(static_cast<std::uint32_t>(modules.size()));
    for (const auto& mod : modules) {
        headerPayload.u16(mod.version.major);
        headerPayload.u16(mod.version.minor);
        headerPayload.u32(strings.indexOf(mod.name));

        Writer metaWriter;
        writeLanguageMetadataSectionBody(metaWriter, strings, mod);
        headerPayload.u32(static_cast<std::uint32_t>(metaWriter.size()));
        headerPayload.bytes(metaWriter.data());

        writeReferenceSectionBody(headerPayload, strings, mod);
        writeLazyHeaderTypeAndObjectModelV3(headerPayload, strings, mod, allTypesById);
        writeSourceFilesTable(headerPayload, strings, mod.sourceFiles);

        headerPayload.u32(static_cast<std::uint32_t>(mod.functions.size()));
        for (const auto& fn : mod.functions) {
            headerPayload.u64(fn.symbolId);
            headerPayload.u32(strings.indexOf(fn.name));
            headerPayload.u8(encodeType(fn.returnType));
            headerPayload.u64(fn.returnTypeId);
            writeTypes(headerPayload, fn.parameterTypes);
            writeTypeIds(headerPayload, fn.parameterTypeIds);
        }
    }
    headerPayload.u32(static_cast<std::uint32_t>(functionLocs.size()));
    for (const auto& loc : functionLocs) {
        headerPayload.u32(loc.moduleIndex);
        headerPayload.u32(loc.functionIndex);
        headerPayload.u32(loc.chunkIndex);
        headerPayload.u32(loc.offsetInChunk);
        headerPayload.u32(loc.uncompressedSize);
    }

    std::vector<std::uint8_t> compressedHeader;
    if (!lz4CompressBlock(
            headerPayload.data().data(), headerPayload.data().size(), compressedHeader)) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        throw std::logic_error("failed to LZ4 compress lazy bytecode header");
#else
        std::abort();
#endif
    }

    const std::uint32_t chunkCount = static_cast<std::uint32_t>(encodedChunks.size());
    const std::uint32_t compressedHeaderOffset = 40u + chunkCount * 20u;
    const std::uint32_t compressedHeaderSize = static_cast<std::uint32_t>(compressedHeader.size());
    const std::uint32_t uncompressedHeaderSize = static_cast<std::uint32_t>(headerPayload.size());
    const std::uint64_t compressedHeaderChecksum =
        fastChecksum64(compressedHeader.data(), compressedHeader.size());

    Writer out;
    out.bytes("RSLB", 4);
    out.u32(3u);
    out.u16(kVersionMajor);
    out.u16(kVersionMinor);
    out.u32(kSdkCompatibilityVersion);
    out.u32(chunkCount);
    out.u32(compressedHeaderOffset);
    out.u32(compressedHeaderSize);
    out.u32(uncompressedHeaderSize);
    out.u64(compressedHeaderChecksum);

    std::uint32_t nextChunkOffset = compressedHeaderOffset + compressedHeaderSize;
    for (const auto& chunk : encodedChunks) {
        out.u32(chunk.uncompressedSize);
        out.u32(nextChunkOffset);
        out.u32(static_cast<std::uint32_t>(chunk.compressed.size()));
        out.u64(chunk.checksum);
        nextChunkOffset += static_cast<std::uint32_t>(chunk.compressed.size());
    }
    out.bytes(compressedHeader);
    for (const auto& chunk : encodedChunks) {
        out.bytes(chunk.compressed);
    }
    return out.take();
}

bool decodeLazyBytecodeBundleHeader(
    const std::uint8_t* data,
    std::size_t size,
    LazyBytecodeBundle& outBundle,
    bool copyBlob,
    diagnostics::DiagnosticBag& diagnostics,
    bool skipChecksum) {
    outBundle = {};
    if (!data || size < 40u) {
        diagnostics.report("RS5003", "truncated lazy bytecode bundle header", {});
        return false;
    }

    Reader hdr(data, size, 0u, size);
    if (hdr.string(4) != "RSLB") {
        diagnostics.report("RS5000", "invalid lazy bytecode bundle magic", {});
        return false;
    }
    const auto formatVersion = hdr.u32();
    const auto verMajor = hdr.u16();
    const auto verMinor = hdr.u16();
    const auto sdkVer = hdr.u32();
    const auto chunkCount = hdr.u32();
    const auto compHdrOffset = hdr.u32();
    const auto compHdrSize = hdr.u32();
    const auto uncompHdrSize = hdr.u32();
    const auto compHdrChecksum = hdr.u64();
    if (!hdr.valid()) {
        diagnostics.report("RS5003", "truncated lazy bytecode bundle header", {});
        return false;
    }
    if ((formatVersion != 2u && formatVersion != 3u) ||
        verMajor != kVersionMajor ||
        verMinor != kVersionMinor ||
        sdkVer != kSdkCompatibilityVersion) {
        diagnostics.report("RS5001", "incompatible lazy bytecode bundle version", {});
        return false;
    }
    const bool v3Format = (formatVersion == 3u);
    if (chunkCount > (size - 40u) / 20u) {
        diagnostics.report("RS5003", "invalid lazy bytecode chunk directory size", {});
        return false;
    }
    const std::size_t dirEnd = 40u + static_cast<std::size_t>(chunkCount) * 20u;
    if (compHdrOffset < dirEnd ||
        compHdrOffset > size ||
        compHdrSize == 0u ||
        compHdrSize > size - compHdrOffset ||
        uncompHdrSize == 0u ||
        uncompHdrSize > 512u * 1024u * 1024u) {
        diagnostics.report("RS5003", "invalid lazy bytecode header bounds", {});
        return false;
    }

    outBundle.chunks.reserve(chunkCount);
    const std::size_t chunksMinOffset = static_cast<std::size_t>(compHdrOffset) + compHdrSize;
    for (std::uint32_t i = 0; i < chunkCount; ++i) {
        LazyBytecodeChunk c;
        c.uncompressedSize = hdr.u32();
        c.compressedOffset = hdr.u32();
        c.compressedSize = hdr.u32();
        c.checksum = hdr.u64();
        if (!hdr.valid() ||
            c.uncompressedSize == 0u ||
            c.uncompressedSize > 512u * 1024u * 1024u ||
            c.compressedSize == 0u ||
            c.compressedOffset < chunksMinOffset ||
            c.compressedOffset > size ||
            c.compressedSize > size - c.compressedOffset) {
            diagnostics.report("RS5003", "invalid lazy bytecode chunk descriptor", {});
            outBundle = {};
            return false;
        }
        outBundle.chunks.push_back(c);
    }

    if (!skipChecksum &&
        fastChecksum64(data + compHdrOffset, compHdrSize) != compHdrChecksum) {
        diagnostics.report("RS5003", "lazy bytecode header checksum mismatch", {});
        outBundle = {};
        return false;
    }

    std::vector<std::uint8_t> rawHeader;
    if (!lz4DecompressBlock(data + compHdrOffset, compHdrSize, uncompHdrSize, rawHeader) ||
        rawHeader.size() != uncompHdrSize) {
        diagnostics.report("RS5003", "failed to decompress lazy bytecode header", {});
        outBundle = {};
        return false;
    }

    Reader reader(rawHeader, 0u, rawHeader.size());
    if (v3Format) {
        if (!readFlatStringPoolV3(reader, outBundle, diagnostics)) {
            outBundle = {};
            return false;
        }
    } else {
        if (!readStringTable(reader, outBundle.sharedStrings, diagnostics, false)) {
            outBundle = {};
            return false;
        }
    }
    const StringLookup strings(outBundle);
    const auto moduleCount = reader.u32();
    if (!reader.valid() || moduleCount > reader.remaining() / 24u + 1u) {
        diagnostics.report("RS5003", "truncated lazy bytecode module count", {});
        outBundle = {};
        return false;
    }
    outBundle.headerModules.reserve(moduleCount);
    std::vector<std::vector<DecodedTypeSchema>> decodedSchemasByMod(moduleCount);
    std::vector<std::vector<std::uint32_t>> inheritedPrefixesByMod(moduleCount);

    for (std::uint32_t m = 0; m < moduleCount; ++m) {
        Module mod;
        mod.version.major = reader.u16();
        mod.version.minor = reader.u16();
        const auto nameIdx = reader.u32();
        if (!reader.valid() ||
            !validStringIndex(nameIdx, strings, diagnostics,
                "invalid lazy module name string index")) {
            outBundle = {};
            return false;
        }
        mod.name = strings.copyAt(nameIdx);
        if (mod.version.major != 0 ||
            (mod.version.minor != 6 &&
             mod.version.minor != 7 &&
             mod.version.minor != 8 &&
             mod.version.minor != 9)) {
            diagnostics.report("RS5001", "unsupported lazy module bytecode version", {});
            outBundle = {};
            return false;
        }

        const auto metaLen = reader.u32();
        auto metaReader = reader.subReader(metaLen);
        if (!metaReader.valid() ||
            !readLanguageMetadataSectionBody(
                metaReader, strings, mod, decodedSchemasByMod[m], diagnostics) ||
            !metaReader.empty()) {
            diagnostics.report("RS5010", "invalid lazy module language metadata", {});
            outBundle = {};
            return false;
        }

        if (!readReferenceSectionBody(reader, strings, mod, diagnostics) ||
            !readLazyHeaderTypeAndObjectModelV2(
                reader, strings, mod, inheritedPrefixesByMod[m], diagnostics, v3Format) ||
            !readSourceFilesTable(reader, strings, mod.sourceFiles, diagnostics)) {
            outBundle = {};
            return false;
        }

        const auto fnCount = reader.u32();
        if (!reader.valid() || fnCount > reader.remaining() / 16u + 1u) {
            diagnostics.report("RS5003", "truncated lazy module function header table", {});
            outBundle = {};
            return false;
        }
        mod.functions.reserve(fnCount);
        for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
            Function fn;
            fn.symbolId = reader.u64();
            const auto fnNameIdx = reader.u32();
            if (!reader.valid() ||
                !validStringIndex(fnNameIdx, strings, diagnostics,
                    "invalid lazy function name string index")) {
                outBundle = {};
                return false;
            }
            fn.name = strings.copyAt(fnNameIdx);
            if (!decodeType(reader.u8(), fn.returnType)) {
                diagnostics.report("RS5004", "invalid lazy function return type", {});
                outBundle = {};
                return false;
            }
            fn.returnTypeId = reader.u64();
            if (!readTypes(reader, fn.parameterTypes, diagnostics) ||
                !readTypeIds(reader, fn.parameterTypeIds, diagnostics)) {
                outBundle = {};
                return false;
            }
            mod.functions.push_back(std::move(fn));
        }
        if (!mod.languageMetadata.nonPublicFunctions.empty()) {
            std::unordered_map<semantic::SymbolId, semantic::Accessibility> fnAccessMap;
            fnAccessMap.reserve(mod.languageMetadata.nonPublicFunctions.size());
            for (const auto& entry : mod.languageMetadata.nonPublicFunctions) {
                fnAccessMap[entry.symbolId] =
                    static_cast<semantic::Accessibility>(entry.accessibility);
            }
            for (auto& fn : mod.functions) {
                if (const auto it = fnAccessMap.find(fn.symbolId); it != fnAccessMap.end()) {
                    fn.accessibility = it->second;
                }
            }
        }
        outBundle.headerModules.push_back(std::move(mod));
    }

    // Reconstruct flattened fields topologically across all base modules
    {
        struct TypePos {
            std::size_t mIdx = 0u;
            std::size_t tIdx = 0u;
        };
        std::vector<TypePos> allPositions;
        std::unordered_map<semantic::SymbolId, std::size_t> posById;
        for (std::size_t mi = 0u; mi < outBundle.headerModules.size(); ++mi) {
            const auto& mod = outBundle.headerModules[mi];
            for (std::size_t ti = 0u; ti < mod.types.size(); ++ti) {
                const std::size_t flatIdx = allPositions.size();
                allPositions.push_back(TypePos{mi, ti});
                if (mod.types[ti].id != 0u) {
                    posById.try_emplace(mod.types[ti].id, flatIdx);
                }
            }
        }
        std::vector<std::uint8_t> doneState(allPositions.size(), 0u);
        auto reconstructFields = [&](auto& self, std::size_t flatIdx) -> bool {
            if (doneState[flatIdx] == 2u) return true;
            if (doneState[flatIdx] == 1u) {
                diagnostics.report("RS5004", "cyclic base type in lazy bytecode header", {});
                return false;
            }
            doneState[flatIdx] = 1u;
            const auto pos = allPositions[flatIdx];
            auto& t = outBundle.headerModules[pos.mIdx].types[pos.tIdx];
            const std::uint32_t inheritedPrefix = inheritedPrefixesByMod[pos.mIdx][pos.tIdx];
            if (inheritedPrefix > 0u) {
                const auto bit = posById.find(t.baseTypeId);
                if (bit == posById.end() || !self(self, bit->second)) {
                    diagnostics.report("RS5004", "missing base type for lazy delta fields", {});
                    return false;
                }
                const auto bPos = allPositions[bit->second];
                const auto& base = outBundle.headerModules[bPos.mIdx].types[bPos.tIdx];
                if (inheritedPrefix > base.fields.size()) {
                    diagnostics.report("RS5004", "inherited field prefix exceeds base fields", {});
                    return false;
                }
                std::vector<semantic::FieldSymbol> combined;
                combined.reserve(static_cast<std::size_t>(inheritedPrefix) + t.fields.size());
                combined.insert(
                    combined.end(),
                    base.fields.begin(),
                    base.fields.begin() + static_cast<std::ptrdiff_t>(inheritedPrefix));
                combined.insert(
                    combined.end(),
                    std::make_move_iterator(t.fields.begin()),
                    std::make_move_iterator(t.fields.end()));
                t.fields = std::move(combined);
            }
            doneState[flatIdx] = 2u;
            return true;
        };
        for (std::size_t i = 0u; i < allPositions.size(); ++i) {
            if (!reconstructFields(reconstructFields, i)) {
                outBundle = {};
                return false;
            }
        }
        for (std::size_t mi = 0u; mi < outBundle.headerModules.size(); ++mi) {
            auto& mod = outBundle.headerModules[mi];
            for (const auto& ts : decodedSchemasByMod[mi]) {
                if (ts.typeIndex >= mod.types.size()) {
                    diagnostics.report("RS5010", "out-of-range type index in schema trailer", {});
                    outBundle = {};
                    return false;
                }
                auto& targetType = mod.types[ts.typeIndex];
                targetType.blittable = (ts.flags & 0x01u) != 0;
                targetType.hasBlittableAttribute = (ts.flags & 0x02u) != 0;
                for (const auto& fs : ts.fields) {
                    if (fs.fieldIndex >= targetType.fields.size()) {
                        diagnostics.report("RS5010", "out-of-range field index in schema trailer", {});
                        outBundle = {};
                        return false;
                    }
                    auto& targetField = targetType.fields[fs.fieldIndex];
                    targetField.fieldTag = fs.fieldTag;
                    targetField.formerlySerializedAs = fs.formerlySerializedAs;
                }
            }
        }
    }

    const auto locCount = reader.u32();
    if (!reader.valid() || locCount > reader.remaining() / 20u + 1u) {
        diagnostics.report("RS5003", "truncated lazy function locator table", {});
        outBundle = {};
        return false;
    }
    outBundle.functionLocs.reserve(locCount);
    for (std::uint32_t li = 0; li < locCount; ++li) {
        LazyFunctionBodyLoc loc;
        loc.moduleIndex = reader.u32();
        loc.functionIndex = reader.u32();
        loc.chunkIndex = reader.u32();
        loc.offsetInChunk = reader.u32();
        loc.uncompressedSize = reader.u32();
        if (!reader.valid() ||
            loc.moduleIndex >= outBundle.headerModules.size() ||
            loc.functionIndex >= outBundle.headerModules[loc.moduleIndex].functions.size() ||
            loc.chunkIndex >= outBundle.chunks.size() ||
            loc.offsetInChunk > outBundle.chunks[loc.chunkIndex].uncompressedSize ||
            loc.uncompressedSize > outBundle.chunks[loc.chunkIndex].uncompressedSize - loc.offsetInChunk) {
            diagnostics.report("RS5003", "invalid lazy function locator entry", {});
            outBundle = {};
            return false;
        }
        outBundle.functionLocs.push_back(loc);
    }

    if (!reader.valid() || !reader.empty()) {
        diagnostics.report("RS5003", "lazy bytecode header has trailing bytes", {});
        outBundle = {};
        return false;
    }

    if (copyBlob) {
        outBundle.ownedBlob.assign(data, data + size);
        outBundle.rawBlobData = outBundle.ownedBlob.data();
        outBundle.rawBlobSize = outBundle.ownedBlob.size();
    } else {
        outBundle.rawBlobData = data;
        outBundle.rawBlobSize = size;
    }
    return true;
}

bool decompressLazyChunk(
    const LazyBytecodeBundle& bundle,
    std::uint32_t chunkIndex,
    std::vector<std::uint8_t>& outChunk,
    diagnostics::DiagnosticBag& diagnostics) {
    outChunk.clear();
    if (chunkIndex >= bundle.chunks.size() || !bundle.rawBlobData) {
        diagnostics.report("RS5003", "lazy chunk index out of range", {});
        return false;
    }
    const auto& chunk = bundle.chunks[chunkIndex];
    if (chunk.compressedOffset > bundle.rawBlobSize ||
        chunk.compressedSize > bundle.rawBlobSize - chunk.compressedOffset) {
        diagnostics.report("RS5003", "lazy chunk range outside bundle blob", {});
        return false;
    }
    const std::uint8_t* compPtr = bundle.rawBlobData + chunk.compressedOffset;
    if (fastChecksum64(compPtr, chunk.compressedSize) != chunk.checksum) {
        diagnostics.report("RS5003", "lazy chunk checksum mismatch", {});
        return false;
    }
    if (!lz4DecompressBlock(compPtr, chunk.compressedSize, chunk.uncompressedSize, outChunk) ||
        outChunk.size() != chunk.uncompressedSize) {
        diagnostics.report("RS5003", "failed to decompress lazy bytecode chunk", {});
        outChunk.clear();
        return false;
    }
    return true;
}

bool decodeLazyFunctionBody(
    const LazyBytecodeBundle& bundle,
    std::uint32_t locIndex,
    Function& outFunction,
    diagnostics::DiagnosticBag& diagnostics,
    const std::vector<std::uint8_t>* preDecompressedChunk,
    const Module* moduleOverride) {
    if (locIndex >= bundle.functionLocs.size()) {
        diagnostics.report("RS5003", "lazy function locator index out of range", {});
        return false;
    }
    const auto& loc = bundle.functionLocs[locIndex];
    const Module* modPtr = moduleOverride;
    if (!modPtr) {
        if (loc.moduleIndex >= bundle.headerModules.size()) {
            diagnostics.report("RS5003", "invalid lazy function module reference", {});
            return false;
        }
        modPtr = &bundle.headerModules[loc.moduleIndex];
    }
    if (loc.functionIndex >= modPtr->functions.size() ||
        loc.chunkIndex >= bundle.chunks.size()) {
        diagnostics.report("RS5003", "invalid lazy function chunk reference", {});
        return false;
    }
    const auto& mod = *modPtr;
    const auto& headerFn = mod.functions[loc.functionIndex];

    std::vector<std::uint8_t> localChunk;
    const std::vector<std::uint8_t>* chunkBuf = preDecompressedChunk;
    if (chunkBuf == nullptr) {
        if (!decompressLazyChunk(bundle, loc.chunkIndex, localChunk, diagnostics)) {
            return false;
        }
        chunkBuf = &localChunk;
    }
    if (loc.offsetInChunk > chunkBuf->size() ||
        loc.uncompressedSize > chunkBuf->size() - loc.offsetInChunk) {
        diagnostics.report("RS5003", "lazy function record outside decompressed chunk", {});
        return false;
    }

    Reader reader(*chunkBuf, loc.offsetInChunk, loc.uncompressedSize);
    const auto symId = reader.u64();
    if (!reader.valid() || symId != headerFn.symbolId) {
        diagnostics.report("RS5004", "lazy function symbol ID mismatch", {});
        return false;
    }

    if (outFunction.symbolId != headerFn.symbolId) {
        outFunction.symbolId = headerFn.symbolId;
        outFunction.accessibility = headerFn.accessibility;
        outFunction.name = headerFn.name;
        outFunction.returnType = headerFn.returnType;
        outFunction.returnTypeId = headerFn.returnTypeId;
        outFunction.parameterTypes = headerFn.parameterTypes;
        outFunction.parameterTypeIds = headerFn.parameterTypeIds;
    }
    outFunction.localTypes.clear();
    outFunction.localTypeIds.clear();
    outFunction.registerTypes.clear();
    outFunction.registerTypeIds.clear();
    outFunction.blocks.clear();
    outFunction.exceptionHandlers.clear();
    outFunction.debugInfo = {};

    if (!readTypes(reader, outFunction.localTypes, diagnostics) ||
        !readSparseTypeIds(reader, outFunction.localTypeIds, diagnostics) ||
        !readTypes(reader, outFunction.registerTypes, diagnostics) ||
        !readSparseTypeIds(reader, outFunction.registerTypeIds, diagnostics)) {
        diagnostics.report("RS5004", "invalid lazy function local/register metadata", {});
        return false;
    }
    const StringLookup strings(bundle);
    if (!readFunctionCodeBody(
            reader,
            strings,
            outFunction,
            hasExceptionMetadata(mod.version),
            diagnostics)) {
        return false;
    }
    if (!readFunctionDebugCore(
            reader, strings, mod.sourceFiles, outFunction, true, diagnostics)) {
        return false;
    }
    if (!reader.valid() || !reader.empty()) {
        diagnostics.report("RS5003", "lazy function body has trailing bytes", {});
        return false;
    }
    return true;
}

} // namespace realscript::bytecode
