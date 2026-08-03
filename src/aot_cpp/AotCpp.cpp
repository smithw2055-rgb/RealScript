#include "realscript/aot_cpp/AotCpp.h"

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/compiler/Compilation.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
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
        case semantic::PrimitiveType::Int: return "Int";
        case semantic::PrimitiveType::Long: return "Long";
        case semantic::PrimitiveType::Double: return "Double";
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
    return opcode >= mir::Opcode::ConvertNullToString &&
        opcode <= mir::Opcode::ConvertLongToDouble;
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

void emitLineDirective(
    Emitter& output,
    const mir::Module& module,
    const debug::SequencePoint* point,
    bool enabled,
    std::vector<SourceMapRecord>& sourceMap,
    semantic::SymbolId symbolId) {
    if (!point) return;
    const auto* source = findSource(module, point->range.fileId);
    if (!source || source->path.empty()) return;
    const auto line = point->range.start.line + 1;
    if (enabled) {
        output.line("#line " + std::to_string(line) + " \"" +
            escapeCppString(source->path) + "\"");
    }
    SourceMapRecord entry;
    entry.symbolId = symbolId;
    entry.generatedLine = output.nextLine();
    entry.sourcePath = source->path;
    entry.sourceLine = line;
    entry.sourceColumn = point->range.start.column + 1;
    sourceMap.push_back(std::move(entry));
}

std::string valueExpression(mir::ValueId id) {
    return "registers[" + std::to_string(id) + "]";
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
    output.line(indent + "if (!context.branch(" + std::to_string(target) +
        ")) return false;");
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
                sequence.sourceName + ":" + std::to_string(sequence.offset) + ":";
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
                if (instruction.opcode == mir::Opcode::Call) {
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
    source.line("#include <utility>");
    source.line("#include <vector>");
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
                "u},");
        }
        source.line("};");
    }
    source.line();

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
        source.line("    " + primitiveExpression(instruction.resultType) + ",");
        source.line("    0x" + hexId(instruction.resultTypeId) + "ULL,");
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
        source.line("static bool " + view.cppName + "(");
        source.line("    ExecutionContext& context,");
        source.line("    const runtime::Value* arguments,");
        source.line("    std::size_t argumentCount,");
        source.line("    runtime::Value& result) {");
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
            for (std::size_t instructionIndex = 0;
                 instructionIndex < block.instructions.size();
                 ++instructionIndex) {
                const auto& instruction = block.instructions[instructionIndex];
                emitLineDirective(
                    source,
                    module,
                    findPoint(function, block.id,
                        static_cast<std::uint32_t>(instructionIndex), false),
                    options.emitLineDirectives,
                    sourceMap,
                    function.symbolId);
                source.line("            if (!context.consume(\"" +
                    escapeCppString(mir::opcodeName(instruction.opcode)) +
                    "\")) return false;");
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
                            std::to_string(instruction.integerImmediate) + "LL};");
                    } else if (instruction.resultType == semantic::PrimitiveType::Enum) {
                        source.line("            " + result +
                            " = runtime::EnumValue{0x" +
                            hexId(instruction.resultTypeId) + "ULL, " +
                            std::to_string(instruction.integerImmediate) + "LL};");
                    } else {
                        source.line("            " + result + " = std::int64_t{" +
                            std::to_string(instruction.integerImmediate) + "LL};");
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
                        "u, " + resultName + ")) return false;");
                    if (instruction.result >= 0) {
                        source.line("            " + result +
                            " = std::move(" + resultName + ");");
                    }
                    break;
                }
                default:
                    if (isConversion(instruction.opcode)) {
                        source.line("            if (!context.convert(" +
                            conversionExpression(instruction.opcode) + ", " +
                            operand(0) + ", " + result + ")) return false;");
                    } else if (isUnary(instruction.opcode)) {
                        source.line("            if (!context.unary(" +
                            unaryExpression(instruction.opcode) + ", " + operand(0) +
                            ", " + result + ")) return false;");
                    } else if (isBinary(instruction.opcode)) {
                        source.line("            if (!context.binary(" +
                            binaryExpression(instruction.opcode) + ", " + operand(0) +
                            ", " + operand(1) + ", " + result +
                            ")) return false;");
                    } else {
                        diagnostics.report("RS7005", "unsupported MIR opcode in AOT generator", {});
                    }
                    break;
                }
            }

            emitLineDirective(
                source,
                module,
                findPoint(function, block.id,
                    static_cast<std::uint32_t>(block.instructions.size()), true),
                options.emitLineDirectives,
                sourceMap,
                function.symbolId);
            source.line("            if (!context.consume(\"" +
                escapeCppString(mir::terminatorName(block.terminator.kind)) +
                "\")) return false;");
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
            case mir::TerminatorKind::None:
                source.line("            return context.fail("
                    "runtime::ErrorCode::InvalidProgram, "
                    "\"AOT block has no terminator\");");
                break;
            }
            source.line("        }");
        }
        source.line("        default:");
        source.line("            return context.fail(runtime::ErrorCode::InvalidProgram, "
            "\"AOT branch target is invalid\");");
        source.line("        }");
        source.line("    }");
        source.line("}");
        source.line();

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
                << jsonEscape(sequence.name) << "\", \"source\": \""
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
