#include "realscript/bytecode/Bytecode.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
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
constexpr std::uint32_t SectionCount = 6;

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
            throw std::logic_error("bytecode writer patch is outside buffer");
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
    Reader(const std::vector<std::uint8_t>& data, std::size_t begin, std::size_t size)
        : data_(data), position_(begin), end_(begin) {
        if (begin > data.size() || size > data.size() - begin) {
            valid_ = false;
            position_ = data.size();
            end_ = data.size();
        } else {
            end_ = begin + size;
        }
    }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool empty() const noexcept { return position_ == end_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return valid_ && position_ <= end_ ? end_ - position_ : 0;
    }
    std::uint8_t u8() {
        if (!require(1)) return 0;
        return data_[position_++];
    }
    std::uint16_t u16() {
        std::uint16_t result = 0;
        for (int shift = 0; shift < 16; shift += 8) {
            result |= static_cast<std::uint16_t>(u8()) << shift;
        }
        return result;
    }
    std::uint32_t u32() {
        std::uint32_t result = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            result |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return result;
    }
    std::uint64_t u64() {
        std::uint64_t result = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            result |= static_cast<std::uint64_t>(u8()) << shift;
        }
        return result;
    }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    double f64() {
        const auto bits = u64();
        double value = 0.0;
        static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    std::string string(std::size_t count) {
        if (!require(count)) return {};
        const auto* begin = reinterpret_cast<const char*>(data_.data() + position_);
        position_ += count;
        return std::string(begin, count);
    }
private:
    bool require(std::size_t count) {
        if (!valid_ || count > end_ - position_) {
            valid_ = false;
            return false;
        }
        return true;
    }
    const std::vector<std::uint8_t>& data_;
    std::size_t position_ = 0;
    std::size_t end_ = 0;
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
    case semantic::PrimitiveType::Error: break;
    }
    throw std::logic_error("invalid type in bytecode encoder");
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
            throw std::logic_error("missing string in bytecode pool");
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
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining() / 8) {
        diagnostics.report("RS5003", "truncated bytecode type ID list", {});
        return false;
    }
    typeIds.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        typeIds.push_back(reader.u64());
    }
    return reader.valid();
}

void writeRegisters(Writer& writer, const std::vector<Register>& values) {
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (const auto value : values) writer.u32(value);
}

bool readRegisters(Reader& reader, std::vector<Register>& values) {
    const auto count = reader.u32();
    if (!reader.valid() || count > reader.remaining() / 4) return false;
    values.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        values.push_back(reader.u32());
    }
    return reader.valid();
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

bool validStringIndex(
    std::uint32_t index,
    const std::vector<std::string>& strings,
    diagnostics::DiagnosticBag& diagnostics,
    const char* message) {
    if (index >= strings.size()) {
        diagnostics.report("RS5007", message, {});
        return false;
    }
    return true;
}

} // namespace

std::vector<std::uint8_t> encodeModule(const Module& module) {
    StringPool strings;
    strings.add(module.name);
    for (const auto& type : module.types) {
        strings.add(type.moduleName);
        strings.add(type.name);
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
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode == Opcode::ConstantString) {
                    strings.add(instruction.stringImmediate);
                }
            }
        }
    }

    Writer stringSection;
    stringSection.u32(static_cast<std::uint32_t>(strings.values().size()));
    for (const auto& value : strings.values()) {
        stringSection.u32(static_cast<std::uint32_t>(value.size()));
        stringSection.bytes(value.data(), value.size());
    }

    Writer typeSection;
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
            typeSection.u8(field.synthetic ? 1 : 0);
        }
        typeSection.u32(static_cast<std::uint32_t>(type.enumMembers.size()));
        for (const auto& member : type.enumMembers) {
            typeSection.u32(strings.indexOf(member.name));
            typeSection.i64(member.value);
        }
    }

    Writer referenceSection;
    referenceSection.u32(static_cast<std::uint32_t>(module.functionReferences.size()));
    for (const auto& reference : module.functionReferences) {
        referenceSection.u64(reference.symbolId);
        referenceSection.u32(strings.indexOf(reference.name));
        referenceSection.u8(encodeType(reference.returnType));
        referenceSection.u64(reference.returnTypeId);
        writeTypes(referenceSection, reference.parameterTypes);
        writeTypeIds(referenceSection, reference.parameterTypeIds);
    }

    Writer codeSection;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> codeRanges;
    for (const auto& function : module.functions) {
        const auto offset = static_cast<std::uint32_t>(codeSection.size());
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
                codeSection.u64(instruction.elementTypeId);
                codeSection.i64(instruction.integerImmediate);
                codeSection.f64(instruction.doubleImmediate);
                codeSection.u8(instruction.boolImmediate ? 1 : 0);
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
    debugSection.u32(static_cast<std::uint32_t>(module.sourceFiles.size()));
    for (const auto& source : module.sourceFiles) {
        debugSection.u32(source.id);
        debugSection.u32(strings.indexOf(source.path));
        debugSection.u64(source.contentHash);
        debugSection.u32(static_cast<std::uint32_t>(source.lineStarts.size()));
        for (const auto start : source.lineStarts) debugSection.u32(start);
    }
    debugSection.u32(static_cast<std::uint32_t>(module.functions.size()));
    for (const auto& function : module.functions) {
        debugSection.u64(function.symbolId);
        const auto& info = function.debugInfo;
        debugSection.u32(info.sourceFileId);
        writeRange(debugSection, info.declaration);
        writeRange(debugSection, info.body);
        debugSection.u32(static_cast<std::uint32_t>(info.sequencePoints.size()));
        for (const auto& point : info.sequencePoints) {
            debugSection.u32(point.blockId);
            debugSection.u32(point.instructionIndex);
            debugSection.u8(point.terminator ? 1 : 0);
            writeRange(debugSection, point.range);
        }
        debugSection.u32(static_cast<std::uint32_t>(info.locals.size()));
        for (const auto& local : info.locals) {
            debugSection.u32(strings.indexOf(local.name));
            debugSection.u32(local.slot);
            debugSection.u8(encodeType(local.type));
            debugSection.u64(local.typeId);
            debugSection.u8(local.parameter ? 1 : 0);
            writeRange(debugSection, local.declaration);
            writeRange(debugSection, local.scope);
        }
    }

    Writer writer;
    writer.bytes("RSBC", 4);
    writer.u16(module.version.major);
    writer.u16(module.version.minor);
    writer.u32(0);
    writer.u32(SectionCount);
    const auto directoryOffset = writer.size();
    for (std::uint32_t index = 0; index < SectionCount; ++index) {
        writer.u32(0);
        writer.u32(0);
        writer.u32(0);
    }

    const std::array<std::pair<std::uint32_t, const Writer*>, SectionCount>
        sections{{
            {SectionStrings, &stringSection},
            {SectionTypes, &typeSection},
            {SectionReferences, &referenceSection},
            {SectionFunctions, &functionSection},
            {SectionCode, &codeSection},
            {SectionDebug, &debugSection},
        }};

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
    module = {};
    if (bytes.size() < 16) {
        diagnostics.report("RS5003", "truncated bytecode header", {});
        return false;
    }

    Reader header(bytes, 0, bytes.size());
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
    if (module.version.major != 0 || module.version.minor != 5) {
        diagnostics.report("RS5001", "unsupported bytecode version", {});
        return false;
    }
    if (sectionCount != SectionCount) {
        diagnostics.report("RS5002", "invalid bytecode section count", {});
        return false;
    }

    std::unordered_map<std::uint32_t, SectionEntry> sections;
    for (std::uint32_t index = 0; index < sectionCount; ++index) {
        SectionEntry entry;
        entry.kind = header.u32();
        entry.offset = header.u32();
        entry.size = header.u32();
        if (!header.valid() || entry.offset > bytes.size() ||
            entry.size > bytes.size() - entry.offset ||
            !sections.emplace(entry.kind, entry).second) {
            diagnostics.report("RS5002", "invalid bytecode section directory", {});
            return false;
        }
    }
    for (const auto kind : {
             SectionStrings,
             SectionTypes,
             SectionReferences,
             SectionFunctions,
             SectionCode,
             SectionDebug,
         }) {
        if (sections.find(kind) == sections.end()) {
            diagnostics.report("RS5002", "missing required bytecode section", {});
            return false;
        }
    }

    const auto directoryEnd = static_cast<std::uint32_t>(16 + SectionCount * 12);
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
        Reader reader(bytes, section.offset, section.size);
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
        if (!reader.empty()) {
            diagnostics.report("RS5002", "string section has trailing bytes", {});
            return false;
        }
    }
    if (strings.empty()) {
        diagnostics.report("RS5007", "bytecode string table is empty", {});
        return false;
    }
    module.name = strings.front();

    {
        const auto section = sections.at(SectionTypes);
        Reader reader(bytes, section.offset, section.size);
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
                field.synthetic = reader.u8() != 0;
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
        if (!reader.valid() || !reader.empty()) {
            diagnostics.report("RS5003", "invalid type descriptor section", {});
            return false;
        }
    }

    {
        const auto section = sections.at(SectionReferences);
        Reader reader(bytes, section.offset, section.size);
        const auto count = reader.u32();
        if (!reader.valid() || count > reader.remaining() / 17) {
            diagnostics.report("RS5003", "truncated function reference section", {});
            return false;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            FunctionReference reference;
            reference.symbolId = reader.u64();
            const auto nameIndex = reader.u32();
            if (!validStringIndex(nameIndex, strings, diagnostics,
                    "invalid reference name string index")) {
                return false;
            }
            reference.name = strings[nameIndex];
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
        if (!reader.valid() || !reader.empty()) {
            diagnostics.report("RS5003", "invalid function reference section", {});
            return false;
        }
    }

    std::vector<FunctionRecord> records;
    {
        const auto section = sections.at(SectionFunctions);
        Reader reader(bytes, section.offset, section.size);
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

    for (auto& record : records) {
        Reader reader(bytes, codeSection.offset + record.codeOffset, record.codeSize);
        const auto blockCount = reader.u32();
        if (!reader.valid() || blockCount > reader.remaining() / 37 + 1) {
            diagnostics.report("RS5003", "invalid bytecode block count", {});
            return false;
        }
        for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
            BasicBlock block;
            block.id = reader.u32();
            const auto parameterCount = reader.u32();
            if (!reader.valid() || parameterCount > reader.remaining() / 13) {
                diagnostics.report("RS5003", "invalid block parameter count", {});
                return false;
            }
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
            for (std::uint32_t instructionIndex = 0;
                 instructionIndex < instructionCount;
                 ++instructionIndex) {
                Instruction instruction;
                const auto opcode = reader.u8();
                if (opcode > static_cast<std::uint8_t>(Opcode::GreaterOrEqualDouble)) {
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
                instruction.typeIndex = reader.u32();
                if (!decodeOptionalType(reader.u8(), instruction.elementType)) {
                    diagnostics.report("RS5004", "invalid instruction element type", {});
                    return false;
                }
                instruction.elementTypeId = reader.u64();
                instruction.integerImmediate = reader.i64();
                instruction.doubleImmediate = reader.f64();
                instruction.boolImmediate = reader.u8() != 0;
                const auto stringIndex = reader.u32();
                if (instruction.opcode == Opcode::ConstantString) {
                    if (!validStringIndex(stringIndex, strings, diagnostics,
                            "invalid constant string index")) {
                        return false;
                    }
                    instruction.stringImmediate = strings[stringIndex];
                } else if (stringIndex != InvalidIndex) {
                    diagnostics.report("RS5007", "unexpected instruction string index", {});
                    return false;
                }
                block.instructions.push_back(std::move(instruction));
            }

            const auto terminator = reader.u8();
            if (terminator > static_cast<std::uint8_t>(TerminatorKind::ReturnVoid)) {
                diagnostics.report("RS5006", "invalid bytecode terminator", {});
                return false;
            }
            block.terminator.kind = static_cast<TerminatorKind>(terminator);
            block.terminator.condition = reader.u32();
            block.terminator.value = reader.u32();
            block.terminator.target = reader.u32();
            block.terminator.falseTarget = reader.u32();
            if (!readRegisters(reader, block.terminator.arguments) ||
                !readRegisters(reader, block.terminator.falseArguments)) {
                diagnostics.report("RS5003", "truncated bytecode terminator", {});
                return false;
            }
            record.function.blocks.push_back(std::move(block));
        }
        if (!reader.valid() || !reader.empty()) {
            diagnostics.report("RS5003", "invalid bytecode function body", {});
            return false;
        }
        module.functions.push_back(std::move(record.function));
    }

    {
        const auto section = sections.at(SectionDebug);
        Reader reader(bytes, section.offset, section.size);
        const auto sourceCount = reader.u32();
        if (!reader.valid() || sourceCount > reader.remaining() / 20 + 1) {
            diagnostics.report("RS5003", "invalid debug source-file count", {});
            return false;
        }
        module.sourceFiles.reserve(sourceCount);
        for (std::uint32_t index = 0; index < sourceCount; ++index) {
            debug::SourceFileInfo source;
            source.id = reader.u32();
            const auto pathIndex = reader.u32();
            if (!validStringIndex(pathIndex, strings, diagnostics,
                    "invalid debug source-file path index")) {
                return false;
            }
            source.path = strings[pathIndex];
            source.contentHash = reader.u64();
            const auto lineCount = reader.u32();
            if (!reader.valid() || lineCount > reader.remaining() / 4) {
                diagnostics.report("RS5003", "invalid debug line-start count", {});
                return false;
            }
            source.lineStarts.reserve(lineCount);
            for (std::uint32_t line = 0; line < lineCount; ++line) {
                source.lineStarts.push_back(reader.u32());
            }
            module.sourceFiles.push_back(std::move(source));
        }
        std::unordered_map<semantic::SymbolId, Function*> functionsById;
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
            auto& info = found->second->debugInfo;
            info.sourceFileId = reader.u32();
            if (!readRange(reader, info.declaration) ||
                !readRange(reader, info.body)) {
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
                local.name = strings[nameIndex];
                local.slot = reader.u32();
                if (!decodeType(reader.u8(), local.type)) {
                    diagnostics.report("RS5004", "invalid local-debug type", {});
                    return false;
                }
                local.typeId = reader.u64();
                local.parameter = reader.u8() != 0;
                if (!readRange(reader, local.declaration) ||
                    !readRange(reader, local.scope)) {
                    diagnostics.report("RS5003", "truncated local-debug range", {});
                    return false;
                }
                info.locals.push_back(std::move(local));
            }
            for (const auto& source : module.sourceFiles) {
                if (source.id == info.sourceFileId) {
                    info.sourceName = source.path;
                    break;
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

} // namespace realscript::bytecode
