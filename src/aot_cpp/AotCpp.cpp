#include "realscript/aot_cpp/AotCpp.h"

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/compiler/Compilation.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace realscript::aot {
namespace {

class Emitter {
public:
    void line(const std::string& value = {}) {
        text_ += value;
        text_ += '\n';
        ++line_;
    }

    void raw(const std::string& value) {
        text_ += value;
        line_ += static_cast<std::uint32_t>(
            std::count(value.begin(), value.end(), '\n'));
    }

    [[nodiscard]] std::uint32_t nextLine() const noexcept { return line_ + 1; }
    [[nodiscard]] const std::string& str() const noexcept { return text_; }

private:
    std::string text_;
    std::uint32_t line_ = 0;
};

struct FunctionView {
    const mir::Module* module = nullptr;
    const mir::Function* function = nullptr;
    std::string qualifiedName;
    std::string cppName;
};

struct TypeView {
    const semantic::TypeSymbol* type = nullptr;
    std::string cppName;
};

struct CallSite {
    std::size_t id = 0;
    const mir::Instruction* instruction = nullptr;
};

struct SourceMapRecord {
    semantic::SymbolId symbolId = 0;
    std::uint32_t generatedLine = 0;
    std::string sourcePath;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0;
};

std::string hexId(semantic::SymbolId id) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << id;
    return stream.str();
}

std::string primitiveExpression(semantic::PrimitiveType type) {
    return "semantic::PrimitiveType::" + std::string([&]() {
        switch (type) {
        case semantic::PrimitiveType::Error: return "Error";
        case semantic::PrimitiveType::Void: return "Void";
        case semantic::PrimitiveType::Bool: return "Bool";
        case semantic::PrimitiveType::Byte: return "Byte";
        case semantic::PrimitiveType::SByte: return "SByte";
        case semantic::PrimitiveType::Short: return "Short";
        case semantic::PrimitiveType::UShort: return "UShort";
        case semantic::PrimitiveType::Int: return "Int";
        case semantic::PrimitiveType::UInt: return "UInt";
        case semantic::PrimitiveType::Long: return "Long";
        case semantic::PrimitiveType::ULong: return "ULong";
        case semantic::PrimitiveType::Float: return "Float";
        case semantic::PrimitiveType::Double: return "Double";
        case semantic::PrimitiveType::Char: return "Char";
        case semantic::PrimitiveType::String: return "String";
        case semantic::PrimitiveType::Object: return "Object";
        case semantic::PrimitiveType::Struct: return "Struct";
        case semantic::PrimitiveType::Enum: return "Enum";
        case semantic::PrimitiveType::Array: return "Array";
        case semantic::PrimitiveType::Handle: return "Handle";
        case semantic::PrimitiveType::Null: return "Null";
        }
        return "Error";
    }());
}

std::string typeKindExpression(semantic::TypeKind kind) {
    switch (kind) {
    case semantic::TypeKind::Class: return "semantic::TypeKind::Class";
    case semantic::TypeKind::Struct: return "semantic::TypeKind::Struct";
    case semantic::TypeKind::Enum: return "semantic::TypeKind::Enum";
    }
    return "semantic::TypeKind::Class";
}

semantic::SymbolId exactTypeId(
    semantic::PrimitiveType type,
    const std::string& name) {
    return semantic::isExactType(type) && !name.empty()
        ? semantic::stableTypeId(name)
        : 0;
}

std::string conversionExpression(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::ConvertNullToString:
        return "semantic::ConversionKind::NullToString";
    case mir::Opcode::ConvertNullToObject:
        return "semantic::ConversionKind::NullToObject";
    case mir::Opcode::ConvertNullToArray:
        return "semantic::ConversionKind::NullToArray";
    case mir::Opcode::ConvertIntToLong:
        return "semantic::ConversionKind::IntToLong";
    case mir::Opcode::ConvertIntToDouble:
        return "semantic::ConversionKind::IntToDouble";
    case mir::Opcode::ConvertLongToDouble:
        return "semantic::ConversionKind::LongToDouble";
    case mir::Opcode::ConvertNumeric:
        return "semantic::ConversionKind::Numeric";
    default:
        return "semantic::ConversionKind::None";
    }
}

std::string unaryExpression(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::NegateInt: return "UnaryOperation::NegateInt";
    case mir::Opcode::NegateLong: return "UnaryOperation::NegateLong";
    case mir::Opcode::NegateDouble: return "UnaryOperation::NegateDouble";
    case mir::Opcode::LogicalNot: return "UnaryOperation::LogicalNot";
    default: return "UnaryOperation::LogicalNot";
    }
}

std::string binaryExpression(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::Equal: return "BinaryOperation::Equal";
    case mir::Opcode::NotEqual: return "BinaryOperation::NotEqual";
    case mir::Opcode::AddInt: return "BinaryOperation::AddInt";
    case mir::Opcode::SubtractInt: return "BinaryOperation::SubtractInt";
    case mir::Opcode::MultiplyInt: return "BinaryOperation::MultiplyInt";
    case mir::Opcode::DivideInt: return "BinaryOperation::DivideInt";
    case mir::Opcode::RemainderInt: return "BinaryOperation::RemainderInt";
    case mir::Opcode::LessInt: return "BinaryOperation::LessInt";
    case mir::Opcode::LessOrEqualInt: return "BinaryOperation::LessOrEqualInt";
    case mir::Opcode::GreaterInt: return "BinaryOperation::GreaterInt";
    case mir::Opcode::GreaterOrEqualInt: return "BinaryOperation::GreaterOrEqualInt";
    case mir::Opcode::AddLong: return "BinaryOperation::AddLong";
    case mir::Opcode::SubtractLong: return "BinaryOperation::SubtractLong";
    case mir::Opcode::MultiplyLong: return "BinaryOperation::MultiplyLong";
    case mir::Opcode::DivideLong: return "BinaryOperation::DivideLong";
    case mir::Opcode::RemainderLong: return "BinaryOperation::RemainderLong";
    case mir::Opcode::LessLong: return "BinaryOperation::LessLong";
    case mir::Opcode::LessOrEqualLong: return "BinaryOperation::LessOrEqualLong";
    case mir::Opcode::GreaterLong: return "BinaryOperation::GreaterLong";
    case mir::Opcode::GreaterOrEqualLong: return "BinaryOperation::GreaterOrEqualLong";
    case mir::Opcode::AddDouble: return "BinaryOperation::AddDouble";
    case mir::Opcode::SubtractDouble: return "BinaryOperation::SubtractDouble";
    case mir::Opcode::MultiplyDouble: return "BinaryOperation::MultiplyDouble";
    case mir::Opcode::DivideDouble: return "BinaryOperation::DivideDouble";
    case mir::Opcode::LessDouble: return "BinaryOperation::LessDouble";
    case mir::Opcode::LessOrEqualDouble: return "BinaryOperation::LessOrEqualDouble";
    case mir::Opcode::GreaterDouble: return "BinaryOperation::GreaterDouble";
    case mir::Opcode::GreaterOrEqualDouble:
        return "BinaryOperation::GreaterOrEqualDouble";
    default: return "BinaryOperation::Equal";
    }
}

bool isConversion(mir::Opcode opcode) {
    return (opcode >= mir::Opcode::ConvertNullToString &&
            opcode <= mir::Opcode::ConvertLongToDouble) ||
        opcode == mir::Opcode::ConvertNumeric;
}

bool isUnary(mir::Opcode opcode) {
    return opcode >= mir::Opcode::NegateInt &&
        opcode <= mir::Opcode::LogicalNot;
}

bool isBinary(mir::Opcode opcode) {
    return (opcode >= mir::Opcode::AddInt &&
        opcode <= mir::Opcode::GreaterOrEqualDouble) ||
        opcode == mir::Opcode::Equal || opcode == mir::Opcode::NotEqual;
}

std::size_t registerCount(const mir::Function& function) {
    mir::ValueId maximum = -1;
    const auto observe = [&](mir::ValueId value) {
        if (value > maximum) maximum = value;
    };
    for (const auto& block : function.blocks) {
        for (const auto& parameter : block.parameters) observe(parameter.value);
        for (const auto& instruction : block.instructions) {
            observe(instruction.result);
            for (const auto operand : instruction.operands) observe(operand);
        }
        observe(block.terminator.condition);
        observe(block.terminator.value);
        for (const auto value : block.terminator.arguments) observe(value);
        for (const auto value : block.terminator.falseArguments) observe(value);
    }
    return maximum < 0 ? 0 : static_cast<std::size_t>(maximum) + 1;
}

const mir::BasicBlock* findBlock(
    const mir::Function& function,
    mir::BlockId id) {
    for (const auto& block : function.blocks) {
        if (block.id == id) return &block;
    }
    return nullptr;
}

const debug::SequencePoint* findPoint(
    const mir::Function& function,
    mir::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator) {
    for (const auto& point : function.debugInfo.sequencePoints) {
        if (point.blockId == blockId &&
            point.instructionIndex == instructionIndex &&
            point.terminator == terminator) {
            return &point;
        }
    }
    return nullptr;
}

const debug::SourceFileInfo* findSource(
    const mir::Module& module,
    debug::SourceFileId id) {
    for (const auto& source : module.sourceFiles) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

const debug::SourceFileInfo* findFunctionSource(
    const mir::Module& module,
    const mir::Function& function) {
    if (const auto* byId = findSource(module, function.debugInfo.sourceFileId)) {
        return byId;
    }
    if (!function.debugInfo.sourceName.empty()) {
        for (const auto& source : module.sourceFiles) {
            if (source.path == function.debugInfo.sourceName) return &source;
        }
    }
    return module.sourceFiles.empty() ? nullptr : &module.sourceFiles.front();
}

std::optional<debug::SequencePoint> resolveSequencePoint(
    const mir::Module& module,
    const mir::Function& function,
    mir::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator,
    text::TextSpan fallbackSpan,
    bool* outExplicitSequencePoint = nullptr) {
    if (const auto* point = findPoint(function, blockId, instructionIndex, terminator)) {
        if (outExplicitSequencePoint) *outExplicitSequencePoint = true;
        return *point;
    }
    if (outExplicitSequencePoint) *outExplicitSequencePoint = false;
    if (fallbackSpan.empty()) return std::nullopt;
    const auto* source = findFunctionSource(module, function);
    if (!source || source->path.empty()) return std::nullopt;
    debug::SequencePoint synthesized;
    synthesized.blockId = blockId;
    synthesized.instructionIndex = instructionIndex;
    synthesized.terminator = terminator;
    synthesized.range = debug::makeSourceRange(*source, fallbackSpan);
    return synthesized;
}

bool emitLineDirective(
    Emitter& output,
    const mir::Module& module,
    const std::optional<debug::SequencePoint>& point,
    bool enabled,
    std::vector<SourceMapRecord>* sourceMap,
    semantic::SymbolId symbolId,
    bool recordInMap = true) {
    if (!point) return false;
    const auto* source = findSource(module, point->range.fileId);
    if (!source || source->path.empty()) return false;
    const auto line = static_cast<std::uint32_t>(point->range.start.line + 1);
    if (enabled) {
        output.line("#line " + std::to_string(line) + " \"" +
            escapeCppString(source->path) + "\"");
    }
    if (sourceMap && recordInMap) {
        SourceMapRecord entry;
        entry.symbolId = symbolId;
        entry.generatedLine = output.nextLine();
        entry.sourcePath = source->path;
        entry.sourceLine = line;
        entry.sourceColumn = static_cast<std::uint32_t>(point->range.start.column + 1);
        sourceMap->push_back(std::move(entry));
    }
    return enabled;
}

void emitGeneratedLineReset(
    Emitter& output,
    bool enabled) {
    if (!enabled) return;
    output.line("#line " + std::to_string(output.nextLine() + 1u) +
        " \"realscript_aot_generated.cpp\"");
}

std::string valueExpression(mir::ValueId id) {
    return "registers[" + std::to_string(id) + "]";
}

enum class InlineMathKind : std::uint8_t {
    None = 0,
    Fixed,
    Fixed16,
    Vec2,
    Vec3,
    Quat,
};

struct TypedSlotType {
    semantic::PrimitiveType primitive = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;

    friend constexpr bool operator==(TypedSlotType left, TypedSlotType right) noexcept {
        return left.primitive == right.primitive && left.typeId == right.typeId;
    }
    friend constexpr bool operator!=(TypedSlotType left, TypedSlotType right) noexcept {
        return !(left == right);
    }
};

InlineMathKind classifyInlineMath(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept {
    if (type != semantic::PrimitiveType::Struct || typeId == 0) {
        return InlineMathKind::None;
    }
    if (typeId == runtime::fixedTypeId()) return InlineMathKind::Fixed;
    if (typeId == runtime::fixed16TypeId()) return InlineMathKind::Fixed16;
    if (typeId == runtime::vec2TypeId()) return InlineMathKind::Vec2;
    if (typeId == runtime::vec3TypeId()) return InlineMathKind::Vec3;
    if (typeId == runtime::quatTypeId()) return InlineMathKind::Quat;
    return InlineMathKind::None;
}

const char* inlineMathFieldMember(
    InlineMathKind kind,
    std::size_t fieldIndex) noexcept {
    switch (kind) {
    case InlineMathKind::Fixed:
    case InlineMathKind::Fixed16:
        return fieldIndex == 0 ? "raw" : nullptr;
    case InlineMathKind::Vec2:
        return fieldIndex == 0 ? "x" : (fieldIndex == 1 ? "y" : nullptr);
    case InlineMathKind::Vec3:
        return fieldIndex == 0 ? "x"
            : (fieldIndex == 1 ? "y" : (fieldIndex == 2 ? "z" : nullptr));
    case InlineMathKind::Quat:
        return fieldIndex == 0 ? "x"
            : (fieldIndex == 1 ? "y"
                : (fieldIndex == 2 ? "z" : (fieldIndex == 3 ? "w" : nullptr)));
    case InlineMathKind::None:
        return nullptr;
    }
    return nullptr;
}

bool isTypedAotSlot(
    TypedSlotType slot,
    const std::unordered_set<semantic::SymbolId>* customStructs = nullptr) noexcept {
    switch (slot.primitive) {
    case semantic::PrimitiveType::Bool:
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::Long:
    case semantic::PrimitiveType::Float:
    case semantic::PrimitiveType::Double:
    case semantic::PrimitiveType::Null:
        return slot.typeId == 0;
    case semantic::PrimitiveType::Enum:
    case semantic::PrimitiveType::Object:
        return slot.typeId != 0;
    case semantic::PrimitiveType::Array:
        return true;
    case semantic::PrimitiveType::Struct:
        if (classifyInlineMath(slot.primitive, slot.typeId) !=
            InlineMathKind::None) {
            return true;
        }
        return customStructs != nullptr &&
            slot.typeId != 0 &&
            customStructs->find(slot.typeId) != customStructs->end();
    default:
        return false;
    }
}

std::vector<TypedSlotType> valueSlotTypes(const mir::Function& function) {
    std::vector<TypedSlotType> result(registerCount(function));
    for (const auto& block : function.blocks) {
        for (const auto& parameter : block.parameters) {
            if (parameter.value >= 0) {
                result[static_cast<std::size_t>(parameter.value)] = {
                    parameter.type,
                    parameter.typeId,
                };
            }
        }
        for (const auto& instruction : block.instructions) {
            if (instruction.result >= 0) {
                const auto typeId = instruction.resultTypeId != 0
                    ? instruction.resultTypeId
                    : (semantic::isExactType(instruction.resultType)
                        ? instruction.typeId
                        : 0);
                result[static_cast<std::size_t>(instruction.result)] = {
                    instruction.resultType,
                    typeId,
                };
            }
        }
    }
    return result;
}

constexpr std::int64_t TypedIntMinimum =
    std::numeric_limits<std::int32_t>::min();
constexpr std::int64_t TypedIntMaximum =
    std::numeric_limits<std::int32_t>::max();

struct TypedIntRange {
    std::int64_t minimum = TypedIntMinimum;
    std::int64_t maximum = TypedIntMaximum;
};

struct TypedLocalOrigin {
    std::size_t slot = std::numeric_limits<std::size_t>::max();
    std::uint64_t version = 0;

    [[nodiscard]] bool valid() const noexcept {
        return slot != std::numeric_limits<std::size_t>::max();
    }
};

struct TypedComparison {
    mir::Opcode opcode = mir::Opcode::LessInt;
    TypedIntRange left;
    TypedIntRange right;
    TypedLocalOrigin leftOrigin;
    TypedLocalOrigin rightOrigin;
};

struct TypedRangeAnalysis {
    std::set<mir::ValueId> safeCheckedArithmetic;
};

TypedIntRange typedFullIntRange() noexcept {
    return {TypedIntMinimum, TypedIntMaximum};
}

TypedIntRange typedValueRange(
    const std::vector<TypedIntRange>& values,
    mir::ValueId value) noexcept {
    if (value < 0 || static_cast<std::size_t>(value) >= values.size()) {
        return typedFullIntRange();
    }
    return values[static_cast<std::size_t>(value)];
}

TypedLocalOrigin typedValueOrigin(
    const std::vector<TypedLocalOrigin>& origins,
    mir::ValueId value) noexcept {
    if (value < 0 || static_cast<std::size_t>(value) >= origins.size()) {
        return {};
    }
    return origins[static_cast<std::size_t>(value)];
}

TypedRangeAnalysis analyzeTypedIntegerRanges(const mir::Function& function) {
    TypedRangeAnalysis analysis;
    if (function.blocks.empty()) return analysis;

    const auto valueCount = registerCount(function);
    std::unordered_map<mir::BlockId, std::size_t> blockIndices;
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        blockIndices.emplace(function.blocks[index].id, index);
    }
    const auto entry = blockIndices.find(0);
    if (entry == blockIndices.end()) return analysis;

    std::set<std::int64_t> thresholdSet{
        TypedIntMinimum, 0, TypedIntMaximum};
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != mir::Opcode::ConstantInt) continue;
            const auto value = std::clamp(
                instruction.integerImmediate,
                TypedIntMinimum,
                TypedIntMaximum);
            thresholdSet.insert(value);
            if (value > TypedIntMinimum) thresholdSet.insert(value - 1);
            if (value < TypedIntMaximum) thresholdSet.insert(value + 1);
        }
    }
    const std::vector<std::int64_t> wideningThresholds(
        thresholdSet.begin(), thresholdSet.end());

    std::vector<std::vector<TypedIntRange>> entryLocals(
        function.blocks.size(),
        std::vector<TypedIntRange>(
            function.localTypes.size(), typedFullIntRange()));
    std::vector<bool> hasEntry(function.blocks.size(), false);
    std::vector<std::vector<std::uint8_t>> lowerUpdates(
        function.blocks.size(),
        std::vector<std::uint8_t>(function.localTypes.size(), 0));
    std::vector<std::vector<std::uint8_t>> upperUpdates(
        function.blocks.size(),
        std::vector<std::uint8_t>(function.localTypes.size(), 0));
    std::vector<std::size_t> worklist;
    std::vector<bool> queued(function.blocks.size(), false);
    std::size_t workIndex = 0;

    auto& initial = entryLocals[entry->second];
    for (std::size_t slot = 0; slot < function.localTypes.size(); ++slot) {
        if (function.localTypes[slot] == semantic::PrimitiveType::Int) {
            initial[slot] = {0, 0};
        }
    }
    hasEntry[entry->second] = true;
    worklist.push_back(entry->second);
    queued[entry->second] = true;

    std::set<mir::ValueId> evaluatedArithmetic;
    std::set<mir::ValueId> unsafeArithmetic;

    const auto enqueue = [&](std::size_t index) {
        if (!queued[index]) {
            queued[index] = true;
            worklist.push_back(index);
        }
    };
    const auto mergeEntry = [&](mir::BlockId target,
                                const std::vector<TypedIntRange>& incoming) {
        const auto found = blockIndices.find(target);
        if (found == blockIndices.end()) return;
        const auto index = found->second;
        if (!hasEntry[index]) {
            entryLocals[index] = incoming;
            hasEntry[index] = true;
            enqueue(index);
            return;
        }
        bool changed = false;
        for (std::size_t slot = 0; slot < incoming.size(); ++slot) {
            if (function.localTypes[slot] != semantic::PrimitiveType::Int) continue;
            auto& current = entryLocals[index][slot];
            auto minimum = std::min(current.minimum, incoming[slot].minimum);
            auto maximum = std::max(current.maximum, incoming[slot].maximum);
            if (minimum < current.minimum) {
                auto& updates = lowerUpdates[index][slot];
                if (updates != std::numeric_limits<std::uint8_t>::max()) ++updates;
                if (updates > 8) {
                    const auto threshold = std::upper_bound(
                        wideningThresholds.begin(),
                        wideningThresholds.end(),
                        minimum);
                    minimum = threshold == wideningThresholds.begin()
                        ? TypedIntMinimum
                        : *std::prev(threshold);
                }
            }
            if (maximum > current.maximum) {
                auto& updates = upperUpdates[index][slot];
                if (updates != std::numeric_limits<std::uint8_t>::max()) ++updates;
                if (updates > 8) {
                    const auto threshold = std::lower_bound(
                        wideningThresholds.begin(),
                        wideningThresholds.end(),
                        maximum);
                    maximum = threshold == wideningThresholds.end()
                        ? TypedIntMaximum
                        : *threshold;
                }
            }
            if (minimum != current.minimum || maximum != current.maximum) {
                current = {minimum, maximum};
                changed = true;
            }
        }
        if (changed) enqueue(index);
    };

    while (workIndex < worklist.size()) {
        const auto blockIndex = worklist[workIndex++];
        queued[blockIndex] = false;
        const auto& block = function.blocks[blockIndex];
        auto locals = entryLocals[blockIndex];
        std::vector<std::uint64_t> localVersions(function.localTypes.size(), 0);
        std::vector<TypedIntRange> values(
            valueCount, typedFullIntRange());
        std::vector<TypedLocalOrigin> origins(valueCount);
        std::unordered_map<mir::ValueId, TypedComparison> comparisons;

        const auto setValue = [&](mir::ValueId value,
                                  TypedIntRange range,
                                  TypedLocalOrigin origin = {}) {
            if (value < 0 || static_cast<std::size_t>(value) >= values.size()) {
                return;
            }
            values[static_cast<std::size_t>(value)] = range;
            origins[static_cast<std::size_t>(value)] = origin;
        };
        const auto noteArithmetic = [&](const mir::Instruction& instruction,
                                        bool safe) {
            if (!instruction.checkedArithmetic || instruction.result < 0) return;
            evaluatedArithmetic.insert(instruction.result);
            if (!safe) unsafeArithmetic.insert(instruction.result);
        };

        for (const auto& instruction : block.instructions) {
            const auto operand = [&](std::size_t index) {
                return index < instruction.operands.size()
                    ? typedValueRange(values, instruction.operands[index])
                    : typedFullIntRange();
            };
            switch (instruction.opcode) {
            case mir::Opcode::Parameter:
                setValue(instruction.result, typedFullIntRange());
                break;
            case mir::Opcode::ConstantInt:
                setValue(instruction.result,
                    {instruction.integerImmediate, instruction.integerImmediate});
                break;
            case mir::Opcode::LoadLocal:
                if (instruction.localIndex < locals.size()) {
                    setValue(instruction.result, locals[instruction.localIndex],
                        {instruction.localIndex,
                         localVersions[instruction.localIndex]});
                }
                break;
            case mir::Opcode::StoreLocal:
                if (instruction.localIndex < locals.size() &&
                    !instruction.operands.empty()) {
                    locals[instruction.localIndex] = operand(0);
                    ++localVersions[instruction.localIndex];
                }
                break;
            case mir::Opcode::AddInt:
            case mir::Opcode::SubtractInt: {
                const auto left = operand(0);
                const auto right = operand(1);
                const auto minimum = instruction.opcode == mir::Opcode::AddInt
                    ? left.minimum + right.minimum
                    : left.minimum - right.maximum;
                const auto maximum = instruction.opcode == mir::Opcode::AddInt
                    ? left.maximum + right.maximum
                    : left.maximum - right.minimum;
                const auto safe = minimum >= TypedIntMinimum &&
                    maximum <= TypedIntMaximum;
                noteArithmetic(instruction, safe);
                setValue(instruction.result,
                    safe ? TypedIntRange{minimum, maximum} : typedFullIntRange());
                break;
            }
            case mir::Opcode::MultiplyInt: {
                const auto left = operand(0);
                const auto right = operand(1);
                const std::int64_t candidates[] = {
                    left.minimum * right.minimum,
                    left.minimum * right.maximum,
                    left.maximum * right.minimum,
                    left.maximum * right.maximum,
                };
                const auto bounds = std::minmax_element(
                    std::begin(candidates), std::end(candidates));
                const auto safe = *bounds.first >= TypedIntMinimum &&
                    *bounds.second <= TypedIntMaximum;
                noteArithmetic(instruction, safe);
                setValue(instruction.result,
                    safe ? TypedIntRange{*bounds.first, *bounds.second}
                         : typedFullIntRange());
                break;
            }
            case mir::Opcode::NegateInt: {
                const auto value = operand(0);
                const auto safe = value.minimum > TypedIntMinimum;
                noteArithmetic(instruction, safe);
                setValue(instruction.result,
                    safe ? TypedIntRange{-value.maximum, -value.minimum}
                         : typedFullIntRange());
                break;
            }
            case mir::Opcode::LessInt:
            case mir::Opcode::LessOrEqualInt:
            case mir::Opcode::GreaterInt:
            case mir::Opcode::GreaterOrEqualInt:
                if (instruction.result >= 0 && instruction.operands.size() >= 2) {
                    comparisons.emplace(instruction.result, TypedComparison{
                        instruction.opcode,
                        operand(0),
                        operand(1),
                        typedValueOrigin(origins, instruction.operands[0]),
                        typedValueOrigin(origins, instruction.operands[1]),
                    });
                }
                break;
            default:
                if (instruction.result >= 0 &&
                    instruction.resultType == semantic::PrimitiveType::Int) {
                    setValue(instruction.result, typedFullIntRange());
                }
                break;
            }
        }

        const auto refinedLocals = [&](const TypedComparison& comparison,
                                       bool takeTrue) {
            auto result = locals;
            bool reachable = true;
            const auto refineMinimum = [&](TypedLocalOrigin origin,
                                           std::int64_t minimum) {
                if (!origin.valid() || origin.slot >= result.size() ||
                    origin.version != localVersions[origin.slot]) return;
                result[origin.slot].minimum =
                    std::max(result[origin.slot].minimum, minimum);
                reachable &= result[origin.slot].minimum <=
                    result[origin.slot].maximum;
            };
            const auto refineMaximum = [&](TypedLocalOrigin origin,
                                           std::int64_t maximum) {
                if (!origin.valid() || origin.slot >= result.size() ||
                    origin.version != localVersions[origin.slot]) return;
                result[origin.slot].maximum =
                    std::min(result[origin.slot].maximum, maximum);
                reachable &= result[origin.slot].minimum <=
                    result[origin.slot].maximum;
            };
            switch (comparison.opcode) {
            case mir::Opcode::LessInt:
                if (takeTrue) {
                    refineMaximum(comparison.leftOrigin,
                        comparison.right.maximum - 1);
                    refineMinimum(comparison.rightOrigin,
                        comparison.left.minimum + 1);
                } else {
                    refineMinimum(comparison.leftOrigin,
                        comparison.right.minimum);
                    refineMaximum(comparison.rightOrigin,
                        comparison.left.maximum);
                }
                break;
            case mir::Opcode::LessOrEqualInt:
                if (takeTrue) {
                    refineMaximum(comparison.leftOrigin,
                        comparison.right.maximum);
                    refineMinimum(comparison.rightOrigin,
                        comparison.left.minimum);
                } else {
                    refineMinimum(comparison.leftOrigin,
                        comparison.right.minimum + 1);
                    refineMaximum(comparison.rightOrigin,
                        comparison.left.maximum - 1);
                }
                break;
            case mir::Opcode::GreaterInt:
                if (takeTrue) {
                    refineMinimum(comparison.leftOrigin,
                        comparison.right.minimum + 1);
                    refineMaximum(comparison.rightOrigin,
                        comparison.left.maximum - 1);
                } else {
                    refineMaximum(comparison.leftOrigin,
                        comparison.right.maximum);
                    refineMinimum(comparison.rightOrigin,
                        comparison.left.minimum);
                }
                break;
            case mir::Opcode::GreaterOrEqualInt:
                if (takeTrue) {
                    refineMinimum(comparison.leftOrigin,
                        comparison.right.minimum);
                    refineMaximum(comparison.rightOrigin,
                        comparison.left.maximum);
                } else {
                    refineMaximum(comparison.leftOrigin,
                        comparison.right.maximum - 1);
                    refineMinimum(comparison.rightOrigin,
                        comparison.left.minimum + 1);
                }
                break;
            default:
                break;
            }
            return std::pair{reachable, std::move(result)};
        };

        if (block.terminator.kind == mir::TerminatorKind::Jump) {
            mergeEntry(block.terminator.target, locals);
        } else if (block.terminator.kind == mir::TerminatorKind::Branch) {
            const auto comparison = comparisons.find(block.terminator.condition);
            if (comparison == comparisons.end()) {
                mergeEntry(block.terminator.target, locals);
                mergeEntry(block.terminator.falseTarget, locals);
            } else {
                auto trueState = refinedLocals(comparison->second, true);
                auto falseState = refinedLocals(comparison->second, false);
                if (trueState.first) {
                    mergeEntry(block.terminator.target, trueState.second);
                }
                if (falseState.first) {
                    mergeEntry(block.terminator.falseTarget, falseState.second);
                }
            }
        }
    }

    for (const auto value : evaluatedArithmetic) {
        if (unsafeArithmetic.find(value) == unsafeArithmetic.end()) {
            analysis.safeCheckedArithmetic.insert(value);
        }
    }
    return analysis;
}

bool supportsTypedPrimitiveAot(
    const mir::Function& function,
    const std::unordered_set<semantic::SymbolId>& customStructs) {
    if (!function.exceptionHandlers.empty()) {
        return false;
    }
    if (function.returnType != semantic::PrimitiveType::Void &&
        !isTypedAotSlot({function.returnType, function.returnTypeId}, &customStructs)) {
        return false;
    }
    bool usesReferenceSlots =
        function.returnType == semantic::PrimitiveType::Object ||
        function.returnType == semantic::PrimitiveType::Array;
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        const auto typeId = index < function.parameterTypeIds.size()
            ? function.parameterTypeIds[index]
            : 0;
        if (!isTypedAotSlot({function.parameterTypes[index], typeId}, &customStructs)) {
            return false;
        }
        if (function.parameterTypes[index] == semantic::PrimitiveType::Object ||
            function.parameterTypes[index] == semantic::PrimitiveType::Array) {
            usesReferenceSlots = true;
        }
    }
    for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
        const auto typeId = index < function.localTypeIds.size()
            ? function.localTypeIds[index]
            : 0;
        if (!isTypedAotSlot({function.localTypes[index], typeId}, &customStructs)) {
            return false;
        }
        if (function.localTypes[index] == semantic::PrimitiveType::Object ||
            function.localTypes[index] == semantic::PrimitiveType::Array) {
            usesReferenceSlots = true;
        }
    }
    const auto types = valueSlotTypes(function);
    for (const auto& slot : types) {
        if (slot.primitive != semantic::PrimitiveType::Error &&
            slot.primitive != semantic::PrimitiveType::Void &&
            !isTypedAotSlot(slot, &customStructs)) {
            return false;
        }
        if (slot.primitive == semantic::PrimitiveType::Object ||
            slot.primitive == semantic::PrimitiveType::Array) {
            usesReferenceSlots = true;
        }
    }
    const auto operandSlot = [&](const mir::Instruction& instruction,
                                 std::size_t index) -> TypedSlotType {
        if (index >= instruction.operands.size() ||
            instruction.operands[index] < 0) {
            return {};
        }
        return types[static_cast<std::size_t>(instruction.operands[index])];
    };
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            switch (instruction.opcode) {
            case mir::Opcode::Parameter:
            case mir::Opcode::ConstantInt:
            case mir::Opcode::ConstantDouble:
            case mir::Opcode::ConstantBool:
            case mir::Opcode::ConstantNull:
            case mir::Opcode::ConvertNullToObject:
            case mir::Opcode::ConvertNullToArray:
            case mir::Opcode::LoadLocal:
            case mir::Opcode::StoreLocal:
            case mir::Opcode::ConvertIntToLong:
            case mir::Opcode::ConvertIntToDouble:
            case mir::Opcode::ConvertLongToDouble:
            case mir::Opcode::ConvertNumeric:
            case mir::Opcode::LessInt:
            case mir::Opcode::LessOrEqualInt:
            case mir::Opcode::GreaterInt:
            case mir::Opcode::GreaterOrEqualInt:
            case mir::Opcode::AddInt:
            case mir::Opcode::SubtractInt:
            case mir::Opcode::MultiplyInt:
            case mir::Opcode::DivideInt:
            case mir::Opcode::RemainderInt:
            case mir::Opcode::NegateInt:
            case mir::Opcode::LessLong:
            case mir::Opcode::LessOrEqualLong:
            case mir::Opcode::GreaterLong:
            case mir::Opcode::GreaterOrEqualLong:
            case mir::Opcode::AddLong:
            case mir::Opcode::SubtractLong:
            case mir::Opcode::MultiplyLong:
            case mir::Opcode::DivideLong:
            case mir::Opcode::RemainderLong:
            case mir::Opcode::NegateLong:
            case mir::Opcode::LessDouble:
            case mir::Opcode::LessOrEqualDouble:
            case mir::Opcode::GreaterDouble:
            case mir::Opcode::GreaterOrEqualDouble:
            case mir::Opcode::AddDouble:
            case mir::Opcode::SubtractDouble:
            case mir::Opcode::MultiplyDouble:
            case mir::Opcode::DivideDouble:
            case mir::Opcode::NegateDouble:
            case mir::Opcode::CheckNotNull:
            case mir::Opcode::ArrayLength:
                break;
            case mir::Opcode::LoadField:
                if (!isTypedAotSlot({instruction.resultType, instruction.resultTypeId}, &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::StoreField:
                if (instruction.operands.size() < 2 ||
                    !isTypedAotSlot(operandSlot(instruction, 1), &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::NewArray:
                if (instruction.operands.size() != 1 ||
                    operandSlot(instruction, 0).primitive != semantic::PrimitiveType::Int ||
                    !isTypedAotSlot({instruction.elementType, instruction.elementTypeId}, &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::LoadElement:
                if (operandSlot(instruction, 1).primitive != semantic::PrimitiveType::Int ||
                    !isTypedAotSlot({instruction.elementType, instruction.elementTypeId}, &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::StoreElement:
                if (operandSlot(instruction, 1).primitive != semantic::PrimitiveType::Int ||
                    !isTypedAotSlot({instruction.elementType, instruction.elementTypeId}, &customStructs) ||
                    !isTypedAotSlot(operandSlot(instruction, 2), &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::LogicalNot:
                if (operandSlot(instruction, 0).primitive !=
                    semantic::PrimitiveType::Bool) return false;
                break;
            case mir::Opcode::Equal:
            case mir::Opcode::NotEqual:
                if (operandSlot(instruction, 0) !=
                    operandSlot(instruction, 1)) return false;
                break;
            case mir::Opcode::NewStruct: {
                const auto typeId = instruction.resultTypeId != 0
                    ? instruction.resultTypeId
                    : instruction.typeId;
                if (classifyInlineMath(semantic::PrimitiveType::Struct, typeId) ==
                        InlineMathKind::None &&
                    customStructs.find(typeId) == customStructs.end()) {
                    return false;
                }
                break;
            }
            case mir::Opcode::LoadStructField:
            case mir::Opcode::StoreStructField: {
                const auto kind = classifyInlineMath(
                    semantic::PrimitiveType::Struct, instruction.typeId);
                if (kind != InlineMathKind::None) {
                    if (inlineMathFieldMember(kind, instruction.fieldIndex) == nullptr) {
                        return false;
                    }
                } else if (customStructs.find(instruction.typeId) == customStructs.end() ||
                           instruction.fieldIndex >= 16) {
                    return false;
                }
                break;
            }
            case mir::Opcode::Call: {
                if (instruction.virtualDispatch || instruction.interfaceDispatch) {
                    return false;
                }
                const std::string_view symName(instruction.symbolName);
                const bool isEcsIntrinsic =
                    symName.rfind("$intrinsic::static_get::ecs_col::", 0) == 0 ||
                    symName.rfind("$intrinsic::static_get::ecs_ent::", 0) == 0 ||
                    symName.rfind("$intrinsic::ecs_writeback::", 0) == 0;
                if (usesReferenceSlots && !isEcsIntrinsic) {
                    return false;
                }
                if (!symName.empty() &&
                    symName.rfind("$intrinsic::", 0) == 0 &&
                    !isEcsIntrinsic) {
                    return false;
                }
                if (instruction.resultType != semantic::PrimitiveType::Void &&
                    !isTypedAotSlot({instruction.resultType, instruction.resultTypeId}, &customStructs)) {
                    return false;
                }
                if (instruction.operands.size() != instruction.parameterTypes.size()) {
                    return false;
                }
                for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                    const auto paramTypeId = index < instruction.parameterTypeIds.size()
                        ? instruction.parameterTypeIds[index]
                        : 0;
                    const TypedSlotType expectedSlot{
                        instruction.parameterTypes[index],
                        paramTypeId,
                    };
                    if (!isTypedAotSlot(expectedSlot, &customStructs) ||
                        operandSlot(instruction, index) != expectedSlot) {
                        return false;
                    }
                }
                break;
            }
            default:
                return false;
            }
        }
        switch (block.terminator.kind) {
        case mir::TerminatorKind::ReturnVoid:
        case mir::TerminatorKind::ReturnValue:
        case mir::TerminatorKind::Jump:
        case mir::TerminatorKind::Branch:
            break;
        case mir::TerminatorKind::Throw:
        case mir::TerminatorKind::None:
            return false;
        }
    }
    return true;
}

std::string typedCppType(TypedSlotType slot) {
    switch (slot.primitive) {
    case semantic::PrimitiveType::Bool:
        return "bool";
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::Long:
        return "std::int64_t";
    case semantic::PrimitiveType::Float:
        return "float";
    case semantic::PrimitiveType::Double:
        return "double";
    case semantic::PrimitiveType::Enum:
        return "runtime::EnumValue";
    case semantic::PrimitiveType::Object:
    case semantic::PrimitiveType::Array:
    case semantic::PrimitiveType::Null:
        return "runtime::Value";
    case semantic::PrimitiveType::Struct:
        switch (classifyInlineMath(slot.primitive, slot.typeId)) {
        case InlineMathKind::Fixed:   return "runtime::FixedValue";
        case InlineMathKind::Fixed16: return "runtime::Fixed16Value";
        case InlineMathKind::Vec2:    return "runtime::Vec2Value";
        case InlineMathKind::Vec3:    return "runtime::Vec3Value";
        case InlineMathKind::Quat:    return "runtime::QuatValue";
        case InlineMathKind::None:    return "AotStruct_" + hexId(slot.typeId);
        }
        break;
    default:
        break;
    }
    return "std::int64_t";
}

std::string typedDefaultValue(TypedSlotType slot) {
    switch (slot.primitive) {
    case semantic::PrimitiveType::Bool:
        return "false";
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::Long:
        return "std::int64_t{0}";
    case semantic::PrimitiveType::Float:
        return "0.0f";
    case semantic::PrimitiveType::Double:
        return "0.0";
    case semantic::PrimitiveType::Enum:
        return "runtime::EnumValue{0x" + hexId(slot.typeId) + "ULL, 0}";
    case semantic::PrimitiveType::Object:
        return "runtime::Value{runtime::NullObject{}}";
    case semantic::PrimitiveType::Array:
        return "runtime::Value{runtime::NullArray{}}";
    case semantic::PrimitiveType::Null:
        return "runtime::Value{std::monostate{}}";
    case semantic::PrimitiveType::Struct:
        switch (classifyInlineMath(slot.primitive, slot.typeId)) {
        case InlineMathKind::Fixed:   return "runtime::FixedValue{}";
        case InlineMathKind::Fixed16: return "runtime::Fixed16Value{}";
        case InlineMathKind::Vec2:    return "runtime::Vec2Value{}";
        case InlineMathKind::Vec3:    return "runtime::Vec3Value{}";
        case InlineMathKind::Quat:    return "runtime::QuatValue{}";
        case InlineMathKind::None:    return "AotStruct_" + hexId(slot.typeId) + "{}";
        }
        break;
    default:
        break;
    }
    return "std::int64_t{0}";
}

std::string typedValueExpression(mir::ValueId id) {
    return "typedRegister_" + std::to_string(id);
}

std::string typedLocalExpression(std::size_t index) {
    return "typedLocal_" + std::to_string(index);
}

std::string int64Literal(std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::min()) {
        return "std::numeric_limits<std::int64_t>::min()";
    }
    return std::to_string(value) + "LL";
}

std::string typedScalarFunctionSignature(
    const FunctionView& view,
    bool declarationOnly) {
    const auto& function = *view.function;
    std::ostringstream out;
    out << "template <bool FastAccounting, bool DeterminismAccounting, bool TrustedRelease"
        << (declarationOnly ? " = false" : "") << ">\n";
    out << "static bool " << view.cppName << "_scalar(\n";
    out << "    ExecutionContext& context";
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        const auto typeId = index < function.parameterTypeIds.size()
            ? function.parameterTypeIds[index]
            : 0;
        out << ",\n    "
            << typedCppType({function.parameterTypes[index], typeId})
            << " typedArg_" << index;
    }
    if (function.returnType != semantic::PrimitiveType::Void) {
        out << ",\n    "
            << typedCppType({function.returnType, function.returnTypeId})
            << "& typedOutResult";
    }
    out << ")" << (declarationOnly ? ";" : " {");
    return out.str();
}

void emitTypedTransfer(
    Emitter& output,
    const mir::Function& function,
    mir::BlockId target,
    const std::vector<mir::ValueId>& arguments,
    const std::string& prefix,
    const std::string& indent) {
    const auto* block = findBlock(function, target);
    if (!block) return;
    output.line(indent + "if constexpr (FastAccounting && !TrustedRelease) {");
    output.line(indent + "    rawAccounting.branch();");
    output.line(indent + "} else if constexpr (DeterminismAccounting && !TrustedRelease) {");
    output.line(indent + "    determinismAccounting.branch(" +
        std::to_string(target) + ");");
    output.line(indent + "} else if constexpr (!TrustedRelease) {");
    output.line(indent + "    if (!context.branch(" +
        std::to_string(target) + ")) return false;");
    output.line(indent + "}");
    for (std::size_t index = 0;
         index < arguments.size() && index < block->parameters.size();
         ++index) {
        output.line(indent + "const auto " + prefix + "_" +
            std::to_string(index) + " = " +
            typedValueExpression(arguments[index]) + ";");
    }
    for (std::size_t index = 0;
         index < arguments.size() && index < block->parameters.size();
         ++index) {
        output.line(indent +
            typedValueExpression(block->parameters[index].value) + " = " +
            prefix + "_" + std::to_string(index) + ";");
    }
    output.line(indent + "goto typedBlock_" + std::to_string(target) + ";");
}

void emitTypedRawConsume(
    Emitter& output,
    std::uint64_t count,
    const std::string& indent) {
    if (count == 0) return;
    output.line(indent + "if constexpr (FastAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!rawAccounting.consume(" +
        std::to_string(count) + "ULL)) return false;");
    output.line(indent + "}");
}

void emitTypedConsume(
    Emitter& output,
    mir::Opcode opcode,
    const std::string& indent) {
    const auto operationId = std::to_string(
        runtime::determinismOperationId(mir::opcodeName(opcode))) + "ULL";
    output.line(indent + "if constexpr (DeterminismAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!determinismAccounting.consume(" +
        operationId + ")) return false;");
    output.line(indent + "} else if constexpr (!FastAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!context.consume(" +
        operationId + ", \"" +
        escapeCppString(mir::opcodeName(opcode)) +
        "\")) return false;");
    output.line(indent + "}");
}

void emitTypedConsume(
    Emitter& output,
    mir::TerminatorKind terminator,
    const std::string& indent) {
    const auto operationId = std::to_string(
        runtime::determinismOperationId(
            mir::terminatorName(terminator))) + "ULL";
    output.line(indent + "if constexpr (DeterminismAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!determinismAccounting.consume(" +
        operationId + ")) return false;");
    output.line(indent + "} else if constexpr (!FastAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!context.consume(" +
        operationId + ", \"" +
        escapeCppString(mir::terminatorName(terminator)) +
        "\")) return false;");
    output.line(indent + "}");
}

bool typedCheckedArithmeticProvenSafe(
    const TypedRangeAnalysis& analysis,
    const mir::Instruction& instruction) noexcept {
    return instruction.result >= 0 &&
        analysis.safeCheckedArithmetic.find(instruction.result) !=
            analysis.safeCheckedArithmetic.end();
}

bool typedInstructionCanFail(
    const TypedRangeAnalysis& analysis,
    const mir::Instruction& instruction) noexcept {
    switch (instruction.opcode) {
    case mir::Opcode::Parameter:
    case mir::Opcode::Call:
    case mir::Opcode::DivideInt:
    case mir::Opcode::RemainderInt:
    case mir::Opcode::DivideLong:
    case mir::Opcode::RemainderLong:
    case mir::Opcode::CheckNotNull:
    case mir::Opcode::LoadField:
    case mir::Opcode::StoreField:
    case mir::Opcode::ArrayLength:
    case mir::Opcode::LoadElement:
    case mir::Opcode::StoreElement:
    case mir::Opcode::NewStruct:
    case mir::Opcode::LoadStructField:
    case mir::Opcode::StoreStructField:
        return true;
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt:
    case mir::Opcode::NegateInt:
        return instruction.checkedArithmetic &&
            !typedCheckedArithmeticProvenSafe(analysis, instruction);
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong:
    case mir::Opcode::NegateLong:
    case mir::Opcode::ConvertNumeric:
        return instruction.checkedArithmetic;
    default:
        return false;
    }
}

std::vector<std::uint64_t> typedRawConsumeBatches(
    const mir::BasicBlock& block,
    const TypedRangeAnalysis& analysis) {
    std::vector<std::uint64_t> result(block.instructions.size() + 1, 0);
    std::size_t batchStart = 0;
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        if (!typedInstructionCanFail(analysis, block.instructions[index])) continue;
        result[batchStart] = index - batchStart + 1;
        batchStart = index + 1;
    }
    result[batchStart] = block.instructions.size() - batchStart + 1;
    return result;
}

struct VectorizedEcsColumnBinding {
    std::size_t colLocalIndex = 0;
    std::size_t iterLocalIndex = 0;
    semantic::SymbolId elemTypeId = 0;
    bool isWrite = false;
};

struct VectorizedEcsLoopPlan {
    mir::BlockId headerBlockId = 0;
    mir::BlockId bodyBlockId = 0;
    mir::BlockId incrementBlockId = 0;
    mir::BlockId exitBlockId = 0;
    std::size_t indexLocalIndex = 0;
    std::size_t bodyPreambleCount = 0;
    bool hasSparseWritebackCall = false;
    bool usesIndexInsideBody = false;
    std::vector<VectorizedEcsColumnBinding> columns;
};

bool isPureVectorizableBodyInstruction(const mir::Instruction& instruction) noexcept {
    switch (instruction.opcode) {
    case mir::Opcode::ConstantInt:
    case mir::Opcode::ConstantDouble:
    case mir::Opcode::ConstantBool:
    case mir::Opcode::LoadLocal:
    case mir::Opcode::StoreLocal:
    case mir::Opcode::ConvertIntToLong:
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt:
    case mir::Opcode::NegateInt:
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong:
    case mir::Opcode::NegateLong:
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble:
    case mir::Opcode::NegateDouble:
    case mir::Opcode::LogicalNot:
    case mir::Opcode::Equal:
    case mir::Opcode::NotEqual:
    case mir::Opcode::LessInt:
    case mir::Opcode::LessOrEqualInt:
    case mir::Opcode::GreaterInt:
    case mir::Opcode::GreaterOrEqualInt:
    case mir::Opcode::LessLong:
    case mir::Opcode::LessOrEqualLong:
    case mir::Opcode::GreaterLong:
    case mir::Opcode::GreaterOrEqualLong:
    case mir::Opcode::LessDouble:
    case mir::Opcode::LessOrEqualDouble:
    case mir::Opcode::GreaterDouble:
    case mir::Opcode::GreaterOrEqualDouble:
    case mir::Opcode::NewStruct:
    case mir::Opcode::LoadStructField:
    case mir::Opcode::StoreStructField:
        return true;
    default:
        return false;
    }
}

std::unordered_map<mir::BlockId, VectorizedEcsLoopPlan> analyzeVectorizedEcsLoops(
    const mir::Function& function,
    const std::unordered_map<std::size_t, semantic::SymbolId>& arrayLocalStructElemType) {
    std::unordered_map<mir::BlockId, VectorizedEcsLoopPlan> plans;
    std::unordered_map<mir::ValueId, std::int64_t> constantInts;
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.opcode == mir::Opcode::ConstantInt &&
                inst.result >= 0 &&
                inst.resultType != semantic::PrimitiveType::Enum) {
                constantInts[inst.result] = inst.integerImmediate;
            }
        }
    }

    for (const auto& headerBlock : function.blocks) {
        if (headerBlock.instructions.size() != 4 ||
            headerBlock.terminator.kind != mir::TerminatorKind::Branch ||
            !headerBlock.terminator.arguments.empty() ||
            !headerBlock.terminator.falseArguments.empty()) {
            continue;
        }
        const auto& idxLoad = headerBlock.instructions[0];
        if (idxLoad.opcode != mir::Opcode::LoadLocal ||
            idxLoad.result < 0 ||
            idxLoad.localIndex >= function.localTypes.size() ||
            function.localTypes[idxLoad.localIndex] != semantic::PrimitiveType::Int) {
            continue;
        }

        VectorizedEcsLoopPlan plan;
        plan.headerBlockId = headerBlock.id;
        plan.indexLocalIndex = idxLoad.localIndex;
        plan.exitBlockId = headerBlock.terminator.falseTarget;
        const mir::ValueId indexVal = idxLoad.result;

        const mir::BasicBlock* curBlock = &headerBlock;
        std::size_t instOffset = 1;
        bool chainValid = true;
        while (curBlock) {
            if (curBlock->instructions.size() != instOffset + 3 ||
                curBlock->terminator.kind != mir::TerminatorKind::Branch ||
                curBlock->terminator.falseTarget != plan.exitBlockId ||
                !curBlock->terminator.arguments.empty() ||
                !curBlock->terminator.falseArguments.empty()) {
                chainValid = false;
                break;
            }
            const auto& colLoad = curBlock->instructions[instOffset + 0];
            const auto& lenInst = curBlock->instructions[instOffset + 1];
            const auto& cmpInst = curBlock->instructions[instOffset + 2];
            if (colLoad.opcode != mir::Opcode::LoadLocal ||
                colLoad.result < 0 ||
                colLoad.localIndex >= function.localTypes.size() ||
                function.localTypes[colLoad.localIndex] != semantic::PrimitiveType::Array ||
                lenInst.opcode != mir::Opcode::ArrayLength ||
                lenInst.operands.size() != 1 ||
                lenInst.operands[0] != colLoad.result ||
                lenInst.result < 0 ||
                cmpInst.opcode != mir::Opcode::LessInt ||
                cmpInst.operands.size() != 2 ||
                cmpInst.operands[0] != indexVal ||
                cmpInst.operands[1] != lenInst.result ||
                cmpInst.result != curBlock->terminator.condition) {
                chainValid = false;
                break;
            }
            plan.columns.push_back(VectorizedEcsColumnBinding{
                colLoad.localIndex, 0, 0, false});
            if (plan.columns.size() > 16) {
                chainValid = false;
                break;
            }
            const auto* nextBlock = findBlock(function, curBlock->terminator.target);
            if (!nextBlock) {
                chainValid = false;
                break;
            }
            if (nextBlock->instructions.size() == 3 &&
                nextBlock->instructions[0].opcode == mir::Opcode::LoadLocal &&
                nextBlock->instructions[1].opcode == mir::Opcode::ArrayLength &&
                nextBlock->instructions[2].opcode == mir::Opcode::LessInt &&
                nextBlock->terminator.kind == mir::TerminatorKind::Branch &&
                nextBlock->terminator.falseTarget == plan.exitBlockId) {
                curBlock = nextBlock;
                instOffset = 0;
                continue;
            }
            plan.bodyBlockId = nextBlock->id;
            break;
        }
        if (!chainValid || plan.columns.empty()) continue;

        const auto* bodyBlock = findBlock(function, plan.bodyBlockId);
        if (!bodyBlock ||
            bodyBlock->terminator.kind != mir::TerminatorKind::Jump ||
            !bodyBlock->terminator.arguments.empty()) {
            continue;
        }
        plan.incrementBlockId = bodyBlock->terminator.target;
        plan.bodyPreambleCount = 4 * plan.columns.size();
        if (bodyBlock->instructions.size() < plan.bodyPreambleCount) continue;

        bool preambleValid = true;
        std::set<std::size_t> usedLocals{plan.indexLocalIndex};
        std::set<semantic::SymbolId> usedElemTypes;
        for (std::size_t c = 0; c < plan.columns.size(); ++c) {
            if (!usedLocals.insert(plan.columns[c].colLocalIndex).second) {
                preambleValid = false;
                break;
            }
            const std::size_t base = 4 * c;
            const auto& b0 = bodyBlock->instructions[base + 0];
            const auto& b1 = bodyBlock->instructions[base + 1];
            const auto& b2 = bodyBlock->instructions[base + 2];
            const auto& b3 = bodyBlock->instructions[base + 3];
            if (b0.opcode != mir::Opcode::LoadLocal ||
                b0.localIndex != plan.columns[c].colLocalIndex ||
                b0.result < 0 ||
                b1.opcode != mir::Opcode::LoadLocal ||
                b1.localIndex != plan.indexLocalIndex ||
                b1.result < 0 ||
                b2.opcode != mir::Opcode::LoadElement ||
                b2.operands.size() != 2 ||
                b2.operands[0] != b0.result ||
                b2.operands[1] != b1.result ||
                b2.result < 0 ||
                b2.elementType != semantic::PrimitiveType::Struct ||
                b2.elementTypeId == 0 ||
                classifyInlineMath(b2.elementType, b2.elementTypeId) != InlineMathKind::None ||
                b3.opcode != mir::Opcode::StoreLocal ||
                b3.operands.size() != 1 ||
                b3.operands[0] != b2.result ||
                b3.localIndex >= function.localTypes.size() ||
                function.localTypes[b3.localIndex] != semantic::PrimitiveType::Struct) {
                preambleValid = false;
                break;
            }
            const auto elemTypeIt = arrayLocalStructElemType.find(plan.columns[c].colLocalIndex);
            if (elemTypeIt == arrayLocalStructElemType.end() ||
                elemTypeIt->second != b2.elementTypeId ||
                !usedElemTypes.insert(b2.elementTypeId).second ||
                !usedLocals.insert(b3.localIndex).second) {
                preambleValid = false;
                break;
            }
            plan.columns[c].iterLocalIndex = b3.localIndex;
            plan.columns[c].elemTypeId = b2.elementTypeId;
        }
        if (!preambleValid) continue;

        bool bodyValid = true;
        for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
            const auto& inst = bodyBlock->instructions[i];
            if (!isPureVectorizableBodyInstruction(inst)) {
                bodyValid = false;
                break;
            }
            if (inst.opcode == mir::Opcode::StoreLocal) {
                if (inst.localIndex == plan.indexLocalIndex) {
                    bodyValid = false;
                    break;
                }
                for (const auto& col : plan.columns) {
                    if (inst.localIndex == col.colLocalIndex) {
                        bodyValid = false;
                        break;
                    }
                }
                if (!bodyValid) break;
            } else if (inst.opcode == mir::Opcode::LoadLocal &&
                       inst.localIndex == plan.indexLocalIndex) {
                plan.usesIndexInsideBody = true;
            }
        }
        if (!bodyValid) continue;

        const auto* incBlock = findBlock(function, plan.incrementBlockId);
        if (!incBlock ||
            incBlock->terminator.kind != mir::TerminatorKind::Jump ||
            incBlock->terminator.target != plan.headerBlockId ||
            !incBlock->terminator.arguments.empty()) {
            continue;
        }

        std::size_t incPos = 0;
        bool incValid = true;
        while (incPos + 4 <= incBlock->instructions.size() &&
               incBlock->instructions[incPos].opcode == mir::Opcode::LoadLocal &&
               incBlock->instructions[incPos].localIndex < function.localTypes.size() &&
               function.localTypes[incBlock->instructions[incPos].localIndex] ==
                   semantic::PrimitiveType::Array) {
            const auto& i0 = incBlock->instructions[incPos + 0];
            const auto& i1 = incBlock->instructions[incPos + 1];
            const auto& i2 = incBlock->instructions[incPos + 2];
            const auto& i3 = incBlock->instructions[incPos + 3];
            if (i0.result < 0 ||
                i1.opcode != mir::Opcode::LoadLocal ||
                i1.localIndex != plan.indexLocalIndex ||
                i1.result < 0 ||
                i2.opcode != mir::Opcode::LoadLocal ||
                i2.result < 0 ||
                i3.opcode != mir::Opcode::StoreElement ||
                i3.operands.size() != 3 ||
                i3.operands[0] != i0.result ||
                i3.operands[1] != i1.result ||
                i3.operands[2] != i2.result) {
                incValid = false;
                break;
            }
            bool matchedCol = false;
            for (auto& col : plan.columns) {
                if (col.colLocalIndex == i0.localIndex &&
                    col.iterLocalIndex == i2.localIndex) {
                    col.isWrite = true;
                    matchedCol = true;
                    break;
                }
            }
            if (!matchedCol) {
                incValid = false;
                break;
            }
            incPos += 4;
            if (incPos < incBlock->instructions.size() &&
                incBlock->instructions[incPos].opcode == mir::Opcode::Call) {
                const std::string_view sym(incBlock->instructions[incPos].symbolName);
                if (sym.rfind("$intrinsic::ecs_writeback::", 0) == 0) {
                    plan.hasSparseWritebackCall = true;
                    incPos += 1;
                } else {
                    incValid = false;
                    break;
                }
            }
        }
        if (!incValid) continue;

        if (incPos >= incBlock->instructions.size()) continue;
        const auto& idxLoadInc = incBlock->instructions[incPos++];
        if (idxLoadInc.opcode != mir::Opcode::LoadLocal ||
            idxLoadInc.localIndex != plan.indexLocalIndex ||
            idxLoadInc.result < 0) {
            continue;
        }
        if (incPos < incBlock->instructions.size() &&
            incBlock->instructions[incPos].opcode == mir::Opcode::ConstantInt) {
            ++incPos;
        }
        if (incPos + 2 != incBlock->instructions.size()) continue;
        const auto& addInc = incBlock->instructions[incPos];
        const auto& storeInc = incBlock->instructions[incPos + 1];
        if (addInc.opcode != mir::Opcode::AddInt ||
            addInc.operands.size() != 2 ||
            addInc.operands[0] != idxLoadInc.result ||
            addInc.result < 0 ||
            storeInc.opcode != mir::Opcode::StoreLocal ||
            storeInc.localIndex != plan.indexLocalIndex ||
            storeInc.operands.size() != 1 ||
            storeInc.operands[0] != addInc.result) {
            continue;
        }
        const auto oneIt = constantInts.find(addInc.operands[1]);
        if (oneIt == constantInts.end() || oneIt->second != 1) continue;

        plans.emplace(plan.headerBlockId, std::move(plan));
    }
    return plans;
}

void emitVectorizedBodyInstruction(
    Emitter& source,
    const mir::Instruction& instruction,
    const std::vector<TypedSlotType>& types,
    const std::string& indent) {
    (void)types;
    const auto result = instruction.result >= 0
        ? typedValueExpression(instruction.result)
        : std::string{};
    const auto operand = [&](std::size_t index) {
        return typedValueExpression(instruction.operands[index]);
    };
    switch (instruction.opcode) {
    case mir::Opcode::ConstantInt:
        if (instruction.resultType == semantic::PrimitiveType::Enum) {
            source.line(indent + result + " = runtime::EnumValue{0x" +
                hexId(instruction.resultTypeId) + "ULL, " +
                int64Literal(instruction.integerImmediate) + "};");
        } else {
            source.line(indent + result + " = std::int64_t{" +
                int64Literal(instruction.integerImmediate) + "};");
        }
        break;
    case mir::Opcode::ConstantDouble: {
        std::ostringstream literal;
        literal << std::setprecision(17) << instruction.doubleImmediate;
        if (instruction.resultType == semantic::PrimitiveType::Float) {
            source.line(indent + result + " = static_cast<float>(" +
                literal.str() + ");");
        } else {
            source.line(indent + result + " = " + literal.str() + ";");
        }
        break;
    }
    case mir::Opcode::ConstantBool:
        source.line(indent + result + " = " +
            (instruction.boolImmediate ? "true" : "false") + ";");
        break;
    case mir::Opcode::LoadLocal:
        source.line(indent + result + " = " +
            typedLocalExpression(instruction.localIndex) + ";");
        break;
    case mir::Opcode::StoreLocal:
        source.line(indent +
            typedLocalExpression(instruction.localIndex) + " = " +
            operand(0) + ";");
        break;
    case mir::Opcode::ConvertIntToLong:
        source.line(indent + result + " = static_cast<std::int64_t>(" + operand(0) + ");");
        break;
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
        source.line(indent + result + " = static_cast<double>(" + operand(0) + ");");
        break;
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt: {
        const auto operation = instruction.opcode == mir::Opcode::AddInt
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractInt ? " - " : " * ");
        if (!instruction.checkedArithmetic) {
            source.line(indent + result + " = aot::typedWrapInt32(" +
                operand(0) + operation + operand(1) + ");");
        } else {
            source.line(indent + result + " = " + operand(0) + operation + operand(1) + ";");
        }
        break;
    }
    case mir::Opcode::NegateInt:
        if (!instruction.checkedArithmetic) {
            source.line(indent + result + " = aot::typedWrapInt32(-" + operand(0) + ");");
        } else {
            source.line(indent + result + " = -" + operand(0) + ";");
        }
        break;
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong: {
        const auto operation = instruction.opcode == mir::Opcode::AddLong
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractLong ? " - " : " * ");
        source.line(indent + result +
            " = static_cast<std::int64_t>(static_cast<std::uint64_t>(" +
            operand(0) + ")" + operation + "static_cast<std::uint64_t>(" +
            operand(1) + "));");
        break;
    }
    case mir::Opcode::NegateLong:
        source.line(indent + result +
            " = static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(" +
            operand(0) + "));");
        break;
    case mir::Opcode::NegateDouble:
        source.line(indent + result + " = -" + operand(0) + ";");
        break;
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble: {
        const char* operation = instruction.opcode == mir::Opcode::AddDouble
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractDouble
                ? " - "
                : (instruction.opcode == mir::Opcode::MultiplyDouble
                    ? " * "
                    : " / "));
        source.line(indent + result + " = " + operand(0) + operation + operand(1) + ";");
        break;
    }
    case mir::Opcode::LogicalNot:
        source.line(indent + result + " = !" + operand(0) + ";");
        break;
    case mir::Opcode::Equal:
        source.line(indent + result + " = (" + operand(0) + " == " + operand(1) + ");");
        break;
    case mir::Opcode::NotEqual:
        source.line(indent + result + " = !(" + operand(0) + " == " + operand(1) + ");");
        break;
    case mir::Opcode::LessInt:
    case mir::Opcode::LessOrEqualInt:
    case mir::Opcode::GreaterInt:
    case mir::Opcode::GreaterOrEqualInt:
    case mir::Opcode::LessLong:
    case mir::Opcode::LessOrEqualLong:
    case mir::Opcode::GreaterLong:
    case mir::Opcode::GreaterOrEqualLong:
    case mir::Opcode::LessDouble:
    case mir::Opcode::LessOrEqualDouble:
    case mir::Opcode::GreaterDouble:
    case mir::Opcode::GreaterOrEqualDouble: {
        const char* operation = "<";
        if (instruction.opcode == mir::Opcode::LessOrEqualInt ||
            instruction.opcode == mir::Opcode::LessOrEqualLong ||
            instruction.opcode == mir::Opcode::LessOrEqualDouble) {
            operation = "<=";
        } else if (instruction.opcode == mir::Opcode::GreaterInt ||
                   instruction.opcode == mir::Opcode::GreaterLong ||
                   instruction.opcode == mir::Opcode::GreaterDouble) {
            operation = ">";
        } else if (instruction.opcode == mir::Opcode::GreaterOrEqualInt ||
                   instruction.opcode == mir::Opcode::GreaterOrEqualLong ||
                   instruction.opcode == mir::Opcode::GreaterOrEqualDouble) {
            operation = ">=";
        }
        source.line(indent + result + " = " + operand(0) + " " + operation + " " + operand(1) + ";");
        break;
    }
    case mir::Opcode::NewStruct: {
        const auto typeId = instruction.resultTypeId != 0
            ? instruction.resultTypeId
            : instruction.typeId;
        const auto kind = classifyInlineMath(semantic::PrimitiveType::Struct, typeId);
        if (kind != InlineMathKind::None) {
            source.line(indent + result + " = " +
                typedDefaultValue({semantic::PrimitiveType::Struct, typeId}) + ";");
        } else {
            source.line(indent + result + " = " +
                typedDefaultValue({semantic::PrimitiveType::Struct, typeId}) + ";");
            source.line(indent + "(void)context.initUnboxedStruct(0x" +
                hexId(typeId) + "ULL, " + result + ");");
        }
        break;
    }
    case mir::Opcode::LoadStructField: {
        const auto kind = classifyInlineMath(
            semantic::PrimitiveType::Struct, instruction.typeId);
        if (kind != InlineMathKind::None) {
            const char* member = inlineMathFieldMember(kind, instruction.fieldIndex);
            if (kind == InlineMathKind::Fixed || kind == InlineMathKind::Fixed16) {
                source.line(indent + result + " = static_cast<std::int64_t>(" +
                    operand(0) + "." + member + ");");
            } else {
                source.line(indent + result + " = runtime::FixedValue{" +
                    operand(0) + "." + member + "};");
            }
        } else {
            source.line(indent + result + " = " + operand(0) + ".f_" +
                std::to_string(instruction.fieldIndex) + ";");
        }
        break;
    }
    case mir::Opcode::StoreStructField: {
        const auto kind = classifyInlineMath(
            semantic::PrimitiveType::Struct, instruction.typeId);
        if (kind != InlineMathKind::None) {
            const char* member = inlineMathFieldMember(kind, instruction.fieldIndex);
            if (result != operand(0)) {
                source.line(indent + result + " = " + operand(0) + ";");
            }
            if (kind == InlineMathKind::Fixed || kind == InlineMathKind::Fixed16) {
                source.line(indent + result + "." + member +
                    " = static_cast<std::int32_t>(" + operand(1) + ");");
            } else {
                source.line(indent + result + "." + member + " = " + operand(1) + ".raw;");
            }
        } else {
            if (result != operand(0)) {
                source.line(indent + result + " = " + operand(0) + ";");
            }
            source.line(indent + result + ".f_" +
                std::to_string(instruction.fieldIndex) + " = " + operand(1) + ";");
        }
        break;
    }
    default:
        break;
    }
}

void emitVectorizedEcsLoop(
    Emitter& source,
    const mir::Module& module,
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const VectorizedEcsLoopPlan& plan,
    bool emitLineDirectives) {
    const auto* bodyBlock = findBlock(function, plan.bodyBlockId);
    if (!bodyBlock) return;

    const auto idxLocalExpr = typedLocalExpression(plan.indexLocalIndex);
    std::string exactCond;
    std::string rawCond;
    for (std::size_t c = 0; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        if (!exactCond.empty()) exactCond += " && ";
        if (!rawCond.empty()) rawCond += " && ";
        exactCond += rawCol + ".exactNativeLayout";
        rawCond += "(" + rawCol + ".basePtr != nullptr)";
        if (plan.columns[c].isWrite) {
            exactCond += " && !" + rawCol + ".readOnly";
            rawCond += " && !" + rawCol + ".readOnly";
        }
    }
    exactCond += " && (" + idxLocalExpr + " >= 0)";
    rawCond += " && (" + idxLocalExpr + " >= 0)";
    if (plan.hasSparseWritebackCall) {
        exactCond += " && !context.hasSparseEcsState()";
        rawCond += " && !context.hasSparseEcsState()";
    }

    const auto firstRawCol = "typedLocalRawCol_" + std::to_string(plan.columns[0].colLocalIndex);

    source.line("            // Reconstructed Vectorized ECS Query Fast-Loop (__restrict + #pragma GCC ivdep)");
    source.line("            if constexpr (TrustedRelease) {");
    source.line("                if (RS_AOT_LIKELY(" + exactCond + ")) {");
    source.line("                    std::int64_t ecsFastCount = " + firstRawCol + ".count;");
    for (std::size_t c = 1; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        source.line("                    if (" + rawCol + ".count < ecsFastCount) ecsFastCount = " + rawCol + ".count;");
    }
    for (const auto& col : plan.columns) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        if (col.isWrite) {
            source.line("                    auto* __restrict " + ptrName +
                " = reinterpret_cast<" + structName + "*>(" + rawCol + ".basePtr);");
        } else {
            source.line("                    const auto* __restrict " + ptrName +
                " = reinterpret_cast<const " + structName + "*>(" + rawCol + ".basePtr);");
        }
    }
    source.line("#if defined(__GNUC__) && !defined(__clang__)");
    source.line("#pragma GCC ivdep");
    source.line("#elif defined(__clang__)");
    source.line("#pragma clang loop vectorize(enable) interleave(enable)");
    source.line("#elif defined(_MSC_VER) && !defined(__clang__)");
    source.line("#pragma loop(ivdep)");
    source.line("#endif");
    source.line("                    for (std::int64_t ecsIdx = " + idxLocalExpr + "; ecsIdx < ecsFastCount; ++ecsIdx) {");
    if (plan.usesIndexInsideBody) {
        source.line("                        " + idxLocalExpr + " = ecsIdx;");
    }
    for (const auto& col : plan.columns) {
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        source.line("                        " + typedLocalExpression(col.iterLocalIndex) +
            " = " + ptrName + "[ecsIdx];");
    }
    for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
        emitLineDirective(
            source,
            module,
            resolveSequencePoint(
                module,
                function,
                bodyBlock->id,
                static_cast<std::uint32_t>(i),
                false,
                bodyBlock->instructions[i].sourceSpan),
            emitLineDirectives,
            nullptr,
            function.symbolId,
            false);
        emitVectorizedBodyInstruction(source, bodyBlock->instructions[i], types, "                        ");
    }
    emitGeneratedLineReset(source, emitLineDirectives);
    for (const auto& col : plan.columns) {
        if (!col.isWrite) continue;
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        source.line("                        " + ptrName + "[ecsIdx] = " +
            typedLocalExpression(col.iterLocalIndex) + ";");
    }
    source.line("                    }");
    source.line("                    if (ecsFastCount > " + idxLocalExpr + ") " + idxLocalExpr + " = ecsFastCount;");
    source.line("                    goto typedBlock_" + std::to_string(plan.exitBlockId) + ";");
    source.line("                } else if (RS_AOT_LIKELY(" + rawCond + ")) {");
    source.line("                    std::int64_t ecsFastCount = " + firstRawCol + ".count;");
    for (std::size_t c = 1; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        source.line("                    if (" + rawCol + ".count < ecsFastCount) ecsFastCount = " + rawCol + ".count;");
    }
    source.line("                    for (std::int64_t ecsIdx = " + idxLocalExpr + "; ecsIdx < ecsFastCount; ++ecsIdx) {");
    if (plan.usesIndexInsideBody) {
        source.line("                        " + idxLocalExpr + " = ecsIdx;");
    }
    for (const auto& col : plan.columns) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        source.line("                        (void)" + structName + "::loadFromRawColumn(" +
            rawCol + ", ecsIdx, " + typedLocalExpression(col.iterLocalIndex) + ");");
    }
    for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
        emitLineDirective(
            source,
            module,
            resolveSequencePoint(
                module,
                function,
                bodyBlock->id,
                static_cast<std::uint32_t>(i),
                false,
                bodyBlock->instructions[i].sourceSpan),
            emitLineDirectives,
            nullptr,
            function.symbolId,
            false);
        emitVectorizedBodyInstruction(source, bodyBlock->instructions[i], types, "                        ");
    }
    emitGeneratedLineReset(source, emitLineDirectives);
    for (const auto& col : plan.columns) {
        if (!col.isWrite) continue;
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        source.line("                        (void)" + structName + "::storeToRawColumn(" +
            rawCol + ", ecsIdx, " + typedLocalExpression(col.iterLocalIndex) + ");");
    }
    source.line("                    }");
    source.line("                    if (ecsFastCount > " + idxLocalExpr + ") " + idxLocalExpr + " = ecsFastCount;");
    source.line("                    goto typedBlock_" + std::to_string(plan.exitBlockId) + ";");
    source.line("                }");
    source.line("            }");
}

void emitTypedPrimitiveFunction(
    Emitter& source,
    const FunctionView& view,
    const GenerationOptions& options,
    std::vector<SourceMapRecord>& sourceMap,
    const std::unordered_map<semantic::SymbolId, const FunctionView*>& typedFunctions,
    const std::unordered_map<const mir::Instruction*, std::size_t>& callIds) {
    const auto& module = *view.module;
    const auto& function = *view.function;
    const auto types = valueSlotTypes(function);
    const auto rangeAnalysis = analyzeTypedIntegerRanges(function);
    std::string safeArithmeticValues;
    for (const auto value : rangeAnalysis.safeCheckedArithmetic) {
        if (!safeArithmeticValues.empty()) safeArithmeticValues += ",";
        safeArithmeticValues += std::to_string(value);
    }
    source.line("// range-proven checked arithmetic values: " +
        (safeArithmeticValues.empty() ? std::string("none")
                                      : safeArithmeticValues));
    source.line(typedScalarFunctionSignature(view, false));
    source.line("    (void)context;");
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        source.line("    (void)typedArg_" + std::to_string(index) + ";");
    }
    if (function.returnType != semantic::PrimitiveType::Void) {
        source.line("    (void)typedOutResult;");
    }
    source.line("    ExecutionContext::TypedRawAccountingScope<FastAccounting && !TrustedRelease> rawAccounting(context);");
    source.line("    ExecutionContext::TypedDeterminismAccountingScope<DeterminismAccounting && !TrustedRelease> determinismAccounting(context);");
    source.line("    std::conditional_t<!FastAccounting && !DeterminismAccounting && !TrustedRelease, std::vector<runtime::PersistentRoot>, aot::EmptyGcGuards> typedLocalGcGuards;");
    source.line("    (void)typedLocalGcGuards;");
    source.line("    std::uint32_t typedActiveGenLine = 0;");
    source.line("    (void)typedActiveGenLine;");
    source.line("    const auto typedFail = [&](runtime::ErrorCode code, const char* message) {");
    source.line("        if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush();");
    source.line("        context.setActiveGeneratedLine(typedActiveGenLine);");
    source.line("        return context.fail(code, message, typedActiveGenLine);");
    source.line("    };");
    source.line("    (void)typedFail;");
    std::vector<std::size_t> localStoreCounts(function.localTypes.size(), 0);
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.opcode == mir::Opcode::StoreLocal &&
                inst.localIndex < localStoreCounts.size()) {
                ++localStoreCounts[inst.localIndex];
            }
        }
    }
    std::unordered_map<mir::ValueId, std::size_t> valueToArrayLocal;
    std::unordered_map<std::size_t, semantic::SymbolId> arrayLocalStructElemType;
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.opcode == mir::Opcode::LoadLocal &&
                inst.result >= 0 &&
                inst.localIndex < function.localTypes.size() &&
                function.localTypes[inst.localIndex] == semantic::PrimitiveType::Array &&
                localStoreCounts[inst.localIndex] == 1) {
                valueToArrayLocal[inst.result] = inst.localIndex;
            }
        }
    }
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if ((inst.opcode == mir::Opcode::LoadElement ||
                 inst.opcode == mir::Opcode::StoreElement) &&
                !inst.operands.empty() &&
                inst.elementType == semantic::PrimitiveType::Struct &&
                classifyInlineMath(inst.elementType, inst.elementTypeId) ==
                    InlineMathKind::None) {
                const auto it = valueToArrayLocal.find(inst.operands[0]);
                if (it != valueToArrayLocal.end()) {
                    arrayLocalStructElemType[it->second] = inst.elementTypeId;
                }
            }
        }
    }
    for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
        const auto typeId = index < function.localTypeIds.size()
            ? function.localTypeIds[index]
            : 0;
        const TypedSlotType localSlot{function.localTypes[index], typeId};
        source.line("    " + typedCppType(localSlot) + " " +
            typedLocalExpression(index) + " = " +
            typedDefaultValue(localSlot) + ";");
        if (function.localTypes[index] == semantic::PrimitiveType::Array) {
            source.line("    aot::TypedRawColumnCache typedLocalRawCol_" +
                std::to_string(index) + "{};");
            source.line("    (void)typedLocalRawCol_" + std::to_string(index) + ";");
        }
    }
    for (std::size_t index = 0; index < types.size(); ++index) {
        source.line("    " + typedCppType(types[index]) + " " +
            typedValueExpression(static_cast<mir::ValueId>(index)) + "{};");
    }
    if (options.emitLineDirectives && !function.debugInfo.locals.empty()) {
        std::unordered_map<std::string, std::size_t> debugNameCounts;
        for (const auto& local : function.debugInfo.locals) {
            if (!local.name.empty() && local.name.front() != '$') ++debugNameCounts[sanitizeCppIdentifier(local.name)];
        }
        std::set<std::string> emittedDebugAliases;
        for (const auto& local : function.debugInfo.locals) {
            if (local.name.empty() || local.name.front() == '$') continue;
            const std::string clean = sanitizeCppIdentifier(local.name);
            std::string aliasName = "dbg_" + clean;
            if (debugNameCounts[clean] > 1) {
                aliasName += (local.parameter ? "_arg" : "_slot") + std::to_string(local.slot);
            }
            if (!emittedDebugAliases.insert(aliasName).second) continue;
            if (local.parameter &&
                local.slot < localStoreCounts.size() &&
                localStoreCounts[local.slot] > 1 &&
                local.slot < function.localTypes.size()) {
                source.line("    [[maybe_unused]] auto& " + aliasName +
                    " = " + typedLocalExpression(local.slot) + ";");
            } else if (local.parameter && local.slot < function.parameterTypes.size()) {
                source.line("    [[maybe_unused]] auto& " + aliasName +
                    " = typedArg_" + std::to_string(local.slot) + ";");
            } else if (!local.parameter && local.slot < function.localTypes.size()) {
                source.line("    [[maybe_unused]] auto& " + aliasName +
                    " = " + typedLocalExpression(local.slot) + ";");
            }
        }
    }
    source.line("    goto typedBlock_0;");

    const auto vectorizedEcsLoops =
        analyzeVectorizedEcsLoops(function, arrayLocalStructElemType);
    std::size_t typedCallSiteCounter = 0;
    for (const auto& block : function.blocks) {
        source.line("    typedBlock_" + std::to_string(block.id) + ":");
        source.line("    {");
        const auto vecLoopIt = vectorizedEcsLoops.find(block.id);
        if (vecLoopIt != vectorizedEcsLoops.end()) {
            emitVectorizedEcsLoop(
                source, module, function, types, vecLoopIt->second, options.emitLineDirectives);
        }
        const auto rawConsumeBatches =
            typedRawConsumeBatches(block, rangeAnalysis);
        bool activeSourceLine = false;
        for (std::size_t instructionIndex = 0;
             instructionIndex < block.instructions.size();
             ++instructionIndex) {
            const auto& instruction = block.instructions[instructionIndex];
            if (activeSourceLine) {
                emitGeneratedLineReset(source, options.emitLineDirectives);
                activeSourceLine = false;
            }
            emitTypedRawConsume(
                source, rawConsumeBatches[instructionIndex], "            ");
            emitTypedConsume(source, instruction.opcode, "            ");
            bool explicitPoint = false;
            const auto resolvedPoint = resolveSequencePoint(
                module,
                function,
                block.id,
                static_cast<std::uint32_t>(instructionIndex),
                false,
                instruction.sourceSpan,
                &explicitPoint);
            const std::uint32_t currentInstGenLine = source.nextLine() + (options.emitLineDirectives && resolvedPoint ? 1u : 0u);
            if (emitLineDirective(
                    source,
                    module,
                    resolvedPoint,
                    options.emitLineDirectives,
                    &sourceMap,
                    function.symbolId,
                    true)) {
                activeSourceLine = true;
            }
            if (resolvedPoint) {
                source.line("            typedActiveGenLine = " + std::to_string(currentInstGenLine) + "u;");
            }
            const auto result = instruction.result >= 0
                ? typedValueExpression(instruction.result)
                : std::string{};
            const auto operand = [&](std::size_t index) {
                return typedValueExpression(instruction.operands[index]);
            };
            const auto operandSlot = [&](std::size_t index) -> TypedSlotType {
                if (index >= instruction.operands.size() ||
                    instruction.operands[index] < 0) return {};
                return types[static_cast<std::size_t>(instruction.operands[index])];
            };
            switch (instruction.opcode) {
            case mir::Opcode::Parameter: {
                const auto index = static_cast<std::size_t>(
                    instruction.integerImmediate);
                source.line("            " + result + " = typedArg_" +
                    std::to_string(index) + ";");
                break;
            }
            case mir::Opcode::ConstantInt:
                if (instruction.resultType == semantic::PrimitiveType::Enum) {
                    source.line("            " + result + " = runtime::EnumValue{0x" +
                        hexId(instruction.resultTypeId) + "ULL, " +
                        int64Literal(instruction.integerImmediate) + "};");
                } else {
                    source.line("            " + result + " = std::int64_t{" +
                        int64Literal(instruction.integerImmediate) + "};");
                }
                break;
            case mir::Opcode::ConstantNull:
                source.line("            " + result + " = runtime::Value{std::monostate{}};");
                break;
            case mir::Opcode::ConvertNullToObject:
                source.line("            " + result + " = runtime::Value{runtime::NullObject{}};");
                break;
            case mir::Opcode::ConvertNullToArray:
                source.line("            " + result + " = runtime::Value{runtime::NullArray{}};");
                break;
            case mir::Opcode::CheckNotNull:
                source.line("            if (!context.checkNotNull(0x" +
                    hexId(instruction.typeId) + "ULL, " + operand(0) + ", " +
                    result + ")) { if constexpr (DeterminismAccounting) determinismAccounting.flush(); return false; }");
                break;
            case mir::Opcode::LoadField: {
                const auto tmpField = "typedField_" + std::to_string(instructionIndex);
                source.line("            runtime::Value " + tmpField + ";");
                source.line("            if (!context.loadField(0x" +
                    hexId(instruction.typeId) + "ULL, " +
                    std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                    ", " + tmpField + ") || !aot::typedUnboxScalar(" +
                    primitiveExpression(instruction.resultType) + ", " + tmpField +
                    ", " + result + ")) { if constexpr (DeterminismAccounting) determinismAccounting.flush(); return false; }");
                break;
            }
            case mir::Opcode::StoreField: {
                const auto valSlot = operandSlot(1);
                source.line("            if (!context.storeField(0x" +
                    hexId(instruction.typeId) + "ULL, " +
                    std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                    ", aot::typedBoxScalar(" + primitiveExpression(valSlot.primitive) +
                    ", " + operand(1) + "))) { if constexpr (DeterminismAccounting) determinismAccounting.flush(); return false; }");
                break;
            }
            case mir::Opcode::NewArray: {
                source.line("            if (!context.newArray(0x" +
                    hexId(instruction.resultTypeId) + "ULL, " +
                    primitiveExpression(instruction.elementType) + ", 0x" +
                    hexId(instruction.elementTypeId) + "ULL, runtime::Value{" +
                    operand(0) + "}, " + result +
                    ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                source.line("            if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
                source.line("                if (context.heap() != nullptr && std::holds_alternative<runtime::ObjectRef>(" + result + ")) {");
                source.line("                    typedLocalGcGuards.push_back(context.heap()->retain(" + result + "));");
                source.line("                }");
                source.line("            }");
                break;
            }
            case mir::Opcode::ArrayLength: {
                const auto arrLocalIt = valueToArrayLocal.find(instruction.operands[0]);
                if (arrLocalIt != valueToArrayLocal.end()) {
                    const auto rawColExpr = "typedLocalRawCol_" + std::to_string(arrLocalIt->second);
                    source.line("            if (RS_AOT_LIKELY(" + rawColExpr + ".count >= 0)) {");
                    source.line("                " + result + " = " + rawColExpr + ".count;");
                    source.line("            } else if (!context.arrayLengthFast(" + operand(0) +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                } else {
                    source.line("            if (!context.arrayLengthFast(" + operand(0) +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                }
                break;
            }
            case mir::Opcode::LoadElement: {
                if (instruction.elementType == semantic::PrimitiveType::Struct &&
                    classifyInlineMath(instruction.elementType, instruction.elementTypeId) ==
                        InlineMathKind::None) {
                    const auto arrLocalIt = valueToArrayLocal.find(instruction.operands[0]);
                    const std::string structName = "AotStruct_" + hexId(instruction.elementTypeId);
                    const auto tmpElem = "typedElem_" + std::to_string(instructionIndex);
                    if (arrLocalIt != valueToArrayLocal.end()) {
                        const auto rawColExpr = "typedLocalRawCol_" + std::to_string(arrLocalIt->second);
                        source.line("            if (RS_AOT_LIKELY(" + rawColExpr + ".basePtr != nullptr)) {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(" + operand(1) + ") >= static_cast<std::uint64_t>(" + rawColExpr + ".count))) {");
                        source.line("                        return typedFail(runtime::ErrorCode::IndexOutOfRange, \"array element index is out of range\");");
                        source.line("                    }");
                        source.line("                }");
                        source.line("                (void)" + structName + "::loadFromRawColumn(" + rawColExpr + ", " + operand(1) + ", " + result + ");");
                        source.line("            } else {");
                        source.line("                runtime::Value " + tmpElem + ";");
                        source.line("                if (!context.loadElement(semantic::PrimitiveType::Struct, 0x" +
                            hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                            ", runtime::Value{" + operand(1) + "}, " + tmpElem +
                            ") || !aot::typedUnboxScalar(semantic::PrimitiveType::Struct, " + tmpElem +
                            ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                        source.line("            }");
                    } else {
                        source.line("            runtime::Value " + tmpElem + ";");
                        source.line("            if (!context.loadElement(semantic::PrimitiveType::Struct, 0x" +
                            hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                            ", runtime::Value{" + operand(1) + "}, " + tmpElem +
                            ") || !aot::typedUnboxScalar(semantic::PrimitiveType::Struct, " + tmpElem +
                            ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    }
                } else {
                    const auto tmpElem = "typedElem_" + std::to_string(instructionIndex);
                    source.line("            runtime::Value " + tmpElem + ";");
                    source.line("            if (!context.loadElement(" +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                        ", runtime::Value{" + operand(1) + "}, " + tmpElem +
                        ") || !aot::typedUnboxScalar(" +
                        primitiveExpression(instruction.resultType) + ", " + tmpElem +
                        ", " + result + ")) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                }
                break;
            }
            case mir::Opcode::StoreElement: {
                const auto valSlot = operandSlot(2);
                if (instruction.elementType == semantic::PrimitiveType::Struct &&
                    classifyInlineMath(instruction.elementType, instruction.elementTypeId) ==
                        InlineMathKind::None) {
                    const auto arrLocalIt = valueToArrayLocal.find(instruction.operands[0]);
                    const std::string structName = "AotStruct_" + hexId(instruction.elementTypeId);
                    if (arrLocalIt != valueToArrayLocal.end()) {
                        const auto rawColExpr = "typedLocalRawCol_" + std::to_string(arrLocalIt->second);
                        source.line("            if (RS_AOT_LIKELY(" + rawColExpr + ".basePtr != nullptr)) {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    if (RS_AOT_UNLIKELY(" + rawColExpr + ".readOnly || static_cast<std::uint64_t>(" + operand(1) + ") >= static_cast<std::uint64_t>(" + rawColExpr + ".count))) {");
                        source.line("                        return typedFail(" + rawColExpr + ".readOnly ? runtime::ErrorCode::CapabilityDenied : runtime::ErrorCode::IndexOutOfRange, \"raw ECS column store failed\");");
                        source.line("                    }");
                        source.line("                }");
                        source.line("                (void)" + structName + "::storeToRawColumn(" + rawColExpr + ", " + operand(1) + ", " + operand(2) + ");");
                        source.line("            } else {");
                        source.line("                if (!context.storeElement(semantic::PrimitiveType::Struct, 0x" +
                            hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                            ", runtime::Value{" + operand(1) + "}, " + operand(2) +
                            ".boxToValue())) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                        source.line("            }");
                    } else {
                        source.line("            if (!context.storeElement(semantic::PrimitiveType::Struct, 0x" +
                            hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                            ", runtime::Value{" + operand(1) + "}, " + operand(2) +
                            ".boxToValue())) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                    }
                } else {
                    source.line("            if (!context.storeElement(" +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                        ", runtime::Value{" + operand(1) + "}, aot::typedBoxScalar(" +
                        primitiveExpression(valSlot.primitive) + ", " + operand(2) +
                        "))) { if constexpr (DeterminismAccounting && !TrustedRelease) determinismAccounting.flush(); return false; }");
                }
                break;
            }
            case mir::Opcode::ConstantDouble: {
                std::ostringstream literal;
                literal << std::setprecision(17) << instruction.doubleImmediate;
                if (instruction.resultType == semantic::PrimitiveType::Float) {
                    source.line("            " + result + " = static_cast<float>(" +
                        literal.str() + ");");
                } else {
                    source.line("            " + result + " = " + literal.str() + ";");
                }
                break;
            }
            case mir::Opcode::ConstantBool:
                source.line("            " + result + " = " +
                    (instruction.boolImmediate ? "true" : "false") + ";");
                break;
            case mir::Opcode::LoadLocal:
                source.line("            " + result + " = " +
                    typedLocalExpression(instruction.localIndex) + ";");
                break;
            case mir::Opcode::StoreLocal:
                source.line("            " +
                    typedLocalExpression(instruction.localIndex) + " = " +
                    operand(0) + ";");
                if (instruction.localIndex < function.localTypes.size() &&
                    function.localTypes[instruction.localIndex] == semantic::PrimitiveType::Array) {
                    const auto elemTypeIt = arrayLocalStructElemType.find(instruction.localIndex);
                    if (elemTypeIt != arrayLocalStructElemType.end()) {
                        const std::string structName = "AotStruct_" + hexId(elemTypeIt->second);
                        source.line("            typedLocalRawCol_" +
                            std::to_string(instruction.localIndex) +
                            " = context.acquireRawColumnCache(" +
                            typedLocalExpression(instruction.localIndex) +
                            ", 0x" + hexId(elemTypeIt->second) + "ULL, sizeof(" +
                            structName + "), " + structName + "_layout, " +
                            structName + "::kFieldCount);");
                    } else {
                        source.line("            typedLocalRawCol_" +
                            std::to_string(instruction.localIndex) +
                            " = context.acquireRawColumnCache(" +
                            typedLocalExpression(instruction.localIndex) + ");");
                    }
                }
                break;
            case mir::Opcode::ConvertIntToLong:
                source.line("            " + result +
                    " = static_cast<std::int64_t>(" + operand(0) + ");");
                break;
            case mir::Opcode::ConvertIntToDouble:
            case mir::Opcode::ConvertLongToDouble:
                source.line("            " + result +
                    " = static_cast<double>(" + operand(0) + ");");
                break;
            case mir::Opcode::ConvertNumeric: {
                const auto srcSlot = operandSlot(0);
                source.line("            if (!aot::typedConvertNumeric<" +
                    primitiveExpression(srcSlot.primitive) + ", " +
                    primitiveExpression(instruction.resultType) + ", " +
                    (instruction.checkedArithmetic ? "true" : "false") + ">(" +
                    operand(0) + ", " + result + ")) return typedFail("
                    "runtime::ErrorCode::IntegerOverflow, "
                    "\"numeric conversion overflow\");");
                break;
            }
            case mir::Opcode::AddInt:
            case mir::Opcode::SubtractInt:
            case mir::Opcode::MultiplyInt: {
                const auto operation = instruction.opcode == mir::Opcode::AddInt
                    ? "+"
                    : instruction.opcode == mir::Opcode::SubtractInt ? "-" : "*";
                const auto temporary = "typedArithmetic_" +
                    std::to_string(instruction.result);
                source.line("            const auto " + temporary + " = " +
                    operand(0) + " " + operation + " " + operand(1) + ";");
                if (!instruction.checkedArithmetic) {
                    source.line("            " + result +
                        " = aot::typedWrapInt32(" + temporary + ");");
                } else {
                    if (typedCheckedArithmeticProvenSafe(
                            rangeAnalysis, instruction)) {
                        source.line("            // checked arithmetic proven safe by integer range analysis");
                    } else {
                        source.line("            if constexpr (!TrustedRelease) {");
                        source.line("                if (" + temporary +
                            " < std::numeric_limits<std::int32_t>::min() || " +
                            temporary +
                            " > std::numeric_limits<std::int32_t>::max()) return "
                            "typedFail(runtime::ErrorCode::IntegerOverflow, "
                            "\"integer arithmetic overflow\");");
                        source.line("            }");
                    }
                    source.line("            " + result + " = " + temporary + ";");
                }
                break;
            }
            case mir::Opcode::DivideInt:
            case mir::Opcode::RemainderInt: {
                source.line("            if (" + operand(1) +
                    " == 0) return typedFail(runtime::ErrorCode::DivisionByZero, \"" +
                    (instruction.opcode == mir::Opcode::DivideInt
                        ? "division by zero"
                        : "remainder by zero") + "\");");
                if (instruction.opcode == mir::Opcode::DivideInt) {
                    source.line("            if (" + operand(0) +
                        " == std::numeric_limits<std::int32_t>::min() && " +
                        operand(1) +
                        " == -1) return typedFail(runtime::ErrorCode::IntegerOverflow, "
                        "\"integer division overflow\");");
                    source.line("            " + result + " = " + operand(0) +
                        " / " + operand(1) + ";");
                } else {
                    source.line("            " + result + " = (" + operand(0) +
                        " == std::numeric_limits<std::int32_t>::min() && " +
                        operand(1) + " == -1) ? std::int64_t{0} : (" +
                        operand(0) + " % " + operand(1) + ");");
                }
                break;
            }
            case mir::Opcode::NegateInt:
                if (!instruction.checkedArithmetic) {
                    source.line("            " + result + " = (" + operand(0) +
                        " == std::numeric_limits<std::int32_t>::min()) ? " +
                        operand(0) + " : -" + operand(0) + ";");
                } else {
                    if (typedCheckedArithmeticProvenSafe(
                            rangeAnalysis, instruction)) {
                        source.line("            // checked negation proven safe by integer range analysis");
                    } else {
                        source.line("            if (" + operand(0) +
                            " == std::numeric_limits<std::int32_t>::min()) return "
                            "typedFail(runtime::ErrorCode::IntegerOverflow, "
                            "\"integer negation overflow\");");
                    }
                    source.line("            " + result + " = -" + operand(0) + ";");
                }
                break;
            case mir::Opcode::AddLong:
            case mir::Opcode::SubtractLong:
            case mir::Opcode::MultiplyLong: {
                const char* helper = instruction.opcode == mir::Opcode::AddLong
                    ? "aot::typedAddLong"
                    : (instruction.opcode == mir::Opcode::SubtractLong
                        ? "aot::typedSubLong"
                        : "aot::typedMulLong");
                source.line("            if (!" + std::string(helper) + "<" +
                    (instruction.checkedArithmetic ? "true" : "false") + ">(" +
                    operand(0) + ", " + operand(1) + ", " + result +
                    ")) return typedFail(runtime::ErrorCode::IntegerOverflow, "
                    "\"long arithmetic overflow\");");
                break;
            }
            case mir::Opcode::DivideLong:
            case mir::Opcode::RemainderLong: {
                source.line("            if (" + operand(1) +
                    " == 0) return typedFail(runtime::ErrorCode::DivisionByZero, \"" +
                    (instruction.opcode == mir::Opcode::DivideLong
                        ? "division by zero"
                        : "remainder by zero") + "\");");
                if (instruction.opcode == mir::Opcode::DivideLong) {
                    source.line("            if (" + operand(0) +
                        " == std::numeric_limits<std::int64_t>::min() && " +
                        operand(1) +
                        " == -1) return typedFail(runtime::ErrorCode::IntegerOverflow, "
                        "\"long division overflow\");");
                    source.line("            " + result + " = " + operand(0) +
                        " / " + operand(1) + ";");
                } else {
                    source.line("            " + result + " = (" + operand(0) +
                        " == std::numeric_limits<std::int64_t>::min() && " +
                        operand(1) + " == -1) ? std::int64_t{0} : (" +
                        operand(0) + " % " + operand(1) + ");");
                }
                break;
            }
            case mir::Opcode::NegateLong:
                if (!instruction.checkedArithmetic) {
                    source.line("            " + result + " = (" + operand(0) +
                        " == std::numeric_limits<std::int64_t>::min()) ? " +
                        operand(0) + " : -" + operand(0) + ";");
                } else {
                    source.line("            if (" + operand(0) +
                        " == std::numeric_limits<std::int64_t>::min()) return "
                        "typedFail(runtime::ErrorCode::IntegerOverflow, "
                        "\"long negation overflow\");");
                    source.line("            " + result + " = -" + operand(0) + ";");
                }
                break;
            case mir::Opcode::AddDouble:
            case mir::Opcode::SubtractDouble:
            case mir::Opcode::MultiplyDouble:
            case mir::Opcode::DivideDouble: {
                const char* operation = instruction.opcode == mir::Opcode::AddDouble
                    ? " + "
                    : (instruction.opcode == mir::Opcode::SubtractDouble
                        ? " - "
                        : (instruction.opcode == mir::Opcode::MultiplyDouble
                            ? " * "
                            : " / "));
                source.line("            " + result + " = " + operand(0) +
                    operation + operand(1) + ";");
                break;
            }
            case mir::Opcode::NegateDouble:
                source.line("            " + result + " = -" + operand(0) + ";");
                break;
            case mir::Opcode::LogicalNot:
                source.line("            " + result + " = !" + operand(0) + ";");
                break;
            case mir::Opcode::Equal:
                source.line("            " + result + " = (" + operand(0) +
                    " == " + operand(1) + ");");
                break;
            case mir::Opcode::NotEqual:
                source.line("            " + result + " = !(" + operand(0) +
                    " == " + operand(1) + ");");
                break;
            case mir::Opcode::LessInt:
            case mir::Opcode::LessOrEqualInt:
            case mir::Opcode::GreaterInt:
            case mir::Opcode::GreaterOrEqualInt:
            case mir::Opcode::LessLong:
            case mir::Opcode::LessOrEqualLong:
            case mir::Opcode::GreaterLong:
            case mir::Opcode::GreaterOrEqualLong:
            case mir::Opcode::LessDouble:
            case mir::Opcode::LessOrEqualDouble:
            case mir::Opcode::GreaterDouble:
            case mir::Opcode::GreaterOrEqualDouble: {
                const char* operation = "<";
                if (instruction.opcode == mir::Opcode::LessOrEqualInt ||
                    instruction.opcode == mir::Opcode::LessOrEqualLong ||
                    instruction.opcode == mir::Opcode::LessOrEqualDouble) {
                    operation = "<=";
                } else if (instruction.opcode == mir::Opcode::GreaterInt ||
                           instruction.opcode == mir::Opcode::GreaterLong ||
                           instruction.opcode == mir::Opcode::GreaterDouble) {
                    operation = ">";
                } else if (instruction.opcode == mir::Opcode::GreaterOrEqualInt ||
                           instruction.opcode == mir::Opcode::GreaterOrEqualLong ||
                           instruction.opcode == mir::Opcode::GreaterOrEqualDouble) {
                    operation = ">=";
                }
                source.line("            " + result + " = " + operand(0) +
                    " " + operation + " " + operand(1) + ";");
                break;
            }
            case mir::Opcode::NewStruct: {
                const auto typeId = instruction.resultTypeId != 0
                    ? instruction.resultTypeId
                    : instruction.typeId;
                const auto kind = classifyInlineMath(
                    semantic::PrimitiveType::Struct, typeId);
                if (kind != InlineMathKind::None) {
                    source.line("            " + result + " = " +
                        typedDefaultValue({semantic::PrimitiveType::Struct, typeId}) + ";");
                } else {
                    source.line("            if (!context.initUnboxedStruct(0x" +
                        hexId(typeId) + "ULL, " + result + ")) return false;");
                }
                break;
            }
            case mir::Opcode::LoadStructField: {
                const auto kind = classifyInlineMath(
                    semantic::PrimitiveType::Struct, instruction.typeId);
                if (kind != InlineMathKind::None) {
                    const char* member = inlineMathFieldMember(
                        kind, instruction.fieldIndex);
                    if (kind == InlineMathKind::Fixed ||
                        kind == InlineMathKind::Fixed16) {
                        source.line("            " + result +
                            " = static_cast<std::int64_t>(" + operand(0) + "." +
                            member + ");");
                    } else {
                        source.line("            " + result +
                            " = runtime::FixedValue{" + operand(0) + "." +
                            member + "};");
                    }
                } else {
                    source.line("            " + result + " = " + operand(0) +
                        ".f_" + std::to_string(instruction.fieldIndex) + ";");
                }
                break;
            }
            case mir::Opcode::StoreStructField: {
                const auto kind = classifyInlineMath(
                    semantic::PrimitiveType::Struct, instruction.typeId);
                if (kind != InlineMathKind::None) {
                    const char* member = inlineMathFieldMember(
                        kind, instruction.fieldIndex);
                    source.line("            " + result + " = " + operand(0) + ";");
                    if (kind == InlineMathKind::Fixed ||
                        kind == InlineMathKind::Fixed16) {
                        source.line("            " + result + "." + member +
                            " = static_cast<std::int32_t>(" + operand(1) + ");");
                    } else {
                        source.line("            " + result + "." + member +
                            " = " + operand(1) + ".raw;");
                    }
                } else {
                    if (result != operand(0)) {
                        source.line("            " + result + " = " + operand(0) + ";");
                    }
                    source.line("            " + result + ".f_" +
                        std::to_string(instruction.fieldIndex) + " = " + operand(1) + ";");
                }
                break;
            }
            case mir::Opcode::Call: {
                const auto callIdx = typedCallSiteCounter++;
                const auto suffix = std::to_string(callIdx);
                source.line("            if constexpr (!TrustedRelease) {");
                source.line("                rawAccounting.suspendBeforeCall();");
                source.line("                determinismAccounting.suspendBeforeCall();");
                source.line("            }");
                const auto targetIt = typedFunctions.find(instruction.symbolId);
                if (targetIt != typedFunctions.end()) {
                    const auto* calleeView = targetIt->second;
                    source.line("            semantic::SymbolId prevFn_" + suffix + " = 0;");
                    source.line("            (void)prevFn_" + suffix + ";");
                    source.line("            if (!context.enterTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(0x" +
                        hexId(calleeView->function->symbolId) + "ULL, \"" +
                        escapeCppString(calleeView->qualifiedName) + "\", prevFn_" + suffix + ", typedActiveGenLine)) {");
                    source.line("                if constexpr (!TrustedRelease) {");
                    source.line("                    rawAccounting.resumeAfterCall();");
                    source.line("                    determinismAccounting.resumeAfterCall();");
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                    std::string callArgs = "context";
                    for (std::size_t argIndex = 0; argIndex < instruction.operands.size(); ++argIndex) {
                        callArgs += ", " + operand(argIndex);
                    }
                    if (instruction.resultType != semantic::PrimitiveType::Void) {
                        const TypedSlotType retSlot{
                            instruction.resultType,
                            instruction.resultTypeId,
                        };
                        source.line("            " + typedCppType(retSlot) +
                            " directCallRes_" + suffix + "{};");
                        callArgs += ", directCallRes_" + suffix;
                    }
                    source.line("            const bool directCallOk_" + suffix +
                        " = " + calleeView->cppName +
                        "_scalar<FastAccounting, DeterminismAccounting, TrustedRelease>(" +
                        callArgs + ");");
                    if (instruction.resultType != semantic::PrimitiveType::Void) {
                        source.line("            context.exitTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(prevFn_" +
                            suffix + ", directCallOk_" + suffix +
                            ", [&]() { return aot::typedBoxScalar(" +
                            primitiveExpression(instruction.resultType) +
                            ", directCallRes_" + suffix + "); });");
                    } else {
                        source.line("            context.exitTypedDirectCall<FastAccounting, DeterminismAccounting, TrustedRelease>(prevFn_" +
                            suffix + ", directCallOk_" + suffix +
                            ", []() { return runtime::Value{std::monostate{}}; });");
                    }
                    source.line("            if constexpr (!TrustedRelease) {");
                    source.line("                rawAccounting.resumeAfterCall();");
                    source.line("                determinismAccounting.resumeAfterCall();");
                    source.line("            }");
                    source.line("            if (!directCallOk_" + suffix + ") return false;");
                    if (instruction.result >= 0 &&
                        instruction.resultType != semantic::PrimitiveType::Void) {
                        source.line("            " + result + " = directCallRes_" + suffix + ";");
                    }
                } else {
                    const auto callId = callIds.at(&instruction);
                    const std::string_view symName(instruction.symbolName);
                    if (symName.rfind("$intrinsic::ecs_writeback::", 0) == 0 &&
                        instruction.operands.size() == 2 &&
                        operandSlot(1).primitive == semantic::PrimitiveType::Struct &&
                        classifyInlineMath(operandSlot(1).primitive, operandSlot(1).typeId) ==
                            InlineMathKind::None) {
                        source.line("            if (RS_AOT_UNLIKELY(context.hasSparseEcsState())) {");
                        source.line("                const bool wbOk_" + suffix +
                            " = context.ecsWritebackBoxed(call_" +
                            std::to_string(callId) + "_signature, " +
                            operand(0) + ", " + operand(1) + ".boxToValue());");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    rawAccounting.resumeAfterCall();");
                        source.line("                    determinismAccounting.resumeAfterCall();");
                        source.line("                }");
                        source.line("                if (!wbOk_" + suffix + ") return false;");
                        source.line("            } else {");
                        source.line("                if constexpr (!TrustedRelease) {");
                        source.line("                    rawAccounting.resumeAfterCall();");
                        source.line("                    determinismAccounting.resumeAfterCall();");
                        source.line("                }");
                        source.line("            }");
                    } else if (symName.rfind("$intrinsic::static_get::ecs_col::", 0) == 0 &&
                               instruction.operands.empty() &&
                               instruction.result >= 0) {
                        semantic::SymbolId denseColSymbolId = 0;
                        const auto rest = symName.substr(std::string_view("$intrinsic::static_get::ecs_col::").size());
                        const auto sep = rest.find("::");
                        if (sep != std::string_view::npos) {
                            const auto colIdx = static_cast<std::size_t>(
                                std::strtoull(std::string(rest.substr(0, sep)).c_str(), nullptr, 10));
                            const auto spec = rest.substr(sep + 2);
                            const auto firstSemi = spec.find(';');
                            const auto restAfterCols = firstSemi == std::string_view::npos
                                ? std::string_view{}
                                : spec.substr(firstSemi);
                            if (restAfterCols == ";;" || restAfterCols.empty()) {
                                const auto colsPart = firstSemi == std::string_view::npos
                                    ? spec
                                    : spec.substr(0, firstSemi);
                                std::size_t start = 0;
                                std::size_t cur = 0;
                                while (start <= colsPart.size()) {
                                    const auto comma = colsPart.find_first_of(",|", start);
                                    const auto tok = comma == std::string_view::npos
                                        ? colsPart.substr(start)
                                        : colsPart.substr(start, comma - start);
                                    if (cur == colIdx && !tok.empty()) {
                                        denseColSymbolId = semantic::stableTypeId("$ecs_column::" + std::string(tok));
                                        break;
                                    }
                                    if (comma == std::string_view::npos) break;
                                    start = comma + 1;
                                    ++cur;
                                }
                            }
                        }
                        source.line("            const bool colOk_" + suffix +
                            " = context.resolveEcsColumnFast(0x" +
                            hexId(denseColSymbolId) + "ULL, call_" +
                            std::to_string(callId) + "_signature, " + result + ");");
                        source.line("            if constexpr (!TrustedRelease) {");
                        source.line("                rawAccounting.resumeAfterCall();");
                        source.line("                determinismAccounting.resumeAfterCall();");
                        source.line("            }");
                        source.line("            if (!colOk_" + suffix + ") return false;");
                        if (!options.trustedRelease) {
                            source.line("            if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
                            source.line("                if (context.heap() != nullptr && std::holds_alternative<runtime::ObjectRef>(" + result + ")) {");
                            source.line("                    typedLocalGcGuards.push_back(context.heap()->retain(" + result + "));");
                            source.line("                }");
                            source.line("            }");
                        }
                    } else {
                        std::string argPtr = "nullptr";
                        if (!instruction.operands.empty()) {
                            source.line("            std::array<runtime::Value, " +
                                std::to_string(instruction.operands.size()) +
                                "> bridgeArgs_" + suffix + "{{");
                            for (std::size_t argIndex = 0; argIndex < instruction.operands.size(); ++argIndex) {
                                const auto argSlot = operandSlot(argIndex);
                                source.line("                aot::typedBoxScalar(" +
                                    primitiveExpression(argSlot.primitive) + ", " +
                                    operand(argIndex) + "),");
                            }
                            source.line("            }};");
                            argPtr = "bridgeArgs_" + suffix + ".data()";
                        }
                        source.line("            context.setActiveGeneratedLine(typedActiveGenLine);");
                        source.line("            runtime::Value bridgeRes_" + suffix + ";");
                        source.line("            const bool bridgeOk_" + suffix +
                            " = context.call(call_" + std::to_string(callId) +
                            "_signature, " + argPtr + ", " +
                            std::to_string(instruction.operands.size()) +
                            "u, bridgeRes_" + suffix + ");");
                        source.line("            if constexpr (!TrustedRelease) {");
                        source.line("                rawAccounting.resumeAfterCall();");
                        source.line("                determinismAccounting.resumeAfterCall();");
                        source.line("            }");
                        source.line("            if (!bridgeOk_" + suffix + ") return false;");
                        if (instruction.result >= 0 &&
                            instruction.resultType != semantic::PrimitiveType::Void) {
                            source.line("            if (!aot::typedUnboxScalar(" +
                                primitiveExpression(instruction.resultType) +
                                ", bridgeRes_" + suffix + ", " + result +
                                ")) return typedFail(runtime::ErrorCode::TypeMismatch, "
                                "\"AOT call return type mismatch\");");
                            if (instruction.resultType == semantic::PrimitiveType::Array ||
                                instruction.resultType == semantic::PrimitiveType::Object) {
                                source.line("            if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
                                source.line("                if (context.heap() != nullptr && std::holds_alternative<runtime::ObjectRef>(" + result + ")) {");
                                source.line("                    typedLocalGcGuards.push_back(context.heap()->retain(" + result + "));");
                                source.line("                }");
                                source.line("            }");
                            }
                        }
                    }
                }
                break;
            }
            default:
                break;
            }
        }

        if (activeSourceLine) {
            emitGeneratedLineReset(source, options.emitLineDirectives);
            activeSourceLine = false;
        }
        emitTypedRawConsume(
            source, rawConsumeBatches[block.instructions.size()], "            ");
        emitTypedConsume(source, block.terminator.kind, "            ");
        bool explicitTermPoint = false;
        const auto resolvedTermPoint = resolveSequencePoint(
            module,
            function,
            block.id,
            static_cast<std::uint32_t>(block.instructions.size()),
            true,
            block.terminator.sourceSpan,
            &explicitTermPoint);
        switch (block.terminator.kind) {
        case mir::TerminatorKind::ReturnVoid:
            emitLineDirective(
                source, module, resolvedTermPoint, options.emitLineDirectives,
                &sourceMap, function.symbolId, explicitTermPoint);
            source.line("            return true;");
            break;
        case mir::TerminatorKind::ReturnValue: {
            const auto value = typedValueExpression(block.terminator.value);
            emitLineDirective(
                source, module, resolvedTermPoint, options.emitLineDirectives,
                &sourceMap, function.symbolId, explicitTermPoint);
            source.line("            typedOutResult = " + value + ";");
            source.line("            return true;");
            break;
        }
        case mir::TerminatorKind::Jump:
            if (const auto* targetBlock = findBlock(function, block.terminator.target)) {
                source.line("            if constexpr (FastAccounting && !TrustedRelease) {");
                source.line("                rawAccounting.branch();");
                source.line("            } else if constexpr (DeterminismAccounting && !TrustedRelease) {");
                source.line("                determinismAccounting.branch(" +
                    std::to_string(block.terminator.target) + ");");
                source.line("            } else if constexpr (!TrustedRelease) {");
                source.line("                if (!context.branch(" +
                    std::to_string(block.terminator.target) + ")) return false;");
                source.line("            }");
                emitLineDirective(
                    source, module, resolvedTermPoint, options.emitLineDirectives,
                    &sourceMap, function.symbolId, explicitTermPoint);
                for (std::size_t idx = 0;
                     idx < block.terminator.arguments.size() && idx < targetBlock->parameters.size();
                     ++idx) {
                    source.line("            const auto typedEdge_" + std::to_string(idx) + " = " +
                        typedValueExpression(block.terminator.arguments[idx]) + ";");
                }
                for (std::size_t idx = 0;
                     idx < block.terminator.arguments.size() && idx < targetBlock->parameters.size();
                     ++idx) {
                    source.line("            " +
                        typedValueExpression(targetBlock->parameters[idx].value) +
                        " = typedEdge_" + std::to_string(idx) + ";");
                }
                source.line("            goto typedBlock_" + std::to_string(block.terminator.target) + ";");
            }
            break;
        case mir::TerminatorKind::Branch:
            emitLineDirective(
                source, module, resolvedTermPoint, options.emitLineDirectives,
                &sourceMap, function.symbolId, explicitTermPoint);
            source.line("            if (" +
                typedValueExpression(block.terminator.condition) + ") {");
            emitGeneratedLineReset(source, options.emitLineDirectives);
            emitTypedTransfer(
                source, function, block.terminator.target,
                block.terminator.arguments, "typedTrueEdge", "                ");
            source.line("            } else {");
            emitTypedTransfer(
                source, function, block.terminator.falseTarget,
                block.terminator.falseArguments, "typedFalseEdge", "                ");
            source.line("            }");
            break;
        case mir::TerminatorKind::Throw:
        case mir::TerminatorKind::None:
            break;
        }
        emitGeneratedLineReset(source, options.emitLineDirectives);
        source.line("    }");
    }
    source.line("}");
    source.line();
    source.line("template <bool FastAccounting, bool DeterminismAccounting, bool TrustedRelease = false>");
    source.line("static bool " + view.cppName + "_typed(");
    source.line("    ExecutionContext& context,");
    source.line("    const runtime::Value* arguments,");
    source.line("    std::size_t argumentCount,");
    source.line("    runtime::Value& result) {");
    source.line("    (void)arguments;");
    source.line("    (void)argumentCount;");
    source.line("    (void)result;");
    source.line("    std::conditional_t<!FastAccounting && !DeterminismAccounting && !TrustedRelease, std::vector<runtime::PersistentRoot>, aot::EmptyGcGuards> typedGcGuards;");
    source.line("    if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
    source.line("        if (context.heap() != nullptr) {");
    source.line("            for (std::size_t idx = 0; idx < argumentCount; ++idx) {");
    source.line("                if (std::holds_alternative<runtime::ObjectRef>(arguments[idx])) {");
    source.line("                    typedGcGuards.push_back(context.heap()->retain(arguments[idx]));");
    source.line("                }");
    source.line("            }");
    source.line("        }");
    source.line("    }");
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        const auto typeId = index < function.parameterTypeIds.size()
            ? function.parameterTypeIds[index]
            : 0;
        const TypedSlotType paramSlot{function.parameterTypes[index], typeId};
        const auto argName = "unboxedArg_" + std::to_string(index);
        source.line("    " + typedCppType(paramSlot) + " " + argName + "{};");
        source.line("    if (" + std::to_string(index) +
            "u >= argumentCount) return context.fail("
            "runtime::ErrorCode::InvalidArguments, "
            "\"AOT parameter index is invalid\");");
        source.line("    if (!aot::typedUnboxScalar(" +
            primitiveExpression(paramSlot.primitive) + ", arguments[" +
            std::to_string(index) + "], " + argName +
            ")) return context.fail(runtime::ErrorCode::TypeMismatch, "
            "\"AOT parameter type mismatch\");");
    }
    std::string scalarCallArgs = "context";
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        scalarCallArgs += ", unboxedArg_" + std::to_string(index);
    }
    if (function.returnType != semantic::PrimitiveType::Void) {
        const TypedSlotType retSlot{function.returnType, function.returnTypeId};
        source.line("    " + typedCppType(retSlot) + " typedOutResult{};");
        scalarCallArgs += ", typedOutResult";
        source.line("    if (!" + view.cppName +
            "_scalar<FastAccounting, DeterminismAccounting, TrustedRelease>(" +
            scalarCallArgs + ")) return false;");
        source.line("    result = aot::typedBoxScalar(" +
            primitiveExpression(function.returnType) + ", typedOutResult);");
        source.line("    return true;");
    } else {
        source.line("    if (!" + view.cppName +
            "_scalar<FastAccounting, DeterminismAccounting, TrustedRelease>(" +
            scalarCallArgs + ")) return false;");
        source.line("    result = std::monostate{};");
        source.line("    return true;");
    }
    source.line("}");
    source.line();
    source.line("static bool " + view.cppName + "(");
    source.line("    ExecutionContext& context,");
    source.line("    const runtime::Value* arguments,");
    source.line("    std::size_t argumentCount,");
    source.line("    runtime::Value& result) {");
    if (options.trustedRelease) {
        source.line("    if (context.trustedReleaseEnabled() || context.fastAccountingEnabled()) {");
        source.line("        return " + view.cppName +
            "_typed<false, false, true>(context, arguments, argumentCount, result);");
        source.line("    }");
    } else {
        source.line("    if (context.trustedReleaseEnabled()) {");
        source.line("        return " + view.cppName +
            "_typed<false, false, true>(context, arguments, argumentCount, result);");
        source.line("    }");
        source.line("    if (context.fastAccountingEnabled()) {");
        source.line("        return " + view.cppName +
            "_typed<true, false, false>(context, arguments, argumentCount, result);");
        source.line("    }");
    }
    source.line("    if (context.typedDeterminismAccountingEnabled()) {");
    source.line("        return " + view.cppName +
        "_typed<false, true, false>(context, arguments, argumentCount, result);");
    source.line("    }");
    source.line("    return " + view.cppName +
        "_typed<false, false, false>(context, arguments, argumentCount, result);");
    source.line("}");
    source.line();
}

std::string arrayValues(
    const std::vector<mir::ValueId>& values,
    const std::string& name,
    Emitter& output,
    const std::string& indent) {
    if (values.empty()) return "nullptr";
    output.line(indent + "std::array<runtime::Value, " +
        std::to_string(values.size()) + "> " + name + "{{");
    for (const auto value : values) {
        output.line(indent + "    " + valueExpression(value) + ",");
    }
    output.line(indent + "}};");
    return name + ".data()";
}

void emitTransfer(
    Emitter& output,
    const mir::Function& function,
    mir::BlockId target,
    const std::vector<mir::ValueId>& arguments,
    const std::string& prefix,
    const std::string& indent) {
    const auto* block = findBlock(function, target);
    if (!block) return;
    if (!arguments.empty()) {
        output.line(indent + "std::array<runtime::Value, " +
            std::to_string(arguments.size()) + "> " + prefix + "{{");
        for (const auto argument : arguments) {
            output.line(indent + "    " + valueExpression(argument) + ",");
        }
        output.line(indent + "}};");
        for (std::size_t index = 0;
             index < arguments.size() && index < block->parameters.size();
             ++index) {
            output.line(indent + valueExpression(block->parameters[index].value) +
                " = std::move(" + prefix + "[" + std::to_string(index) + "]);");
        }
    }
    output.line(indent + "if (!context.trustedReleaseEnabled() && !context.branch(" +
        std::to_string(target) + ")) return false;");
    output.line(indent + "currentBlock = " + std::to_string(target) + ";");
}

std::string canonicalInput(
    const std::vector<mir::Module>& modules,
    const GenerationOptions& options) {
    std::vector<const mir::Module*> ordered;
    ordered.reserve(modules.size());
    for (const auto& module : modules) ordered.push_back(&module);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->name < right->name;
    });
    std::string value = options.programName + "\n" + options.cppNamespace + "\n";
    for (const auto* module : ordered) {
        value += mir::printModule(*module);
        for (const auto& type : module->types) {
            value += "type-contract:" + semantic::canonicalTypeName(type) +
                ":base=" + std::to_string(type.baseTypeId) +
                ":abstract=" + (type.abstractType ? "1" : "0") +
                ":sealed=" + (type.sealedType ? "1" : "0") +
                ":delegate=" + (type.delegateType ? "1" : "0") +
                ":interface=" + (type.interfaceType ? "1" : "0") + ":";
            for (const auto symbolId : type.virtualDispatchTable) {
                value += std::to_string(symbolId) + ";";
            }
            for (const auto& implementation : type.interfaceDispatchMaps) {
                value += "iface#" +
                    std::to_string(implementation.interfaceTypeId) + "[";
                for (const auto symbolId : implementation.slots) {
                    value += std::to_string(symbolId) + ";";
                }
                value += "]";
            }
            value += "\n";
        }
        for (const auto& attribute : module->languageMetadata.attributes) {
            value += "attribute:" + attribute.target + ":" + attribute.name + ":" +
                attribute.sourceName + ":" + std::to_string(attribute.offset) + "(";
            for (const auto& argument : attribute.arguments) {
                value += argument.name + "=" + argument.value + ";";
            }
            value += ")\n";
        }
        for (const auto& implementation : module->languageMetadata.interfaces) {
            value += "interface:" + implementation.typeName + ":";
            for (const auto& name : implementation.interfaces) value += name + ";";
            value += "\n";
        }
        for (const auto& instantiation :
             module->languageMetadata.genericInstantiations) {
            value += "generic:" + instantiation.genericName + ":" +
                instantiation.generatedName + ":";
            for (const auto& argument : instantiation.arguments) {
                value += argument + ";";
            }
            value += "\n";
        }
        for (const auto& sequence : module->languageMetadata.sequences) {
            value += "sequence:" + sequence.typeName + ":" + sequence.name + ":" +
                sequence.resultTypeName + ":" + sequence.sourceName + ":" +
                std::to_string(sequence.offset) + ":";
            for (const auto& callback : sequence.callbacks) value += callback + ";";
            value += "\n";
        }
    }
    return value;
}

std::string jsonEscape(const std::string& value) {
    std::string output;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                std::ostringstream stream;
                stream << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << static_cast<unsigned>(character);
                output += stream.str();
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return output;
}

} // namespace

std::string sanitizeCppIdentifier(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 1);
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if ((index == 0 && std::isdigit(character)) ||
            !(std::isalnum(character) || character == '_')) {
            result.push_back('_');
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    if (result.empty()) result = "RealScriptProgram";
    return result;
}

std::string escapeCppString(const std::string& value) {
    std::string result;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20 || character >= 0x7f) {
                std::ostringstream stream;
                stream << "\\x" << std::hex << std::setw(2)
                    << std::setfill('0') << static_cast<unsigned>(character);
                result += stream.str();
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return result;
}

GeneratedProgram CppGenerator::generate(
    const std::vector<mir::Module>& modules,
    diagnostics::DiagnosticBag& diagnostics,
    GenerationOptions options) const {
    GeneratedProgram generated;
    if (modules.empty()) {
        diagnostics.report("RS7000", "AOT generation requires at least one MIR module", {});
        return generated;
    }
    options.programName = sanitizeCppIdentifier(options.programName);
    options.cppNamespace = sanitizeCppIdentifier(options.cppNamespace);
    if (sanitizeCppIdentifier(options.querySymbol) != options.querySymbol) {
        diagnostics.report("RS7001", "AOT query symbol is not a valid C identifier", {});
        return generated;
    }

    for (const auto& module : modules) {
        diagnostics::DiagnosticBag verification;
        if (!mir::verifyModule(module, verification)) {
            diagnostics.report(
                "RS7002",
                "AOT generation rejected malformed MIR module '" + module.name + "'",
                {});
            diagnostics.append(verification);
        }
    }
    if (diagnostics.hasErrors()) return generated;

    std::vector<FunctionView> functions;
    std::map<semantic::SymbolId, TypeView> types;
    std::set<semantic::SymbolId> symbols;
    for (const auto& module : modules) {
        for (const auto& type : module.types) {
            const auto inserted = types.emplace(
                type.id,
                TypeView{&type, "type_" + hexId(type.id)});
            if (!inserted.second &&
                semantic::canonicalTypeName(*inserted.first->second.type) !=
                    semantic::canonicalTypeName(type)) {
                diagnostics.report("RS7003", "duplicate AOT TypeId has incompatible names", {});
            }
        }
        for (const auto& function : module.functions) {
            if (!symbols.insert(function.symbolId).second) {
                diagnostics.report("RS7004", "duplicate AOT function SymbolId", {});
                continue;
            }
            functions.push_back(FunctionView{
                &module,
                &function,
                module.name + "::" + function.name,
                "fn_" + hexId(function.symbolId),
            });
        }
    }
    if (diagnostics.hasErrors()) return generated;
    if (functions.empty()) {
        diagnostics.report(
            "RS7006",
            "AOT generation requires at least one function",
            {});
        return generated;
    }
    std::sort(functions.begin(), functions.end(), [](const auto& left, const auto& right) {
        return left.qualifiedName < right.qualifiedName;
    });

    std::vector<CallSite> callSites;
    for (const auto& view : functions) {
        for (const auto& block : view.function->blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode == mir::Opcode::Call ||
                    instruction.opcode == mir::Opcode::NewDelegate) {
                    callSites.push_back(CallSite{callSites.size(), &instruction});
                }
            }
        }
    }

    generated.contentHash = compiler::stableFingerprint(canonicalInput(modules, options));
    std::vector<std::string> moduleNames;
    moduleNames.reserve(modules.size());
    for (const auto& module : modules) moduleNames.push_back(module.name);
    std::sort(moduleNames.begin(), moduleNames.end());

    Emitter header;
    header.line("#pragma once");
    header.line();
    header.line("#include \"realscript/aot_cpp/AotRuntime.h\"");
    header.line();
    header.line("#if defined(_WIN32) && defined(REALSCRIPT_AOT_BUILD_SHARED_MODULE)");
    header.line("#define REALSCRIPT_AOT_MODULE_EXPORT __declspec(dllexport)");
    header.line("#else");
    header.line("#define REALSCRIPT_AOT_MODULE_EXPORT");
    header.line("#endif");
    header.line();
    header.line("namespace " + options.cppNamespace + " {");
    header.line("[[nodiscard]] const realscript::aot::ProgramDescriptor& " +
        options.programName + "Program() noexcept;");
    header.line("} // namespace " + options.cppNamespace);
    header.line();
    header.line("extern \"C\" REALSCRIPT_AOT_MODULE_EXPORT RsStatusV1 " + options.querySymbol + "(");
    header.line("    const RsRuntimeApiV1* runtime_api,");
    header.line("    RsModuleExportsV1* out_exports);");
    generated.header = header.str();

    Emitter source;
    source.line("#include \"realscript_aot_generated.h\"");
    source.line();
    source.line("#include <array>");
    source.line("#include <cstddef>");
    source.line("#include <cstdint>");
    source.line("#include <limits>");
    source.line("#include <utility>");
    source.line("#include <vector>");
    if (options.trustedRelease) {
        source.line();
        source.line("#if defined(__GNUC__) && !defined(__clang__)");
        source.line("#if defined(__x86_64__)");
        source.line("#pragma GCC target(\"avx2,fma\")");
        source.line("#endif");
        source.line("#pragma GCC optimize(\"O3,tree-vectorize\")");
        source.line("#endif");
    }
    source.line();
    source.line("namespace " + options.cppNamespace + " {");
    source.line("using namespace realscript;");
    source.line("using namespace realscript::aot;");
    source.line();

    for (const auto& [id, view] : types) {
        (void)id;
        const auto& type = *view.type;
        if (!type.fields.empty()) {
            source.line("static constexpr FieldDescriptor " + view.cppName +
                "_fields[] = {");
            for (const auto& field : type.fields) {
                source.line("    {\"" + escapeCppString(field.name) + "\", " +
                    primitiveExpression(field.type) + ", 0x" +
                    hexId(exactTypeId(field.type, field.typeName)) + "ULL, " +
                    std::to_string(field.index) + "u, " +
                    (field.synthetic ? "true" : "false") + "},");
            }
            source.line("};");
        }
        if (!type.virtualDispatchTable.empty()) {
            source.line("static constexpr semantic::SymbolId " +
                view.cppName + "_virtualSlots[] = {");
            for (const auto symbolId : type.virtualDispatchTable) {
                source.line("    0x" + hexId(symbolId) + "ULL,");
            }
            source.line("};");
        }
        for (std::size_t interfaceIndex = 0;
             interfaceIndex < type.interfaceDispatchMaps.size();
             ++interfaceIndex) {
            const auto& implementation =
                type.interfaceDispatchMaps[interfaceIndex];
            if (implementation.slots.empty()) continue;
            source.line("static constexpr semantic::SymbolId " +
                view.cppName + "_interface_" +
                std::to_string(interfaceIndex) + "_slots[] = {");
            for (const auto symbolId : implementation.slots) {
                source.line("    0x" + hexId(symbolId) + "ULL,");
            }
            source.line("};");
        }
        if (!type.interfaceDispatchMaps.empty()) {
            source.line("static constexpr InterfaceDispatchDescriptor " +
                view.cppName + "_interfaceMaps[] = {");
            for (std::size_t interfaceIndex = 0;
                 interfaceIndex < type.interfaceDispatchMaps.size();
                 ++interfaceIndex) {
                const auto& implementation =
                    type.interfaceDispatchMaps[interfaceIndex];
                source.line("    {0x" +
                    hexId(implementation.interfaceTypeId) + "ULL, " +
                    (implementation.slots.empty()
                        ? std::string("nullptr")
                        : view.cppName + "_interface_" +
                            std::to_string(interfaceIndex) + "_slots") +
                    ", " + std::to_string(implementation.slots.size()) +
                    "u},");
            }
            source.line("};");
        }
        if (!type.enumMembers.empty()) {
            source.line("static constexpr EnumMemberDescriptor " + view.cppName +
                "_enumMembers[] = {");
            for (const auto& member : type.enumMembers) {
                source.line("    {\"" + escapeCppString(member.name) + "\", " +
                    std::to_string(member.value) + "LL},");
            }
            source.line("};");
        }
    }
    if (!types.empty()) {
        source.line("static constexpr TypeDescriptor programTypes[] = {");
        for (const auto& [id, view] : types) {
            (void)id;
            const auto& type = *view.type;
            source.line("    {0x" + hexId(type.id) + "ULL, " +
                typeKindExpression(type.kind) + ", 0x" +
                hexId(type.baseTypeId) + "ULL, \"" +
                escapeCppString(type.moduleName) + "\", \"" +
                escapeCppString(type.name) + "\", " +
                (type.fields.empty() ? "nullptr" : view.cppName + "_fields") +
                ", " + std::to_string(type.fields.size()) + "u, " +
                (type.enumMembers.empty() ? "nullptr" : view.cppName + "_enumMembers") +
                ", " + std::to_string(type.enumMembers.size()) + "u, " +
                (type.virtualDispatchTable.empty()
                    ? "nullptr"
                    : view.cppName + "_virtualSlots") +
                ", " + std::to_string(type.virtualDispatchTable.size()) +
                "u, " + (type.interfaceType ? "true" : "false") + ", " +
                (type.interfaceDispatchMaps.empty()
                    ? "nullptr"
                    : view.cppName + "_interfaceMaps") +
                ", " + std::to_string(type.interfaceDispatchMaps.size()) +
                "u, " + (type.delegateType ? "true" : "false") + "},");
        }
        source.line("};");
    }
    source.line();

    std::unordered_set<semantic::SymbolId> unboxableCustomStructs;
    for (const auto& [id, view] : types) {
        (void)id;
        const auto& type = *view.type;
        if (type.kind != semantic::TypeKind::Struct ||
            classifyInlineMath(semantic::PrimitiveType::Struct, type.id) !=
                InlineMathKind::None ||
            type.fields.empty() ||
            type.fields.size() > 16) {
            continue;
        }
        bool allScalarOrMath = true;
        for (const auto& field : type.fields) {
            const auto fieldTypeId = semantic::isExactType(field.type) && !field.typeName.empty()
                ? semantic::stableTypeId(field.typeName)
                : 0;
            if (field.staticField ||
                field.type == semantic::PrimitiveType::Enum ||
                field.type == semantic::PrimitiveType::Object ||
                field.type == semantic::PrimitiveType::Array ||
                field.type == semantic::PrimitiveType::String ||
                !isTypedAotSlot({field.type, fieldTypeId}, nullptr)) {
                allScalarOrMath = false;
                break;
            }
        }
        if (allScalarOrMath) {
            unboxableCustomStructs.insert(type.id);
        }
    }

    for (const auto& [id, view] : types) {
        if (unboxableCustomStructs.find(id) == unboxableCustomStructs.end()) {
            continue;
        }
        const auto& type = *view.type;
        const std::string structName = "AotStruct_" + hexId(type.id);
        source.line("struct " + structName + " {");
        source.line("    static constexpr bool kIsGeneratedAotStruct = true;");
        source.line("    static constexpr semantic::SymbolId kTypeId = 0x" + hexId(type.id) + "ULL;");
        source.line("    static constexpr std::size_t kFieldCount = " + std::to_string(type.fields.size()) + "u;");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            const auto fTypeId = exactTypeId(field.type, field.typeName);
            const TypedSlotType fSlot{field.type, fTypeId};
            source.line("    " + typedCppType(fSlot) + " f_" + std::to_string(i) +
                " = " + typedDefaultValue(fSlot) + ";");
        }
        std::string eqExpr = "true";
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            eqExpr += " && (left.f_" + std::to_string(i) + " == right.f_" + std::to_string(i) + ")";
        }
        source.line("    friend bool operator==(const " + structName + "& left, const " + structName + "& right) noexcept {");
        source.line("        return " + eqExpr + ";");
        source.line("    }");
        source.line("    friend bool operator!=(const " + structName + "& left, const " + structName + "& right) noexcept {");
        source.line("        return !(left == right);");
        source.line("    }");
        source.line("    [[nodiscard]] runtime::Value boxToValue() const {");
        source.line("        const std::array<runtime::Value, " + std::to_string(type.fields.size()) + "> fields{{");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            source.line("            aot::typedBoxScalar(" + primitiveExpression(field.type) + ", f_" + std::to_string(i) + "),");
        }
        source.line("        }};");
        source.line("        return runtime::Value{runtime::makeStructValueFromFields(kTypeId, runtime::ValueSpan(fields.data(), fields.size()))};");
        source.line("    }");
        source.line("    [[nodiscard]] static bool unboxFromValue(const runtime::Value& value, " + structName + "& out) {");
        source.line("        const auto* s = std::get_if<runtime::StructValue>(&value);");
        source.line("        if (!s || !s->valid() || s->typeId != kTypeId || s->fieldCount() != kFieldCount) return false;");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            source.line("        if (const auto fv_" + std::to_string(i) + " = runtime::structValueFieldAt(*s, " + std::to_string(i) + "u)) {");
            source.line("            if (!aot::typedUnboxScalar(" + primitiveExpression(field.type) + ", *fv_" + std::to_string(i) + ", out.f_" + std::to_string(i) + ")) return false;");
            source.line("        } else return false;");
        }
        source.line("        return true;");
        source.line("    }");
        source.line("    static bool loadFromRawColumn(const aot::TypedRawColumnCache& rawCol, std::int64_t index, " + structName + "& out) noexcept {");
        source.line("        if (RS_AOT_LIKELY(rawCol.exactNativeLayout)) {");
        source.line("            std::memcpy(&out, rawCol.basePtr + static_cast<std::size_t>(index) * sizeof(" + structName + "), sizeof(" + structName + "));");
        source.line("            return true;");
        source.line("        }");
        source.line("        const std::uint8_t* elemPtr = rawCol.basePtr + static_cast<std::size_t>(index) * rawCol.strideBytes;");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            source.line("        out.f_" + std::to_string(i) + " = aot::readRawColumnField<decltype(out.f_" +
                std::to_string(i) + ")>(elemPtr, rawCol.fields[" + std::to_string(i) + "u]);");
        }
        source.line("        return true;");
        source.line("    }");
        source.line("    static bool storeToRawColumn(const aot::TypedRawColumnCache& rawCol, std::int64_t index, const " + structName + "& val) noexcept {");
        source.line("        if (RS_AOT_LIKELY(rawCol.exactNativeLayout)) {");
        source.line("            std::memcpy(rawCol.basePtr + static_cast<std::size_t>(index) * sizeof(" + structName + "), &val, sizeof(" + structName + "));");
        source.line("            return true;");
        source.line("        }");
        source.line("        std::uint8_t* elemPtr = rawCol.basePtr + static_cast<std::size_t>(index) * rawCol.strideBytes;");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            source.line("        aot::writeRawColumnField<decltype(val.f_" +
                std::to_string(i) + ")>(elemPtr, rawCol.fields[" + std::to_string(i) + "u], val.f_" + std::to_string(i) + ");");
        }
        source.line("        return true;");
        source.line("    }");
        source.line("};");
        source.line("static constexpr aot::GeneratedFieldLayout " + structName + "_layout[] = {");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            const auto fTypeId = exactTypeId(field.type, field.typeName);
            std::string expectedKind = "runtime::NativeFieldKind::Int64";
            switch (field.type) {
            case semantic::PrimitiveType::Bool: expectedKind = "runtime::NativeFieldKind::Bool"; break;
            case semantic::PrimitiveType::Int:
            case semantic::PrimitiveType::Long: expectedKind = "runtime::NativeFieldKind::Int64"; break;
            case semantic::PrimitiveType::Float: expectedKind = "runtime::NativeFieldKind::Float32"; break;
            case semantic::PrimitiveType::Double: expectedKind = "runtime::NativeFieldKind::Float64"; break;
            case semantic::PrimitiveType::Struct:
                switch (classifyInlineMath(field.type, fTypeId)) {
                case InlineMathKind::Fixed: expectedKind = "runtime::NativeFieldKind::Fixed"; break;
                case InlineMathKind::Fixed16: expectedKind = "runtime::NativeFieldKind::Fixed16"; break;
                case InlineMathKind::Vec2: expectedKind = "runtime::NativeFieldKind::Vec2"; break;
                case InlineMathKind::Vec3: expectedKind = "runtime::NativeFieldKind::Vec3"; break;
                case InlineMathKind::Quat: expectedKind = "runtime::NativeFieldKind::Quat"; break;
                case InlineMathKind::None: break;
                }
                break;
            default: break;
            }
            source.line("    {offsetof(" + structName + ", f_" + std::to_string(i) + "), sizeof(decltype(" +
                structName + "::f_" + std::to_string(i) + ")), " + expectedKind + "},");
        }
        source.line("};");
    }
    source.line();

    std::unordered_map<semantic::SymbolId, const FunctionView*> typedFunctions;
    for (const auto& view : functions) {
        if (supportsTypedPrimitiveAot(*view.function, unboxableCustomStructs)) {
            typedFunctions.emplace(view.function->symbolId, &view);
        }
    }

    for (const auto& view : functions) {
        const auto& function = *view.function;
        if (!function.parameterTypes.empty()) {
            source.line("static constexpr semantic::PrimitiveType " + view.cppName +
                "_parameterTypes[] = {");
            for (const auto type : function.parameterTypes) {
                source.line("    " + primitiveExpression(type) + ",");
            }
            source.line("};");
            source.line("static constexpr semantic::SymbolId " + view.cppName +
                "_parameterTypeIds[] = {");
            for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
                const auto id = index < function.parameterTypeIds.size()
                    ? function.parameterTypeIds[index]
                    : 0;
                source.line("    0x" + hexId(id) + "ULL,");
            }
            source.line("};");
        }
        if (typedFunctions.find(function.symbolId) != typedFunctions.end()) {
            source.line(typedScalarFunctionSignature(view, true));
        }
        source.line("static bool " + view.cppName + "(");
        source.line("    ExecutionContext& context,");
        source.line("    const runtime::Value* arguments,");
        source.line("    std::size_t argumentCount,");
        source.line("    runtime::Value& result);");
        source.line("static RsStatusV1 abi_" + view.cppName + "(");
        source.line("    void* executionContext,");
        source.line("    const void* arguments,");
        source.line("    std::uint32_t argumentCount,");
        source.line("    void* result);");
    }
    source.line();

    for (const auto& call : callSites) {
        const auto& instruction = *call.instruction;
        const auto prefix = "call_" + std::to_string(call.id);
        if (!instruction.parameterTypes.empty()) {
            source.line("static constexpr semantic::PrimitiveType " + prefix +
                "_types[] = {");
            for (const auto type : instruction.parameterTypes) {
                source.line("    " + primitiveExpression(type) + ",");
            }
            source.line("};");
            source.line("static constexpr semantic::SymbolId " + prefix +
                "_typeIds[] = {");
            for (std::size_t index = 0;
                 index < instruction.parameterTypes.size(); ++index) {
                const auto id = index < instruction.parameterTypeIds.size()
                    ? instruction.parameterTypeIds[index]
                    : 0;
                source.line("    0x" + hexId(id) + "ULL,");
            }
            source.line("};");
        }
        source.line("static constexpr CallSignature " + prefix + "_signature{");
        source.line("    0x" + hexId(instruction.symbolId) + "ULL,");
        source.line("    \"" + escapeCppString(instruction.symbolName) + "\",");
        source.line("    " + primitiveExpression(
            instruction.opcode == mir::Opcode::NewDelegate
                ? instruction.elementType
                : instruction.resultType) + ",");
        source.line("    0x" + hexId(
            instruction.opcode == mir::Opcode::NewDelegate
                ? instruction.elementTypeId
                : instruction.resultTypeId) + "ULL,");
        source.line("    " + (instruction.parameterTypes.empty()
            ? std::string("nullptr")
            : prefix + "_types") + ",");
        source.line("    " + (instruction.parameterTypes.empty()
            ? std::string("nullptr")
            : prefix + "_typeIds") + ",");
        source.line("    " + std::to_string(instruction.parameterTypes.size()) + "u,");
        source.line(std::string("    ") +
            (instruction.virtualDispatch ? "true" : "false") + ",");
        source.line("    " + std::to_string(instruction.virtualSlot) + "u,");
        source.line(std::string("    ") +
            (instruction.interfaceDispatch ? "true" : "false") + ",");
        source.line("    0x" + hexId(instruction.interfaceTypeId) + "ULL,");
        source.line("    " + std::to_string(instruction.interfaceSlot) + "u,");
        source.line("};");
    }
    source.line();

    std::vector<SourceMapRecord> sourceMap;
    std::unordered_map<const mir::Instruction*, std::size_t> callIds;
    for (const auto& call : callSites) callIds.emplace(call.instruction, call.id);

    for (const auto& view : functions) {
        const auto& module = *view.module;
        const auto& function = *view.function;
        if (typedFunctions.find(function.symbolId) != typedFunctions.end()) {
            emitTypedPrimitiveFunction(
                source, view, options, sourceMap, typedFunctions, callIds);
        } else {
        source.line("static bool " + view.cppName + "(");
        source.line("    ExecutionContext& context,");
        source.line("    const runtime::Value* arguments,");
        source.line("    std::size_t argumentCount,");
        source.line("    runtime::Value& result) {");
        source.line("    (void)result;");
        source.line("    std::vector<runtime::Value> argumentValues;");
        source.line("    if (argumentCount != 0) {");
        source.line("        argumentValues.assign(arguments, arguments + argumentCount);");
        source.line("    }");
        source.line("    std::vector<runtime::Value> locals(" +
            std::to_string(function.localTypes.size()) + ");");
        for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
            const auto typeId = index < function.localTypeIds.size()
                ? function.localTypeIds[index]
                : 0;
            source.line("    locals[" + std::to_string(index) +
                "] = context.defaultValue(" +
                primitiveExpression(function.localTypes[index]) + ", 0x" +
                hexId(typeId) + "ULL);");
        }
        source.line("    std::vector<runtime::Value> registers(" +
            std::to_string(registerCount(function)) + ");");
        source.line("    FrameScope frame(context, argumentValues, locals, registers);");
        source.line("    std::uint32_t currentBlock = 0;");
        source.line("    for (;;) {");
        source.line("        switch (currentBlock) {");

        std::size_t functionCallIndex = 0;
        for (const auto& block : function.blocks) {
            source.line("        case " + std::to_string(block.id) + ": {");
            const auto emitExceptionRoute = [&](const std::string& indent) {
                for (const auto& handler : function.exceptionHandlers) {
                    if (std::find(
                            handler.protectedBlocks.begin(),
                            handler.protectedBlocks.end(),
                            block.id) == handler.protectedBlocks.end()) {
                        continue;
                    }
                    source.line(indent + "if (context.pendingExceptionMatches(0x" +
                        hexId(handler.catchTypeId) + "ULL)) {");
                    source.line(indent + "    if (!context.takePendingException(locals[" +
                        std::to_string(handler.exceptionLocal) +
                        "])) return false;");
                    source.line(indent + "    currentBlock = " +
                        std::to_string(handler.handlerBlock) + "u;");
                    source.line(indent + "    continue;");
                    source.line(indent + "}");
                }
            };
            bool activeSourceLine = false;
            for (std::size_t instructionIndex = 0;
                 instructionIndex < block.instructions.size();
                 ++instructionIndex) {
                const auto& instruction = block.instructions[instructionIndex];
                if (!options.trustedRelease) {
                    if (activeSourceLine) {
                        emitGeneratedLineReset(source, options.emitLineDirectives);
                        activeSourceLine = false;
                    }
                    source.line("            if (!context.trustedReleaseEnabled() && !context.consume(" +
                        std::to_string(
                            runtime::determinismOperationId(
                                mir::opcodeName(instruction.opcode))) +
                        "ULL, \"" +
                        escapeCppString(mir::opcodeName(instruction.opcode)) +
                        "\")) return false;");
                }
                bool explicitPoint = false;
                const auto resolvedPoint = resolveSequencePoint(
                    module,
                    function,
                    block.id,
                    static_cast<std::uint32_t>(instructionIndex),
                    false,
                    instruction.sourceSpan,
                    &explicitPoint);
                const std::uint32_t currentInstGenLine = source.nextLine() + (options.emitLineDirectives && resolvedPoint ? 1u : 0u);
                if (emitLineDirective(
                        source,
                        module,
                        resolvedPoint,
                        options.emitLineDirectives,
                        &sourceMap,
                        function.symbolId,
                        true)) {
                    activeSourceLine = true;
                }
                if (resolvedPoint) {
                    source.line("            context.setActiveGeneratedLine(" + std::to_string(currentInstGenLine) + "u);");
                }
                const auto result = instruction.result >= 0
                    ? valueExpression(instruction.result)
                    : std::string{};
                const auto operand = [&](std::size_t index) {
                    return valueExpression(instruction.operands[index]);
                };

                switch (instruction.opcode) {
                case mir::Opcode::Parameter:
                    source.line("            if (" +
                        std::to_string(instruction.integerImmediate) +
                        "u >= argumentValues.size()) return context.fail("
                        "runtime::ErrorCode::InvalidArguments, "
                        "\"AOT parameter index is invalid\");");
                    source.line("            " + result + " = argumentValues[" +
                        std::to_string(instruction.integerImmediate) + "];");
                    break;
                case mir::Opcode::ConstantInt:
                    if (instruction.resultType == semantic::PrimitiveType::Long) {
                        source.line("            " + result +
                            " = runtime::LongValue{" +
                            int64Literal(instruction.integerImmediate) + "};");
                    } else if (instruction.resultType == semantic::PrimitiveType::Enum) {
                        source.line("            " + result +
                            " = runtime::EnumValue{0x" +
                            hexId(instruction.resultTypeId) + "ULL, " +
                            int64Literal(instruction.integerImmediate) + "};");
                    } else {
                        source.line("            " + result + " = std::int64_t{" +
                            int64Literal(instruction.integerImmediate) + "};");
                    }
                    break;
                case mir::Opcode::ConstantDouble: {
                    std::ostringstream value;
                    value << std::setprecision(17) << instruction.doubleImmediate;
                    source.line("            " + result + " = " + value.str() + ";");
                    break;
                }
                case mir::Opcode::ConstantBool:
                    source.line("            " + result + " = " +
                        (instruction.boolImmediate ? "true" : "false") + ";");
                    break;
                case mir::Opcode::ConstantString:
                    source.line("            if (!context.constantString(\"" +
                        escapeCppString(instruction.stringImmediate) + "\", " +
                        result + ")) return false;");
                    break;
                case mir::Opcode::ConstantNull:
                    source.line("            " + result + " = std::monostate{};");
                    break;
                case mir::Opcode::ConstantTypeId:
                    source.line("            " + result +
                        " = runtime::ULongValue{0x" +
                        hexId(static_cast<semantic::SymbolId>(
                            instruction.integerImmediate)) + "ULL};");
                    break;
                case mir::Opcode::LoadLocal:
                    source.line("            " + result + " = locals[" +
                        std::to_string(instruction.localIndex) + "];");
                    break;
                case mir::Opcode::StoreLocal:
                    source.line("            locals[" +
                        std::to_string(instruction.localIndex) + "] = " +
                        operand(0) + ";");
                    break;
                case mir::Opcode::NewObject:
                    source.line("            if (!context.newObject(0x" +
                        hexId(instruction.resultTypeId != 0
                            ? instruction.resultTypeId
                            : instruction.typeId) + "ULL, " + result +
                        ")) return false;");
                    break;
                case mir::Opcode::NewStruct:
                    source.line("            if (!context.newStruct(0x" +
                        hexId(instruction.resultTypeId != 0
                            ? instruction.resultTypeId
                            : instruction.typeId) + "ULL, " + result +
                        ")) return false;");
                    break;
                case mir::Opcode::NewArray:
                    source.line("            if (!context.newArray(0x" +
                        hexId(instruction.resultTypeId) + "ULL, " +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                        ", " + result + ")) return false;");
                    break;
                case mir::Opcode::CheckNotNull:
                    source.line("            if (!context.checkNotNull(0x" +
                        hexId(instruction.typeId) + "ULL, " + operand(0) + ", " +
                        result + ")) return false;");
                    break;
                case mir::Opcode::IsType:
                case mir::Opcode::AsType:
                    source.line("            if (!context.typeOperation(" +
                        primitiveExpression(
                            instruction.parameterTypes.empty()
                                ? semantic::PrimitiveType::Error
                                : instruction.parameterTypes.front()) +
                        ", " + primitiveExpression(instruction.elementType) +
                        ", 0x" + hexId(instruction.elementTypeId) + "ULL, " +
                        operand(0) + ", " +
                        (instruction.opcode == mir::Opcode::AsType
                            ? "true"
                            : "false") +
                        ", " + result + ")) return false;");
                    break;
                case mir::Opcode::ArrayLength:
                    source.line("            if (!context.arrayLength(" + operand(0) +
                        ", " + result + ")) return false;");
                    break;
                case mir::Opcode::LoadElement:
                    source.line("            if (!context.loadElement(" +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                        ", " + operand(1) + ", " + result +
                        ")) return false;");
                    break;
                case mir::Opcode::StoreElement:
                    source.line("            if (!context.storeElement(" +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, " + operand(0) +
                        ", " + operand(1) + ", " + operand(2) +
                        ")) return false;");
                    break;
                case mir::Opcode::LoadField:
                    source.line("            if (!context.loadField(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                        ", " + result + ")) return false;");
                    break;
                case mir::Opcode::StoreField:
                    source.line("            if (!context.storeField(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                        ", " + operand(1) + ")) return false;");
                    break;
                case mir::Opcode::LoadStructField:
                    source.line("            if (!context.loadStructField(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                        ", " + result + ")) return false;");
                    break;
                case mir::Opcode::StoreStructField:
                    source.line("            if (!context.storeStructField(0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                        ", " + operand(1) + ", " + result +
                        ")) return false;");
                    break;
                case mir::Opcode::Call: {
                    const auto callId = callIds.at(&instruction);
                    const auto callIndex = functionCallIndex++;
                    const auto name = "callArguments_" +
                        std::to_string(callIndex);
                    const auto resultName = "callResult_" +
                        std::to_string(callIndex);
                    const auto pointer = arrayValues(
                        instruction.operands, name, source, "            ");
                    source.line("            runtime::Value " + resultName + ";");
                    source.line("            if (!context.call(call_" +
                        std::to_string(callId) + "_signature, " + pointer + ", " +
                        std::to_string(instruction.operands.size()) +
                        "u, " + resultName + ")) {");
                    source.line("                if (context.hasPendingException()) {");
                    emitExceptionRoute("                    ");
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                    if (instruction.result >= 0) {
                        source.line("            " + result +
                            " = std::move(" + resultName + ");");
                    }
                    break;
                }
                case mir::Opcode::NewDelegate: {
                    const auto callId = callIds.at(&instruction);
                    const auto callIndex = functionCallIndex++;
                    const auto name = "delegateTarget_" +
                        std::to_string(callIndex);
                    const auto pointer = arrayValues(
                        instruction.operands, name, source, "            ");
                    source.line("            if (!context.newDelegate(0x" +
                        hexId(instruction.typeId) + "ULL, call_" +
                        std::to_string(callId) + "_signature, " + pointer +
                        ", " + std::to_string(instruction.operands.size()) +
                        "u, " + result + ")) return false;");
                    break;
                }
                case mir::Opcode::InvokeDelegate: {
                    const auto callIndex = functionCallIndex++;
                    const auto name = "delegateArguments_" +
                        std::to_string(callIndex);
                    const auto resultName = "delegateResult_" +
                        std::to_string(callIndex);
                    const auto pointer = arrayValues(
                        instruction.operands, name, source, "            ");
                    source.line("            runtime::Value " + resultName + ";");
                    source.line("            if (!context.invokeDelegate(0x" +
                        hexId(instruction.typeId) + "ULL, " + pointer + ", " +
                        std::to_string(instruction.operands.size()) +
                        "u, " + resultName + ")) {");
                    source.line("                if (context.hasPendingException()) {");
                    emitExceptionRoute("                    ");
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                    if (instruction.result >= 0) {
                        source.line("            " + result +
                            " = std::move(" + resultName + ");");
                    }
                    break;
                }
                case mir::Opcode::CombineDelegate:
                case mir::Opcode::RemoveDelegate:
                    source.line("            if (!context.combineDelegates(0x" +
                        hexId(instruction.typeId) + "ULL, " + operand(0) +
                        ", " + operand(1) + ", " +
                        (instruction.opcode == mir::Opcode::RemoveDelegate
                            ? "true"
                            : "false") +
                        ", " + result + ")) return false;");
                    break;
                default:
                    if (isConversion(instruction.opcode)) {
                        source.line("            if (!context.convert(" +
                            conversionExpression(instruction.opcode) + ", " +
                            primitiveExpression(instruction.resultType) + ", " +
                            (instruction.checkedArithmetic ? "true" : "false") + ", " +
                            operand(0) + ", " + result + ")) return false;");
                    } else if (isUnary(instruction.opcode)) {
                        source.line("            if (!context.unary(" +
                            unaryExpression(instruction.opcode) + ", " +
                            (instruction.checkedArithmetic ? "true" : "false") + ", " + operand(0) +
                            ", " + result + ")) return false;");
                    } else if (isBinary(instruction.opcode)) {
                        source.line("            if (!context.binary(" +
                            binaryExpression(instruction.opcode) + ", " +
                            (instruction.checkedArithmetic ? "true" : "false") + ", " + operand(0) +
                            ", " + operand(1) + ", " + result +
                            ")) return false;");
                    } else {
                        diagnostics.report("RS7005", "unsupported MIR opcode in AOT generator", {});
                    }
                    break;
                }
            }

            if (!options.trustedRelease) {
                if (activeSourceLine) {
                    emitGeneratedLineReset(source, options.emitLineDirectives);
                    activeSourceLine = false;
                }
                source.line("            if (!context.trustedReleaseEnabled() && !context.consume(" +
                    std::to_string(
                        runtime::determinismOperationId(
                            mir::terminatorName(block.terminator.kind))) +
                    "ULL, \"" +
                    escapeCppString(mir::terminatorName(block.terminator.kind)) +
                    "\")) return false;");
            }
            bool explicitTermPoint = false;
            const auto resolvedTermPoint = resolveSequencePoint(
                module,
                function,
                block.id,
                static_cast<std::uint32_t>(block.instructions.size()),
                true,
                block.terminator.sourceSpan,
                &explicitTermPoint);
            const std::uint32_t currentTermGenLine = source.nextLine() + (options.emitLineDirectives && resolvedTermPoint ? 1u : 0u);
            emitLineDirective(
                source,
                module,
                resolvedTermPoint,
                options.emitLineDirectives,
                &sourceMap,
                function.symbolId,
                true);
            if (resolvedTermPoint) {
                source.line("            context.setActiveGeneratedLine(" + std::to_string(currentTermGenLine) + "u);");
            }
            switch (block.terminator.kind) {
            case mir::TerminatorKind::ReturnVoid:
                source.line("            result = std::monostate{};");
                source.line("            return true;");
                break;
            case mir::TerminatorKind::ReturnValue:
                source.line("            result = " +
                    valueExpression(block.terminator.value) + ";");
                source.line("            return true;");
                break;
            case mir::TerminatorKind::Jump:
                emitTransfer(
                    source,
                    function,
                    block.terminator.target,
                    block.terminator.arguments,
                    "edgeValues",
                    "            ");
                source.line("            continue;");
                break;
            case mir::TerminatorKind::Branch:
                source.line("            if (!context.expectType(" +
                    valueExpression(block.terminator.condition) + ", " +
                    primitiveExpression(semantic::PrimitiveType::Bool) +
                    ", \"branch\")) return false;");
                source.line("            if (std::get<bool>(" +
                    valueExpression(block.terminator.condition) + ")) {");
                emitTransfer(
                    source,
                    function,
                    block.terminator.target,
                    block.terminator.arguments,
                    "trueEdgeValues",
                    "                ");
                source.line("            } else {");
                emitTransfer(
                    source,
                    function,
                    block.terminator.falseTarget,
                    block.terminator.falseArguments,
                    "falseEdgeValues",
                    "                ");
                source.line("            }");
                source.line("            continue;");
                break;
            case mir::TerminatorKind::Throw:
                source.line("            if (!context.throwScript(" +
                    valueExpression(block.terminator.value) + ")) {");
                source.line("                if (context.hasPendingException()) {");
                emitExceptionRoute("                    ");
                source.line("                }");
                source.line("                return false;");
                source.line("            }");
                source.line("            return context.fail("
                    "runtime::ErrorCode::InvalidProgram, "
                    "\"AOT throw unexpectedly returned\");");
                break;
            case mir::TerminatorKind::None:
                source.line("            return context.fail("
                    "runtime::ErrorCode::InvalidProgram, "
                    "\"AOT block has no terminator\");");
                break;
            }
            emitGeneratedLineReset(source, options.emitLineDirectives);
            source.line("        }");
        }
        source.line("        default:");
        source.line("            return context.fail(runtime::ErrorCode::InvalidProgram, "
            "\"AOT branch target is invalid\");");
        source.line("        }");
        source.line("    }");
        source.line("}");
        source.line();
        }

        source.line("static RsStatusV1 abi_" + view.cppName + "(");
        source.line("    void* executionContext,");
        source.line("    const void* arguments,");
        source.line("    std::uint32_t argumentCount,");
        source.line("    void* result) {");
        source.line("    if (!executionContext || !result || "
            "(argumentCount != 0 && !arguments)) {");
        source.line("        return RS_STATUS_V1_INVALID_ARGUMENT;");
        source.line("    }");
            source.line("    const auto succeeded = static_cast<ExecutionContext*>(executionContext)->invoke(");
        source.line("        0x" + hexId(view.function->symbolId) + "ULL,");
        source.line("        static_cast<const runtime::Value*>(arguments),");
        source.line("        argumentCount,");
        source.line("        *static_cast<runtime::Value*>(result));");
        source.line("    return succeeded ? RS_STATUS_V1_OK : RS_STATUS_V1_RUNTIME_ERROR;");
        source.line("}");
        source.line();
    }

    if (!sourceMap.empty()) {
        source.line("static constexpr SourceMapEntry programSourceMap[] = {");
        for (const auto& map : sourceMap) {
            source.line("    {0x" + hexId(map.symbolId) + "ULL, " +
                std::to_string(map.generatedLine) + "u, \"" +
                escapeCppString(map.sourcePath) + "\", " +
                std::to_string(map.sourceLine) + "u, " +
                std::to_string(map.sourceColumn) + "u},");
        }
        source.line("};");
    }

    source.line("static constexpr FunctionDescriptor programFunctions[] = {");
    for (const auto& view : functions) {
        const auto& function = *view.function;
        source.line("    {0x" + hexId(function.symbolId) + "ULL, \"" +
            escapeCppString(view.qualifiedName) + "\", " +
            primitiveExpression(function.returnType) + ", 0x" +
            hexId(function.returnTypeId) + "ULL, " +
            (function.parameterTypes.empty()
                ? std::string("nullptr")
                : view.cppName + "_parameterTypes") + ", " +
            (function.parameterTypes.empty()
                ? std::string("nullptr")
                : view.cppName + "_parameterTypeIds") + ", " +
            std::to_string(function.parameterTypes.size()) + "u, &" +
            view.cppName + ", nullptr},");
    }
    source.line("};");

    source.line("static constexpr RsFunctionEntryV1 abiFunctions[] = {");
    for (const auto& view : functions) {
        source.line("    {sizeof(RsFunctionEntryV1), RS_BACKEND_V1_NATIVE_AOT, 0x" +
            hexId(view.function->symbolId) + "ULL, " +
            std::to_string(GeneratedModuleVersion) + "ULL, &abi_" +
            view.cppName + ", &programFunctions[" +
            std::to_string(&view - functions.data()) + "], nullptr},");
    }
    source.line("};");

    source.line("static constexpr const char* programModuleNames[] = {");
    for (const auto& moduleName : moduleNames) {
        source.line("    \"" + escapeCppString(moduleName) + "\",");
    }
    source.line("};");

    source.line("static constexpr ProgramDescriptor programDescriptor{");
    source.line("    RuntimeAbiMajor,");
    source.line("    RuntimeAbiMinor,");
    source.line("    \"" + escapeCppString(options.programName) + "\",");
    source.line("    0x" + hexId(generated.contentHash) + "ULL,");
    source.line("    programModuleNames,");
    source.line("    " + std::to_string(moduleNames.size()) + "u,");
    source.line("    " + (types.empty() ? std::string("nullptr") : "programTypes") + ",");
    source.line("    " + std::to_string(types.size()) + "u,");
    source.line("    programFunctions,");
    source.line("    " + std::to_string(functions.size()) + "u,");
    source.line("    " + (sourceMap.empty()
        ? std::string("nullptr")
        : "programSourceMap") + ",");
    source.line("    " + std::to_string(sourceMap.size()) + "u,");
    source.line("};");
    source.line();
    source.line("const ProgramDescriptor& " + options.programName +
        "Program() noexcept {");
    source.line("    return programDescriptor;");
    source.line("}");
    source.line();
    source.line("} // namespace " + options.cppNamespace);
    source.line();
    source.line("extern \"C\" REALSCRIPT_AOT_MODULE_EXPORT RsStatusV1 " + options.querySymbol + "(");
    source.line("    const RsRuntimeApiV1* runtime_api,");
    source.line("    RsModuleExportsV1* out_exports) {");
    source.line("    if (!runtime_api || !out_exports ||");
    source.line("        runtime_api->size < sizeof(RsRuntimeApiV1) ||");
    source.line("        out_exports->size < sizeof(RsModuleExportsV1)) {");
    source.line("        return RS_STATUS_V1_INVALID_ARGUMENT;");
    source.line("    }");
    source.line("    if (runtime_api->abi_major != realscript::aot::RuntimeAbiMajor ||");
    source.line("        runtime_api->abi_minor < realscript::aot::RuntimeAbiMinor) {");
    source.line("        return RS_STATUS_V1_ABI_MISMATCH;");
    source.line("    }");
    source.line("    if (runtime_api->user_data) {");
    source.line("        realscript::runtime::detail::bindHostValuePoolBridge(runtime_api->user_data);");
    source.line("    }");
    source.line("    out_exports->required_abi_major = realscript::aot::RuntimeAbiMajor;");
    source.line("    out_exports->required_abi_minor = realscript::aot::RuntimeAbiMinor;");
    source.line("    out_exports->module_name = " + options.cppNamespace +
        "::programDescriptor.name;");
    source.line("    out_exports->content_hash = " + options.cppNamespace +
        "::programDescriptor.contentHash;");
    source.line("    out_exports->functions = " + options.cppNamespace +
        "::abiFunctions;");
    source.line("    out_exports->function_count = static_cast<std::uint32_t>(" +
        std::to_string(functions.size()) + "u);");
    source.line("    out_exports->program_descriptor = &" + options.cppNamespace +
        "::programDescriptor;");
    source.line("    return RS_STATUS_V1_OK;");
    source.line("}");

    generated.source = source.str();

    std::ostringstream manifest;
    manifest << "{\n"
        << "  \"format\": \"RealScript C++17 AOT manifest v1\",\n"
        << "  \"program\": \"" << jsonEscape(options.programName) << "\",\n"
        << "  \"runtimeAbi\": \"" << RuntimeAbiMajor << '.'
        << RuntimeAbiMinor << "\",\n"
        << "  \"contentHash\": \"0x" << hexId(generated.contentHash) << "\",\n"
        << "  \"modules\": [";
    for (std::size_t index = 0; index < moduleNames.size(); ++index) {
        if (index != 0) manifest << ", ";
        manifest << '"' << jsonEscape(moduleNames[index]) << '"';
    }
    manifest << "],\n  \"functions\": [\n";
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const auto& function = functions[index];
        manifest << "    {\"id\": \"0x" << hexId(function.function->symbolId)
            << "\", \"name\": \"" << jsonEscape(function.qualifiedName)
            << "\"}" << (index + 1 == functions.size() ? "" : ",") << '\n';
    }
    manifest << "  ],\n  \"typeCount\": " << types.size()
        << ",\n  \"sourceMapCount\": " << sourceMap.size()
        << ",\n  \"languageMetadata\": {\n";

    manifest << "    \"attributes\": [";
    bool firstMetadata = true;
    for (const auto& module : modules) {
        for (const auto& attribute : module.languageMetadata.attributes) {
            if (!firstMetadata) manifest << ',';
            firstMetadata = false;
            manifest << "\n      {\"module\": \""
                << jsonEscape(module.name) << "\", \"target\": \""
                << jsonEscape(attribute.target) << "\", \"name\": \""
                << jsonEscape(attribute.name) << "\", \"source\": \""
                << jsonEscape(attribute.sourceName) << "\", \"offset\": "
                << attribute.offset << ", \"arguments\": [";
            for (std::size_t index = 0; index < attribute.arguments.size(); ++index) {
                if (index != 0) manifest << ", ";
                manifest << "{\"name\": \""
                    << jsonEscape(attribute.arguments[index].name)
                    << "\", \"value\": \""
                    << jsonEscape(attribute.arguments[index].value) << "\"}";
            }
            manifest << "]}";
        }
    }
    if (!firstMetadata) manifest << '\n';
    manifest << "    ],\n    \"interfaces\": [";
    firstMetadata = true;
    for (const auto& module : modules) {
        for (const auto& implementation : module.languageMetadata.interfaces) {
            if (!firstMetadata) manifest << ',';
            firstMetadata = false;
            manifest << "\n      {\"module\": \""
                << jsonEscape(module.name) << "\", \"type\": \""
                << jsonEscape(implementation.typeName)
                << "\", \"interfaces\": [";
            for (std::size_t index = 0;
                 index < implementation.interfaces.size(); ++index) {
                if (index != 0) manifest << ", ";
                manifest << '"' << jsonEscape(implementation.interfaces[index]) << '"';
            }
            manifest << "]}";
        }
    }
    if (!firstMetadata) manifest << '\n';
    manifest << "    ],\n    \"genericInstantiations\": [";
    firstMetadata = true;
    for (const auto& module : modules) {
        for (const auto& instantiation :
             module.languageMetadata.genericInstantiations) {
            if (!firstMetadata) manifest << ',';
            firstMetadata = false;
            manifest << "\n      {\"module\": \""
                << jsonEscape(module.name) << "\", \"generic\": \""
                << jsonEscape(instantiation.genericName)
                << "\", \"generated\": \""
                << jsonEscape(instantiation.generatedName)
                << "\", \"arguments\": [";
            for (std::size_t index = 0;
                 index < instantiation.arguments.size(); ++index) {
                if (index != 0) manifest << ", ";
                manifest << '"' << jsonEscape(instantiation.arguments[index]) << '"';
            }
            manifest << "]}";
        }
    }
    if (!firstMetadata) manifest << '\n';
    manifest << "    ],\n    \"sequences\": [";
    firstMetadata = true;
    for (const auto& module : modules) {
        for (const auto& sequence : module.languageMetadata.sequences) {
            if (!firstMetadata) manifest << ',';
            firstMetadata = false;
            manifest << "\n      {\"module\": \""
                << jsonEscape(module.name) << "\", \"type\": \""
                << jsonEscape(sequence.typeName) << "\", \"name\": \""
                << jsonEscape(sequence.name) << "\", \"resultType\": \""
                << jsonEscape(sequence.resultTypeName)
                << "\", \"source\": \""
                << jsonEscape(sequence.sourceName) << "\", \"offset\": "
                << sequence.offset << ", \"callbacks\": [";
            for (std::size_t index = 0;
                 index < sequence.callbacks.size(); ++index) {
                if (index != 0) manifest << ", ";
                manifest << '"' << jsonEscape(sequence.callbacks[index]) << '"';
            }
            manifest << "]}";
        }
    }
    if (!firstMetadata) manifest << '\n';
    manifest << "    ]\n  }\n}\n";
    generated.manifest = manifest.str();
    return generated;
}

} // namespace realscript::aot
