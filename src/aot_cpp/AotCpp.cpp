#include "AotCppInternal.h"
#include "realscript/compiler/ModuleBuildCache.h"

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

using namespace detail;

namespace {

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
    std::string value = options.programName + "\n" + options.cppNamespace + "\n" +
        options.querySymbol + "\n" +
        "emitLineDirectives=" + (options.emitLineDirectives ? "1" : "0") + "\n" +
        "trustedRelease=" + (options.trustedRelease ? "1" : "0") + "\n" +
        "enableSharedReferenceGenerics=" +
        (options.enableSharedReferenceGenerics ? "1" : "0") + "\n";
    for (const auto* module : ordered) {
        value += mir::printModule(*module);
        for (const auto& type : module->types) {
            value += "type-contract:" + semantic::canonicalTypeName(type) +
                ":base=" + std::to_string(type.baseTypeId) +
                ":abstract=" + (type.abstractType ? "1" : "0") +
                ":sealed=" + (type.sealedType ? "1" : "0") +
                ":delegate=" + (type.delegateType ? "1" : "0") +
                ":interface=" + (type.interfaceType ? "1" : "0") +
                ":lifecycle=" + (type.hasValueLifecycle ? "1" : "0") + ":";
            for (const auto& field : type.fields) {
                value += field.name +
                    (field.staticField ? ":s;" : (field.constField ? ":c;" : ";"));
            }
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
    std::set<std::string> lifecycleTargets;
    std::set<semantic::SymbolId> symbols;
    const auto isValueLifecycleAttrName = [](std::string_view attrName) noexcept {
        if (attrName == "ValueLifecycle" || attrName == "ValueLifecycleAttribute") {
            return true;
        }
        const auto sep = attrName.rfind("::");
        if (sep != std::string_view::npos) {
            const auto tail = attrName.substr(sep + 2);
            return tail == "ValueLifecycle" || tail == "ValueLifecycleAttribute";
        }
        return false;
    };
    for (const auto& module : modules) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (isValueLifecycleAttrName(attr.name) && !attr.target.empty()) {
                lifecycleTargets.insert(attr.target);
            }
        }
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

    bool usesAsyncRuntimeInlines = false;
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    if (instruction.opcode == mir::Opcode::AsyncEnter ||
                        instruction.opcode == mir::Opcode::AwaitSuspend ||
                        instruction.opcode == mir::Opcode::AwaitResume ||
                        instruction.opcode == mir::Opcode::AsyncResolve ||
                        (instruction.opcode == mir::Opcode::Call &&
                         instruction.symbolName.find("Async.__") != std::string::npos)) {
                        usesAsyncRuntimeInlines = true;
                        break;
                    }
                }
                if (usesAsyncRuntimeInlines) break;
            }
            if (usesAsyncRuntimeInlines) break;
        }
        if (usesAsyncRuntimeInlines) break;
    }

    Emitter source;
    if (!usesAsyncRuntimeInlines) {
        source.line("#define REALSCRIPT_AOT_NO_ASYNC_INLINES 1");
    }
    source.line("#include \"realscript_aot_generated.h\"");
    source.line();
    source.line("#include <limits>");
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
                    (field.synthetic ? "true" : "false") + ", " +
                    (field.staticField ? "true" : "false") + ", " +
                    (field.constField ? "true" : "false") + "},");
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
            const std::string canonical = semantic::canonicalTypeName(type);
            const bool hasLifecycle = type.hasValueLifecycle ||
                lifecycleTargets.count(canonical) > 0 ||
                canonical == "Core::ResourceHandle" ||
                canonical.rfind("Core::ResourceHandle<", 0) == 0;
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
                "u, " + (type.delegateType ? "true" : "false") +
                ", " + (hasLifecycle ? "true" : "false") + "},");
        }
        source.line("};");
    }
    source.line();

    std::unordered_set<semantic::SymbolId> unboxableCustomStructs;
    std::vector<semantic::SymbolId> orderedUnboxableStructs;
    for (bool progress = true; progress;) {
        progress = false;
        for (const auto& [id, view] : types) {
            if (unboxableCustomStructs.find(id) != unboxableCustomStructs.end()) {
                continue;
            }
            const auto& type = *view.type;
            if (type.kind != semantic::TypeKind::Struct ||
                classifyInlineMath(semantic::PrimitiveType::Struct, type.id) !=
                    InlineMathKind::None ||
                type.fields.empty() ||
                type.fields.size() > 256) {
                continue;
            }
            bool allScalarMathOrNested = true;
            for (const auto& field : type.fields) {
                const auto fieldTypeId = semantic::isExactType(field.type) && !field.typeName.empty()
                    ? semantic::stableTypeId(field.typeName)
                    : 0;
                if (field.staticField ||
                    !isTypedAotSlot({field.type, fieldTypeId}, &unboxableCustomStructs)) {
                    allScalarMathOrNested = false;
                    break;
                }
            }
            if (allScalarMathOrNested) {
                unboxableCustomStructs.insert(type.id);
                orderedUnboxableStructs.push_back(type.id);
                progress = true;
            }
        }
    }

    std::unordered_map<semantic::SymbolId, std::size_t> structGcSlotCounts;
    for (const auto id : orderedUnboxableStructs) {
        const auto viewIt = types.find(id);
        if (viewIt == types.end()) continue;
        const auto& type = *viewIt->second.type;
        bool hasNestedCustomStructField = false;
        std::size_t gcSlots = 0;
        for (const auto& field : type.fields) {
            const auto fTid = exactTypeId(field.type, field.typeName);
            if ((field.type == semantic::PrimitiveType::Struct &&
                 classifyInlineMath(field.type, fTid) == InlineMathKind::None) ||
                field.type == semantic::PrimitiveType::Enum ||
                field.type == semantic::PrimitiveType::Object ||
                field.type == semantic::PrimitiveType::Array ||
                field.type == semantic::PrimitiveType::String) {
                hasNestedCustomStructField = true;
            }
            if (field.type == semantic::PrimitiveType::Object ||
                field.type == semantic::PrimitiveType::Array ||
                field.type == semantic::PrimitiveType::String) {
                ++gcSlots;
            } else if (field.type == semantic::PrimitiveType::Struct &&
                       classifyInlineMath(field.type, fTid) == InlineMathKind::None) {
                const auto gcIt = structGcSlotCounts.find(fTid);
                if (gcIt != structGcSlotCounts.end()) {
                    gcSlots += gcIt->second;
                }
            }
        }
        structGcSlotCounts[type.id] = gcSlots;
        const std::string structName = "AotStruct_" + hexId(type.id);
        source.line("struct " + structName + " {");
        source.line("    // RealScript struct: " + semantic::canonicalTypeName(type));
        source.line("    static constexpr bool kIsGeneratedAotStruct = true;");
        source.line("    static constexpr semantic::SymbolId kTypeId = 0x" + hexId(type.id) + "ULL;");
        source.line("    static constexpr std::size_t kFieldCount = " + std::to_string(type.fields.size()) + "u;");
        source.line("    static constexpr std::size_t kGcSlotCount = " + std::to_string(gcSlots) + "u;");
        source.line("    static void retainGcSlots(runtime::ManagedHeap* rawH, runtime::PersistentRoot*& slotCursor, const " + structName + "& val) {");
        if (gcSlots == 0) {
            source.line("        (void)rawH; (void)slotCursor; (void)val;");
        } else {
            for (std::size_t i = 0; i < type.fields.size(); ++i) {
                const auto& field = type.fields[i];
                const auto fTid = exactTypeId(field.type, field.typeName);
                if (field.type == semantic::PrimitiveType::Object ||
                    field.type == semantic::PrimitiveType::Array ||
                    field.type == semantic::PrimitiveType::String) {
                    source.line("        aot::retainTypedGcSlot(rawH, *slotCursor++, val.f_" + std::to_string(i) + ");");
                } else if (field.type == semantic::PrimitiveType::Struct &&
                           classifyInlineMath(field.type, fTid) == InlineMathKind::None) {
                    const auto gcIt = structGcSlotCounts.find(fTid);
                    if (gcIt != structGcSlotCounts.end() && gcIt->second > 0) {
                        source.line("        AotStruct_" + hexId(fTid) + "::retainGcSlots(rawH, slotCursor, val.f_" + std::to_string(i) + ");");
                    }
                }
            }
        }
        source.line("    }");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            const auto fTypeId = exactTypeId(field.type, field.typeName);
            const TypedSlotType fSlot{field.type, fTypeId};
            source.line("    " + typedCppType(fSlot) + " f_" + std::to_string(i) +
                " = " + typedDefaultValue(fSlot) + "; // " + field.name);
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
        std::string heapEqExpr = "true";
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            const auto fTid = exactTypeId(field.type, field.typeName);
            const auto idxStr = std::to_string(i);
            if (field.type == semantic::PrimitiveType::String) {
                heapEqExpr += " && aot::equalStringValuesNoThrow(rawH, left.f_" + idxStr + ", right.f_" + idxStr + ")";
            } else if (field.type == semantic::PrimitiveType::Struct &&
                       classifyInlineMath(field.type, fTid) == InlineMathKind::None) {
                heapEqExpr += " && AotStruct_" + hexId(fTid) + "::equalsWithHeap(rawH, left.f_" + idxStr + ", right.f_" + idxStr + ")";
            } else {
                heapEqExpr += " && (left.f_" + idxStr + " == right.f_" + idxStr + ")";
            }
        }
        source.line("    [[nodiscard]] static bool equalsWithHeap(const runtime::ManagedHeap* rawH, const " + structName + "& left, const " + structName + "& right) noexcept {");
        source.line("        (void)rawH;");
        source.line("        return " + heapEqExpr + ";");
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
        source.line("    [[nodiscard]] static bool loadFromFlatLeaves(const runtime::Value*& cur, " + structName + "& out) noexcept {");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            const auto fTid = exactTypeId(field.type, field.typeName);
            if (field.type == semantic::PrimitiveType::Struct &&
                classifyInlineMath(field.type, fTid) == InlineMathKind::None) {
                source.line("        if (!AotStruct_" + hexId(fTid) + "::loadFromFlatLeaves(cur, out.f_" + std::to_string(i) + ")) return false;");
            } else {
                source.line("        if (!aot::typedUnboxScalar(" + primitiveExpression(field.type) + ", *cur++, out.f_" + std::to_string(i) + ")) return false;");
            }
        }
        source.line("        return true;");
        source.line("    }");
        source.line("    static void storeToFlatLeaves(runtime::Value*& cur, const " + structName + "& val) {");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            const auto fTid = exactTypeId(field.type, field.typeName);
            if (field.type == semantic::PrimitiveType::Struct &&
                classifyInlineMath(field.type, fTid) == InlineMathKind::None) {
                source.line("        AotStruct_" + hexId(fTid) + "::storeToFlatLeaves(cur, val.f_" + std::to_string(i) + ");");
            } else {
                source.line("        *cur++ = aot::typedBoxScalar(" + primitiveExpression(field.type) + ", val.f_" + std::to_string(i) + ");");
            }
        }
        source.line("    }");
        source.line("    static void storeToFlatLeavesPinned(runtime::Value*& cur, const " + structName + "& val) {");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            const auto fTid = exactTypeId(field.type, field.typeName);
            const auto mathKind = classifyInlineMath(field.type, fTid);
            if (field.type == semantic::PrimitiveType::Struct &&
                mathKind == InlineMathKind::None) {
                source.line("        AotStruct_" + hexId(fTid) + "::storeToFlatLeavesPinned(cur, val.f_" + std::to_string(i) + ");");
            } else if (field.type == semantic::PrimitiveType::String ||
                       field.type == semantic::PrimitiveType::Enum ||
                       mathKind == InlineMathKind::Quat ||
                       mathKind == InlineMathKind::Quatf) {
                source.line("        {");
                source.line("            const runtime::Value lv_" + std::to_string(i) + " = aot::typedBoxScalar(" + primitiveExpression(field.type) + ", val.f_" + std::to_string(i) + ");");
                source.line("            runtime::pinValueInPools(lv_" + std::to_string(i) + ");");
                source.line("            *cur++ = lv_" + std::to_string(i) + ";");
                source.line("        }");
            } else {
                source.line("        *cur++ = aot::typedBoxScalar(" + primitiveExpression(field.type) + ", val.f_" + std::to_string(i) + ");");
            }
        }
        source.line("    }");
        source.line("    [[nodiscard]] static bool unboxFromValue(const runtime::Value& value, " + structName + "& out) {");
        source.line("        const auto* s = std::get_if<runtime::StructValue>(&value);");
        source.line("        if (!s || !s->valid() || s->typeId != kTypeId || s->fieldCount() != kFieldCount) return false;");
        source.line("        if (s->isFrameSlice()) {");
        source.line("            if (const auto* sc = runtime::getFlatStructSchema(s->frameSchemaId())) {");
        source.line("                if (const runtime::Value* leaves = runtime::detail::lookupFlatStackSlice(s->storage.frameSliceBank(), s->storage.frameSliceOffset(), sc->flatSlotCount)) {");
        source.line("                    return loadFromFlatLeaves(leaves, out);");
        source.line("                }");
        source.line("            }");
        source.line("        }");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            source.line("        if (const auto fv_" + std::to_string(i) + " = runtime::structValueFieldAt(*s, " + std::to_string(i) + "u)) {");
            source.line("            if (!aot::typedUnboxScalar(" + primitiveExpression(field.type) + ", *fv_" + std::to_string(i) + ", out.f_" + std::to_string(i) + ")) return false;");
            source.line("        } else return false;");
        }
        source.line("        return true;");
        source.line("    }");
        source.line("    static bool loadFromRawColumn(const aot::TypedRawColumnCache& rawCol, std::int64_t index, " + structName + "& out) noexcept {");
        if (hasNestedCustomStructField) {
            source.line("        (void)rawCol; (void)index; (void)out; return false;");
        } else {
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
        }
        source.line("    }");
        source.line("    static bool storeToRawColumn(const aot::TypedRawColumnCache& rawCol, std::int64_t index, const " + structName + "& val) noexcept {");
        if (hasNestedCustomStructField) {
            source.line("        (void)rawCol; (void)index; (void)val; return false;");
        } else {
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
        }
        source.line("    }");
        source.line("};");
        source.line("static constexpr aot::GeneratedFieldLayout " + structName + "_layout[] = {");
        for (std::size_t i = 0; i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            const auto fTypeId = exactTypeId(field.type, field.typeName);
            std::string expectedKind = "runtime::NativeFieldKind::Int64";
            switch (field.type) {
            case semantic::PrimitiveType::Bool: expectedKind = "runtime::NativeFieldKind::Bool"; break;
            case semantic::PrimitiveType::SByte:
            case semantic::PrimitiveType::Byte:
            case semantic::PrimitiveType::Short:
            case semantic::PrimitiveType::UShort:
            case semantic::PrimitiveType::Int:
            case semantic::PrimitiveType::UInt:
            case semantic::PrimitiveType::Long: expectedKind = "runtime::NativeFieldKind::Int64"; break;
            case semantic::PrimitiveType::ULong: expectedKind = "runtime::NativeFieldKind::UInt64"; break;
            case semantic::PrimitiveType::Float: expectedKind = "runtime::NativeFieldKind::Float32"; break;
            case semantic::PrimitiveType::Double: expectedKind = "runtime::NativeFieldKind::Float64"; break;
            case semantic::PrimitiveType::Struct:
                switch (classifyInlineMath(field.type, fTypeId)) {
                case InlineMathKind::Fixed: expectedKind = "runtime::NativeFieldKind::Fixed"; break;
                case InlineMathKind::Fixed16: expectedKind = "runtime::NativeFieldKind::Fixed16"; break;
                case InlineMathKind::Fixed64: expectedKind = "runtime::NativeFieldKind::Fixed64"; break;
                case InlineMathKind::Vec2: expectedKind = "runtime::NativeFieldKind::Vec2"; break;
                case InlineMathKind::Vec3: expectedKind = "runtime::NativeFieldKind::Vec3"; break;
                case InlineMathKind::Quat: expectedKind = "runtime::NativeFieldKind::Quat"; break;
                case InlineMathKind::Vec2f: expectedKind = "runtime::NativeFieldKind::Vec2f"; break;
                case InlineMathKind::Vec3f: expectedKind = "runtime::NativeFieldKind::Vec3f"; break;
                case InlineMathKind::Quatf: expectedKind = "runtime::NativeFieldKind::Quatf"; break;
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

    std::unordered_map<const mir::Instruction*, std::size_t> callIds;
    for (const auto& call : callSites) callIds.emplace(call.instruction, call.id);

    struct SharedGenericMemberSpec {
        const FunctionView* view = nullptr;
        std::vector<semantic::SymbolId> typeIds;
        std::vector<std::size_t> callIds;
    };
    struct SharedGenericGroup {
        std::size_t groupIndex = 0;
        std::string stemKey;
        std::vector<SharedGenericMemberSpec> members;
    };

    std::unordered_map<semantic::SymbolId, const FunctionView*> typedFunctions;
    for (const auto& view : functions) {
        if (view.module->name == "RealScript.Async" &&
            view.function->name.rfind("Async.__", 0) == 0) {
            continue;
        }
        if (supportsTypedPrimitiveAot(*view.function, unboxableCustomStructs)) {
            typedFunctions.emplace(view.function->symbolId, &view);
        }
    }

    std::vector<SharedGenericGroup> sharedGenericGroups;
    std::unordered_map<semantic::SymbolId, std::pair<std::size_t, std::size_t>> sharedGenericBySymbol;

    if (options.enableSharedReferenceGenerics) {
        std::unordered_map<semantic::SymbolId, std::string> genericStemBySymbol;
        for (const auto& module : modules) {
            for (const auto& inst : module.languageMetadata.genericInstantiations) {
                const auto dotPos = inst.genericName.find('.');
                const std::string memberOwnerPrefix = dotPos != std::string::npos
                    ? inst.genericName.substr(0, dotPos + 1)
                    : std::string{};
                const std::string typePrefix = inst.generatedName + ".";
                const std::string enumPrefix = inst.generatedName + "__Enumerator.";
                for (const auto& fn : module.functions) {
                    if (fn.name == inst.generatedName) {
                        genericStemBySymbol[fn.symbolId] =
                            module.name + "::fn:" + inst.genericName;
                    } else if (!memberOwnerPrefix.empty() &&
                               fn.name == memberOwnerPrefix + inst.generatedName) {
                        genericStemBySymbol[fn.symbolId] =
                            module.name + "::member:" + inst.genericName;
                    } else if (fn.name.rfind(typePrefix, 0) == 0) {
                        const auto suffix = fn.name.substr(typePrefix.size());
                        genericStemBySymbol[fn.symbolId] =
                            module.name + "::type:" + inst.genericName + "." + suffix;
                    } else if (fn.name.rfind(enumPrefix, 0) == 0) {
                        const auto suffix = fn.name.substr(enumPrefix.size());
                        genericStemBySymbol[fn.symbolId] =
                            module.name + "::type:" + inst.genericName + "__Enumerator." + suffix;
                    }
                }
            }
        }

        auto buildErasedSignature = [&](const FunctionView& view,
                                        SharedGenericMemberSpec& outSpec,
                                        bool& usesReferenceSlot) -> std::string {
            const auto& fn = *view.function;
            outSpec.view = &view;
            outSpec.typeIds.clear();
            outSpec.callIds.clear();
            usesReferenceSlot = false;

            std::ostringstream sig;
            auto recordSlot = [&](semantic::PrimitiveType prim, semantic::SymbolId tid, bool pushToSpec) {
                sig << static_cast<int>(prim) << ':';
                if (prim == semantic::PrimitiveType::Object ||
                    prim == semantic::PrimitiveType::Array) {
                    sig << "ref;";
                    if (tid != 0 || prim == semantic::PrimitiveType::Object ||
                        prim == semantic::PrimitiveType::Array) {
                        usesReferenceSlot = true;
                    }
                    if (pushToSpec) outSpec.typeIds.push_back(tid);
                } else {
                    sig << tid << ';';
                    if (pushToSpec) outSpec.typeIds.push_back(tid);
                }
            };

            sig << "ret=";
            recordSlot(fn.returnType, fn.returnTypeId, false);
            sig << "params=" << fn.parameterTypes.size() << '[';
            for (std::size_t i = 0; i < fn.parameterTypes.size(); ++i) {
                const auto tid = i < fn.parameterTypeIds.size() ? fn.parameterTypeIds[i] : 0;
                recordSlot(fn.parameterTypes[i], tid, false);
            }
            sig << "]locals=" << fn.localTypes.size() << '[';
            for (std::size_t i = 0; i < fn.localTypes.size(); ++i) {
                const auto tid = i < fn.localTypeIds.size() ? fn.localTypeIds[i] : 0;
                recordSlot(fn.localTypes[i], tid, true);
            }
            sig << "]regs=" << registerCount(fn)
                << ";eh=" << fn.exceptionHandlers.size() << '[';
            for (const auto& h : fn.exceptionHandlers) {
                sig << h.exceptionLocal << ':' << h.handlerBlock << ':'
                    << (h.catchTypeId == 0 ? "all" : "typed") << '[';
                for (const auto pb : h.protectedBlocks) sig << pb << ',';
                sig << ']';
                outSpec.typeIds.push_back(h.catchTypeId);
                if (h.catchTypeId != 0) usesReferenceSlot = true;
            }
            sig << "]blocks=" << fn.blocks.size() << '{';
            for (const auto& block : fn.blocks) {
                sig << "b" << block.id << '(';
                for (const auto& bp : block.parameters) {
                    sig << bp.value << ':' << static_cast<int>(bp.type) << ',';
                    if (bp.type == semantic::PrimitiveType::Struct ||
                        bp.type == semantic::PrimitiveType::Enum) {
                        return {};
                    }
                }
                sig << ")[";
                for (const auto& inst : block.instructions) {
                    sig << static_cast<int>(inst.opcode) << ':'
                        << inst.result << ':'
                        << static_cast<int>(inst.resultType) << ':'
                        << static_cast<int>(inst.elementType) << ':'
                        << inst.localIndex << ':'
                        << inst.fieldIndex << ':'
                        << (inst.checkedArithmetic ? '1' : '0') << ':'
                        << (inst.boolImmediate ? '1' : '0') << ':'
                        << (inst.virtualDispatch ? '1' : '0') << ':'
                        << inst.virtualSlot << ':'
                        << (inst.interfaceDispatch ? '1' : '0') << ':'
                        << inst.interfaceSlot << ':';
                    if (inst.opcode != mir::Opcode::ConstantTypeId) {
                        sig << inst.integerImmediate << ':';
                    } else {
                        sig << "typeid_imm:";
                        outSpec.typeIds.push_back(
                            static_cast<semantic::SymbolId>(inst.integerImmediate));
                        usesReferenceSlot = true;
                    }
                    if (inst.opcode == mir::Opcode::ConstantDouble) {
                        std::ostringstream ds;
                        ds << std::setprecision(17) << inst.doubleImmediate;
                        sig << ds.str() << ':';
                    }
                    if (inst.opcode == mir::Opcode::ConstantString) {
                        sig << inst.stringImmediate.size() << '#' << inst.stringImmediate << ':';
                    }
                    sig << '(';
                    for (const auto op : inst.operands) sig << op << ',';
                    sig << ')';

                    switch (inst.opcode) {
                    case mir::Opcode::ConstantInt:
                        if (inst.resultType == semantic::PrimitiveType::Enum) {
                            sig << "enum=" << inst.resultTypeId << ';';
                            outSpec.typeIds.push_back(inst.resultTypeId);
                        }
                        break;
                    case mir::Opcode::NewObject: {
                        const auto effId = inst.resultTypeId != 0 ? inst.resultTypeId : inst.typeId;
                        outSpec.typeIds.push_back(effId);
                        usesReferenceSlot = true;
                        break;
                    }
                    case mir::Opcode::NewStruct: {
                        const auto effId = inst.resultTypeId != 0 ? inst.resultTypeId : inst.typeId;
                        sig << "newstruct=" << effId << ';';
                        outSpec.typeIds.push_back(effId);
                        break;
                    }
                    case mir::Opcode::NewArray:
                        recordSlot(semantic::PrimitiveType::Array, inst.resultTypeId, true);
                        recordSlot(inst.elementType, inst.elementTypeId, true);
                        break;
                    case mir::Opcode::CheckNotNull:
                    case mir::Opcode::LoadField:
                    case mir::Opcode::StoreField:
                    case mir::Opcode::InvokeDelegate:
                    case mir::Opcode::CombineDelegate:
                    case mir::Opcode::RemoveDelegate:
                    case mir::Opcode::AsyncResolve:
                        outSpec.typeIds.push_back(inst.typeId);
                        usesReferenceSlot = true;
                        break;
                    case mir::Opcode::LoadStructField:
                    case mir::Opcode::StoreStructField:
                        sig << "sfield=" << inst.typeId << ';';
                        outSpec.typeIds.push_back(inst.typeId);
                        break;
                    case mir::Opcode::IsType:
                    case mir::Opcode::AsType:
                        sig << "tp=" << (inst.parameterTypes.empty()
                            ? -1
                            : static_cast<int>(inst.parameterTypes.front())) << ';';
                        recordSlot(inst.elementType, inst.elementTypeId, true);
                        break;
                    case mir::Opcode::LoadElement:
                    case mir::Opcode::StoreElement:
                    case mir::Opcode::AwaitResume:
                        recordSlot(inst.elementType, inst.elementTypeId, true);
                        break;
                    case mir::Opcode::Call: {
                        sig << "callp=" << inst.parameterTypes.size() << '[';
                        for (std::size_t pi = 0; pi < inst.parameterTypes.size(); ++pi) {
                            const auto ptid = pi < inst.parameterTypeIds.size()
                                ? inst.parameterTypeIds[pi]
                                : 0;
                            recordSlot(inst.parameterTypes[pi], ptid, false);
                        }
                        sig << ']';
                        outSpec.callIds.push_back(callIds.at(&inst));
                        break;
                    }
                    case mir::Opcode::NewDelegate: {
                        sig << "ndelp=" << inst.parameterTypes.size() << '[';
                        for (std::size_t pi = 0; pi < inst.parameterTypes.size(); ++pi) {
                            const auto ptid = pi < inst.parameterTypeIds.size()
                                ? inst.parameterTypeIds[pi]
                                : 0;
                            recordSlot(inst.parameterTypes[pi], ptid, false);
                        }
                        sig << ']';
                        outSpec.typeIds.push_back(inst.typeId);
                        outSpec.callIds.push_back(callIds.at(&inst));
                        usesReferenceSlot = true;
                        break;
                    }
                    default:
                        break;
                    }
                    sig << '|';
                }
                sig << "]term=" << static_cast<int>(block.terminator.kind) << ':'
                    << block.terminator.condition << ':'
                    << block.terminator.value << ':'
                    << block.terminator.target << ':'
                    << block.terminator.falseTarget << '(';
                for (const auto a : block.terminator.arguments) sig << a << ',';
                sig << ")(";
                for (const auto a : block.terminator.falseArguments) sig << a << ',';
                sig << ");";
            }
            sig << '}';
            return sig.str();
        };

        std::map<std::string, std::vector<SharedGenericMemberSpec>> groupedCandidates;
        std::vector<std::string> groupOrder;
        for (const auto& view : functions) {
            if (typedFunctions.find(view.function->symbolId) != typedFunctions.end()) {
                continue;
            }
            const auto stemIt = genericStemBySymbol.find(view.function->symbolId);
            if (stemIt == genericStemBySymbol.end()) continue;
            SharedGenericMemberSpec spec;
            bool usesRef = false;
            const auto layoutSig = buildErasedSignature(view, spec, usesRef);
            if (layoutSig.empty() || !usesRef) continue;
            const std::string fullKey = stemIt->second + "\n" + layoutSig;
            auto& bucket = groupedCandidates[fullKey];
            if (bucket.empty()) groupOrder.push_back(fullKey);
            bucket.push_back(std::move(spec));
        }

        for (const auto& fullKey : groupOrder) {
            auto& bucket = groupedCandidates[fullKey];
            if (bucket.size() < 2) continue;
            SharedGenericGroup group;
            group.groupIndex = sharedGenericGroups.size();
            group.stemKey = fullKey.substr(0, fullKey.find('\n'));
            group.members = std::move(bucket);
            for (std::size_t mIdx = 0; mIdx < group.members.size(); ++mIdx) {
                sharedGenericBySymbol.emplace(
                    group.members[mIdx].view->function->symbolId,
                    std::make_pair(group.groupIndex, mIdx));
            }
            sharedGenericGroups.push_back(std::move(group));
        }
    }

    generated.sharedGenericBodyCount = sharedGenericGroups.size();
    generated.foldedGenericFunctionCount = sharedGenericBySymbol.size();

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

    if (!sharedGenericGroups.empty()) {
        source.line("// Reference-Type Shared Generic Implementation Table & Bodies");
        source.line("struct SharedRefGenericSpec {");
        source.line("    semantic::SymbolId symbolId;");
        source.line("    semantic::SymbolId returnTypeId;");
        source.line("    const semantic::SymbolId* typeIds;");
        source.line("    const CallSignature* const* calls;");
        source.line("};");
        source.line();

        for (const auto& group : sharedGenericGroups) {
            const auto& primaryView = *group.members.front().view;
            const auto& module = *primaryView.module;
            const auto& function = *primaryView.function;
            const std::string sharedFnName =
                "shared_ref_generic_" + std::to_string(group.groupIndex);
            source.line("// Shared reference-type generic body for " +
                group.stemKey + " (" + std::to_string(group.members.size()) +
                " instantiations)");
            source.line("static bool " + sharedFnName + "(");
            source.line("    ExecutionContext& context,");
            source.line("    const runtime::Value* arguments,");
            source.line("    std::size_t argumentCount,");
            source.line("    runtime::Value& result,");
            source.line("    const SharedRefGenericSpec& spec) {");
            source.line("    (void)result;");
            source.line("    (void)spec;");
            source.line("    std::vector<runtime::Value> argumentValues;");
            source.line("    if (argumentCount != 0) {");
            source.line("        argumentValues.assign(arguments, arguments + argumentCount);");
            source.line("    }");

            std::size_t specTypeCursor = 0;
            std::size_t specCallCursor = 0;
            const auto nextSpecTypeExpr = [&]() {
                return "spec.typeIds[" + std::to_string(specTypeCursor++) + "u]";
            };
            const auto nextSpecCallExpr = [&]() {
                return "(*spec.calls[" + std::to_string(specCallCursor++) + "u])";
            };

            source.line("    std::vector<runtime::Value> locals(" +
                std::to_string(function.localTypes.size()) + ");");
            for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
                const auto tidExpr = nextSpecTypeExpr();
                source.line("    locals[" + std::to_string(index) +
                    "] = context.defaultValue(" +
                    primitiveExpression(function.localTypes[index]) + ", " +
                    tidExpr + ");");
            }
            std::vector<std::string> handlerCatchExprs;
            handlerCatchExprs.reserve(function.exceptionHandlers.size());
            for (std::size_t hi = 0; hi < function.exceptionHandlers.size(); ++hi) {
                handlerCatchExprs.push_back(nextSpecTypeExpr());
            }

            source.line("    std::vector<runtime::Value> registers(" +
                std::to_string(registerCount(function)) + ");");
            const bool isAsyncFunction = !function.blocks.empty() &&
                !function.blocks.front().instructions.empty() &&
                function.blocks.front().instructions.front().opcode == mir::Opcode::AsyncEnter;
            source.line("    FrameScope frame(context, argumentValues, locals, registers, " +
                std::string(!isAsyncFunction ? "true" : "false") + ");");
            const auto allMembersNonEscapingStackRef =
                [&](std::size_t bIdx, std::size_t iIdx) noexcept -> bool {
                if (isAsyncFunction) return false;
                for (const auto& member : group.members) {
                    const auto* mf = member.view->function;
                    if (bIdx >= mf->blocks.size() ||
                        iIdx >= mf->blocks[bIdx].instructions.size() ||
                        !isNonEscapingMirStackRefAlloc(
                            *mf, mf->blocks[bIdx].instructions[iIdx], types)) {
                        return false;
                    }
                }
                return true;
            };
            if (!isAsyncFunction) {
                bool hasNonEscapingStackRef = false;
                for (std::size_t bIdx = 0; bIdx < function.blocks.size() && !hasNonEscapingStackRef; ++bIdx) {
                    for (std::size_t iIdx = 0; iIdx < function.blocks[bIdx].instructions.size(); ++iIdx) {
                        if (allMembersNonEscapingStackRef(bIdx, iIdx)) {
                            hasNonEscapingStackRef = true;
                            break;
                        }
                    }
                }
                if (hasNonEscapingStackRef) {
                    source.line("    StackRefScope aotStackRefScope(context);");
                }
            }
            source.line("    std::uint32_t currentBlock = 0;");
            if (isAsyncFunction) {
                source.line("    runtime::CoroutineFrameHandle activeCoroutine{};");
                source.line("    (void)context.tryEnterResumingCoroutine(spec.symbolId, argumentValues, locals, registers, currentBlock, activeCoroutine);");
            }
            source.line("    for (;;) {");
            source.line("        switch (currentBlock) {");

            std::size_t functionCallIndex = 0;
            for (const auto& block : function.blocks) {
                source.line("        case " + std::to_string(block.id) + ": {");
                const auto emitExceptionRoute = [&](const std::string& indent) {
                    for (std::size_t hi = 0; hi < function.exceptionHandlers.size(); ++hi) {
                        const auto& handler = function.exceptionHandlers[hi];
                        if (std::find(
                                handler.protectedBlocks.begin(),
                                handler.protectedBlocks.end(),
                                block.id) == handler.protectedBlocks.end()) {
                            continue;
                        }
                        source.line(indent + "if (context.pendingExceptionMatches(" +
                            handlerCatchExprs[hi] + ")) {");
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
                    const std::uint32_t currentInstGenLine =
                        source.nextLine() + (options.emitLineDirectives && resolvedPoint ? 1u : 0u);
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
                        for (std::size_t mIdx = 1; mIdx < group.members.size(); ++mIdx) {
                            const auto& mView = *group.members[mIdx].view;
                            bool mExplicit = false;
                            if (const auto mPoint = resolveSequencePoint(
                                    *mView.module,
                                    *mView.function,
                                    block.id,
                                    static_cast<std::uint32_t>(instructionIndex),
                                    false,
                                    mView.function->blocks[&block - function.blocks.data()]
                                        .instructions[instructionIndex]
                                        .sourceSpan,
                                    &mExplicit)) {
                                if (const auto* mSrc = findSource(*mView.module, mPoint->range.fileId);
                                    mSrc && !mSrc->path.empty()) {
                                    sourceMap.push_back(SourceMapRecord{
                                        mView.function->symbolId,
                                        currentInstGenLine,
                                        mSrc->path,
                                        static_cast<std::uint32_t>(mPoint->range.start.line + 1),
                                        static_cast<std::uint32_t>(mPoint->range.start.column + 1)});
                                }
                            }
                        }
                        source.line("            context.setActiveGeneratedLine(" +
                            std::to_string(currentInstGenLine) + "u);");
                    }
                    const auto resExpr = instruction.result >= 0
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
                        source.line("            " + resExpr + " = argumentValues[" +
                            std::to_string(instruction.integerImmediate) + "];");
                        break;
                    case mir::Opcode::ConstantInt:
                        if (instruction.resultType == semantic::PrimitiveType::Long) {
                            source.line("            " + resExpr +
                                " = runtime::LongValue{" +
                                int64Literal(instruction.integerImmediate) + "};");
                        } else if (instruction.resultType == semantic::PrimitiveType::Enum) {
                            const auto enumTidExpr = nextSpecTypeExpr();
                            source.line("            " + resExpr +
                                " = runtime::EnumValue{" + enumTidExpr + ", " +
                                int64Literal(instruction.integerImmediate) + "};");
                        } else {
                            source.line("            " + resExpr + " = std::int64_t{" +
                                int64Literal(instruction.integerImmediate) + "};");
                        }
                        break;
                    case mir::Opcode::ConstantDouble: {
                        std::ostringstream value;
                        value << std::setprecision(17) << instruction.doubleImmediate;
                        source.line("            " + resExpr + " = " + value.str() + ";");
                        break;
                    }
                    case mir::Opcode::ConstantBool:
                        source.line("            " + resExpr + " = " +
                            (instruction.boolImmediate ? "true" : "false") + ";");
                        break;
                    case mir::Opcode::ConstantString:
                        source.line("            if (!context.constantString(\"" +
                            escapeCppString(instruction.stringImmediate) + "\", " +
                            resExpr + ")) return false;");
                        break;
                    case mir::Opcode::ConstantNull:
                        source.line("            " + resExpr + " = std::monostate{};");
                        break;
                    case mir::Opcode::ConstantTypeId: {
                        const auto tidExpr = nextSpecTypeExpr();
                        source.line("            " + resExpr +
                            " = runtime::ULongValue{" + tidExpr + "};");
                        break;
                    }
                    case mir::Opcode::LoadLocal:
                        source.line("            " + resExpr + " = locals[" +
                            std::to_string(instruction.localIndex) + "];");
                        break;
                    case mir::Opcode::StoreLocal:
                        source.line("            locals[" +
                            std::to_string(instruction.localIndex) + "] = " +
                            operand(0) + ";");
                        break;
                    case mir::Opcode::NewObject: {
                        const auto tidExpr = nextSpecTypeExpr();
                        const std::size_t blkIdx = static_cast<std::size_t>(&block - function.blocks.data());
                        const char* allocFn = allMembersNonEscapingStackRef(blkIdx, instructionIndex)
                            ? "context.newStackRefObject("
                            : "context.newObject(";
                        source.line("            if (!" + std::string(allocFn) +
                            tidExpr + ", " + resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::NewStruct: {
                        const auto tidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.newStruct(" +
                            tidExpr + ", " + resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::NewArray: {
                        const auto arrTidExpr = nextSpecTypeExpr();
                        const auto elemTidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.newArray(" +
                            arrTidExpr + ", " +
                            primitiveExpression(instruction.elementType) + ", " +
                            elemTidExpr + ", " + operand(0) +
                            ", " + resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::CheckNotNull: {
                        const auto tidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.checkNotNull(" +
                            tidExpr + ", " + operand(0) + ", " +
                            resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::IsType:
                    case mir::Opcode::AsType: {
                        const auto elemTidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.typeOperation(" +
                            primitiveExpression(
                                instruction.parameterTypes.empty()
                                    ? semantic::PrimitiveType::Error
                                    : instruction.parameterTypes.front()) +
                            ", " + primitiveExpression(instruction.elementType) +
                            ", " + elemTidExpr + ", " + operand(0) + ", " +
                            (instruction.opcode == mir::Opcode::AsType ? "true" : "false") +
                            ", " + resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::ArrayLength:
                        source.line("            if (!context.arrayLength(" + operand(0) +
                            ", " + resExpr + ")) return false;");
                        break;
                    case mir::Opcode::LoadElement: {
                        const auto elemTidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.loadElement(" +
                            primitiveExpression(instruction.elementType) + ", " +
                            elemTidExpr + ", " + operand(0) +
                            ", " + operand(1) + ", " + resExpr +
                            ")) return false;");
                        break;
                    }
                    case mir::Opcode::StoreElement: {
                        const auto elemTidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.storeElement(" +
                            primitiveExpression(instruction.elementType) + ", " +
                            elemTidExpr + ", " + operand(0) +
                            ", " + operand(1) + ", " + operand(2) +
                            ")) return false;");
                        break;
                    }
                    case mir::Opcode::LoadField: {
                        const auto tidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.loadField(" +
                            tidExpr + ", " +
                            std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                            ", " + resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::StoreField: {
                        const auto tidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.storeField(" +
                            tidExpr + ", " +
                            std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                            ", " + operand(1) + ")) return false;");
                        break;
                    }
                    case mir::Opcode::LoadStructField: {
                        const auto tidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.loadStructField(" +
                            tidExpr + ", " +
                            std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                            ", " + resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::StoreStructField: {
                        const auto tidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.storeStructField(" +
                            tidExpr + ", " +
                            std::to_string(instruction.fieldIndex) + "u, " + operand(0) +
                            ", " + operand(1) + ", " + resExpr +
                            ")) return false;");
                        break;
                    }
                    case mir::Opcode::Call: {
                        const auto callSigExpr = nextSpecCallExpr();
                        const auto callIndex = functionCallIndex++;
                        const auto name = "callArguments_" + std::to_string(callIndex);
                        const auto resultName = "callResult_" + std::to_string(callIndex);
                        const auto pointer = arrayValues(
                            instruction.operands, name, source, "            ");
                        source.line("            runtime::Value " + resultName + ";");
                        source.line("            if (!context.call(" +
                            callSigExpr + ", " + pointer + ", " +
                            std::to_string(instruction.operands.size()) +
                            "u, " + resultName + ")) {");
                        source.line("                if (context.hasPendingException()) {");
                        emitExceptionRoute("                    ");
                        if (isAsyncFunction) {
                            source.line("                    return context.finishUncaughtAsyncException(activeCoroutine, spec.returnTypeId, result);");
                        }
                        source.line("                }");
                        source.line("                return false;");
                        source.line("            }");
                        if (instruction.result >= 0) {
                            source.line("            " + resExpr +
                                " = std::move(" + resultName + ");");
                        }
                        break;
                    }
                    case mir::Opcode::NewDelegate: {
                        const auto delTidExpr = nextSpecTypeExpr();
                        const auto callSigExpr = nextSpecCallExpr();
                        const auto callIndex = functionCallIndex++;
                        const auto name = "delegateTarget_" + std::to_string(callIndex);
                        const auto pointer = arrayValues(
                            instruction.operands, name, source, "            ");
                        source.line("            if (!context.newDelegate(" +
                            delTidExpr + ", " + callSigExpr + ", " + pointer +
                            ", " + std::to_string(instruction.operands.size()) +
                            "u, " + resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::InvokeDelegate: {
                        const auto delTidExpr = nextSpecTypeExpr();
                        const auto callIndex = functionCallIndex++;
                        const auto name = "delegateArguments_" + std::to_string(callIndex);
                        const auto resultName = "delegateResult_" + std::to_string(callIndex);
                        const auto pointer = arrayValues(
                            instruction.operands, name, source, "            ");
                        source.line("            runtime::Value " + resultName + ";");
                        source.line("            if (!context.invokeDelegate(" +
                            delTidExpr + ", " + pointer + ", " +
                            std::to_string(instruction.operands.size()) +
                            "u, " + resultName + ")) {");
                        source.line("                if (context.hasPendingException()) {");
                        emitExceptionRoute("                    ");
                        if (isAsyncFunction) {
                            source.line("                    return context.finishUncaughtAsyncException(activeCoroutine, spec.returnTypeId, result);");
                        }
                        source.line("                }");
                        source.line("                return false;");
                        source.line("            }");
                        if (instruction.result >= 0) {
                            source.line("            " + resExpr +
                                " = std::move(" + resultName + ");");
                        }
                        break;
                    }
                    case mir::Opcode::CombineDelegate:
                    case mir::Opcode::RemoveDelegate: {
                        const auto delTidExpr = nextSpecTypeExpr();
                        source.line("            if (!context.combineDelegates(" +
                            delTidExpr + ", " + operand(0) +
                            ", " + operand(1) + ", " +
                            (instruction.opcode == mir::Opcode::RemoveDelegate ? "true" : "false") +
                            ", " + resExpr + ")) return false;");
                        break;
                    }
                    case mir::Opcode::AsyncEnter:
                        break;
                    case mir::Opcode::AwaitSuspend:
                        source.line("            {");
                        source.line("                bool suspendedNow = false;");
                        source.line("                if (!context.awaitSuspend(spec.symbolId, spec.returnTypeId, " +
                            operand(0) + ", locals[" +
                            std::to_string(instruction.localIndex) + "], " +
                            std::to_string(instruction.integerImmediate) +
                            "u, argumentValues, locals, registers, activeCoroutine, suspendedNow, result)) return false;");
                        source.line("                if (suspendedNow) return true;");
                        source.line("            }");
                        break;
                    case mir::Opcode::AwaitResume: {
                        const auto elemTidExpr = nextSpecTypeExpr();
                        const std::string outPtr = instruction.result >= 0
                            ? ("&" + resExpr)
                            : std::string("nullptr");
                        source.line("            if (!context.awaitResume(locals[" +
                            std::to_string(instruction.localIndex) + "], " +
                            primitiveExpression(instruction.elementType) + ", " +
                            elemTidExpr + ", " + outPtr + ")) {");
                        source.line("                if (context.hasPendingException()) {");
                        emitExceptionRoute("                    ");
                        if (isAsyncFunction) {
                            source.line("                    return context.finishUncaughtAsyncException(activeCoroutine, spec.returnTypeId, result);");
                        }
                        source.line("                }");
                        source.line("                return false;");
                        source.line("            }");
                        break;
                    }
                    case mir::Opcode::AsyncResolve: {
                        const auto tidExpr = nextSpecTypeExpr();
                        const std::string payloadPtr = instruction.operands.empty()
                            ? std::string("nullptr")
                            : ("&" + operand(0));
                        source.line("            if (!context.asyncResolve(activeCoroutine, " +
                            tidExpr + ", " + payloadPtr + ", " + resExpr + ")) return false;");
                        break;
                    }
                    default:
                        if (isConversion(instruction.opcode)) {
                            source.line("            if (!context.convert(" +
                                conversionExpression(instruction.opcode) + ", " +
                                primitiveExpression(instruction.resultType) + ", " +
                                (instruction.checkedArithmetic ? "true" : "false") + ", " +
                                operand(0) + ", " + resExpr + ")) return false;");
                        } else if (isUnary(instruction.opcode)) {
                            source.line("            if (!context.unary(" +
                                unaryExpression(instruction.opcode) + ", " +
                                (instruction.checkedArithmetic ? "true" : "false") + ", " +
                                operand(0) + ", " + resExpr + ")) return false;");
                        } else if (isBinary(instruction.opcode)) {
                            source.line("            if (!context.binary(" +
                                binaryExpression(instruction.opcode) + ", " +
                                (instruction.checkedArithmetic ? "true" : "false") + ", " +
                                operand(0) + ", " + operand(1) + ", " + resExpr +
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
                const std::uint32_t currentTermGenLine =
                    source.nextLine() + (options.emitLineDirectives && resolvedTermPoint ? 1u : 0u);
                emitLineDirective(
                    source,
                    module,
                    resolvedTermPoint,
                    options.emitLineDirectives,
                    &sourceMap,
                    function.symbolId,
                    true);
                if (resolvedTermPoint) {
                    for (std::size_t mIdx = 1; mIdx < group.members.size(); ++mIdx) {
                        const auto& mView = *group.members[mIdx].view;
                        bool mExplicit = false;
                        if (const auto mPoint = resolveSequencePoint(
                                *mView.module,
                                *mView.function,
                                block.id,
                                static_cast<std::uint32_t>(block.instructions.size()),
                                true,
                                mView.function->blocks[&block - function.blocks.data()]
                                    .terminator.sourceSpan,
                                &mExplicit)) {
                            if (const auto* mSrc = findSource(*mView.module, mPoint->range.fileId);
                                mSrc && !mSrc->path.empty()) {
                                sourceMap.push_back(SourceMapRecord{
                                    mView.function->symbolId,
                                    currentTermGenLine,
                                    mSrc->path,
                                    static_cast<std::uint32_t>(mPoint->range.start.line + 1),
                                    static_cast<std::uint32_t>(mPoint->range.start.column + 1)});
                            }
                        }
                    }
                    source.line("            context.setActiveGeneratedLine(" +
                        std::to_string(currentTermGenLine) + "u);");
                }
                switch (block.terminator.kind) {
                case mir::TerminatorKind::ReturnVoid:
                    source.line("            result = std::monostate{};");
                    source.line("            return true;");
                    break;
                case mir::TerminatorKind::ReturnValue:
                    source.line("            result = runtime::detachFrameSliceValue(" +
                        valueExpression(block.terminator.value) + ");");
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
                    if (isAsyncFunction) {
                        source.line("                    return context.finishUncaughtAsyncException(activeCoroutine, spec.returnTypeId, result);");
                    }
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
    }

    for (const auto& view : functions) {
        const auto& module = *view.module;
        const auto& function = *view.function;
        const auto sharedIt = sharedGenericBySymbol.find(function.symbolId);
        if (sharedIt != sharedGenericBySymbol.end()) {
            const auto& group = sharedGenericGroups[sharedIt->second.first];
            const auto& memberSpec = group.members[sharedIt->second.second];
            std::string typesPtrExpr = "nullptr";
            if (!memberSpec.typeIds.empty()) {
                typesPtrExpr = view.cppName + "_sharedTypeIds";
                std::string arrLine = "static constexpr semantic::SymbolId " + typesPtrExpr + "[] = {";
                for (std::size_t idx = 0; idx < memberSpec.typeIds.size(); ++idx) {
                    if (idx != 0) arrLine += ", ";
                    arrLine += "0x" + hexId(memberSpec.typeIds[idx]) + "ULL";
                }
                arrLine += "};";
                source.line(arrLine);
            }
            std::string callsPtrExpr = "nullptr";
            if (!memberSpec.callIds.empty()) {
                callsPtrExpr = view.cppName + "_sharedCalls";
                std::string arrLine = "static constexpr const CallSignature* " + callsPtrExpr + "[] = {";
                for (std::size_t idx = 0; idx < memberSpec.callIds.size(); ++idx) {
                    if (idx != 0) arrLine += ", ";
                    arrLine += "&call_" + std::to_string(memberSpec.callIds[idx]) + "_signature";
                }
                arrLine += "};";
                source.line(arrLine);
            }
            source.line("static constexpr SharedRefGenericSpec " + view.cppName + "_sharedSpec{0x" +
                hexId(function.symbolId) + "ULL, 0x" +
                hexId(function.returnTypeId) + "ULL, " +
                typesPtrExpr + ", " + callsPtrExpr + "};");
            source.line("static bool " + view.cppName + "(");
            source.line("    ExecutionContext& context,");
            source.line("    const runtime::Value* arguments,");
            source.line("    std::size_t argumentCount,");
            source.line("    runtime::Value& result) {");
            source.line("    return shared_ref_generic_" +
                std::to_string(group.groupIndex) +
                "(context, arguments, argumentCount, result, " +
                view.cppName + "_sharedSpec);");
            source.line("}");
            source.line();
        } else if (typedFunctions.find(function.symbolId) != typedFunctions.end()) {
            emitTypedPrimitiveFunction(
                source, view, options, sourceMap, typedFunctions, types, callIds);
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
        const bool isAsyncFunction = !function.blocks.empty() &&
            !function.blocks.front().instructions.empty() &&
            function.blocks.front().instructions.front().opcode == mir::Opcode::AsyncEnter;
        source.line("    FrameScope frame(context, argumentValues, locals, registers, " +
            std::string(!isAsyncFunction ? "true" : "false") + ");");
        if (!isAsyncFunction) {
            bool hasNonEscapingStackRef = false;
            for (const auto& blk : function.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (isNonEscapingMirStackRefAlloc(function, inst, types)) {
                        hasNonEscapingStackRef = true;
                        break;
                    }
                }
                if (hasNonEscapingStackRef) break;
            }
            if (hasNonEscapingStackRef) {
                source.line("    StackRefScope aotStackRefScope(context);");
            }
        }
        source.line("    std::uint32_t currentBlock = 0;");
        if (isAsyncFunction) {
            source.line("    runtime::CoroutineFrameHandle activeCoroutine{};");
            source.line("    (void)context.tryEnterResumingCoroutine(0x" +
                hexId(function.symbolId) +
                "ULL, argumentValues, locals, registers, currentBlock, activeCoroutine);");
        }
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
                case mir::Opcode::NewObject: {
                    const char* allocFn = (!isAsyncFunction &&
                        isNonEscapingMirStackRefAlloc(function, instruction, types))
                        ? "context.newStackRefObject(0x"
                        : "context.newObject(0x";
                    source.line("            if (!" + std::string(allocFn) +
                        hexId(instruction.resultTypeId != 0
                            ? instruction.resultTypeId
                            : instruction.typeId) + "ULL, " + result +
                        ")) return false;");
                    break;
                }
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
                    if (isAsyncFunction) {
                        source.line("                    return context.finishUncaughtAsyncException(activeCoroutine, 0x" +
                            hexId(function.returnTypeId) + "ULL, result);");
                    }
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
                    if (isAsyncFunction) {
                        source.line("                    return context.finishUncaughtAsyncException(activeCoroutine, 0x" +
                            hexId(function.returnTypeId) + "ULL, result);");
                    }
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
                case mir::Opcode::AsyncEnter:
                    break;
                case mir::Opcode::AwaitSuspend: {
                    source.line("            {");
                    source.line("                bool suspendedNow = false;");
                    source.line("                if (!context.awaitSuspend(0x" +
                        hexId(function.symbolId) + "ULL, 0x" +
                        hexId(function.returnTypeId) + "ULL, " +
                        operand(0) + ", locals[" +
                        std::to_string(instruction.localIndex) + "], " +
                        std::to_string(instruction.integerImmediate) +
                        "u, argumentValues, locals, registers, activeCoroutine, suspendedNow, result)) return false;");
                    source.line("                if (suspendedNow) return true;");
                    source.line("            }");
                    break;
                }
                case mir::Opcode::AwaitResume: {
                    const std::string outPtr = instruction.result >= 0
                        ? ("&" + result)
                        : std::string("nullptr");
                    source.line("            if (!context.awaitResume(locals[" +
                        std::to_string(instruction.localIndex) + "], " +
                        primitiveExpression(instruction.elementType) + ", 0x" +
                        hexId(instruction.elementTypeId) + "ULL, " +
                        outPtr + ")) {");
                    source.line("                if (context.hasPendingException()) {");
                    emitExceptionRoute("                    ");
                    if (isAsyncFunction) {
                        source.line("                    return context.finishUncaughtAsyncException(activeCoroutine, 0x" +
                            hexId(function.returnTypeId) + "ULL, result);");
                    }
                    source.line("                }");
                    source.line("                return false;");
                    source.line("            }");
                    break;
                }
                case mir::Opcode::AsyncResolve: {
                    const std::string payloadPtr = instruction.operands.empty()
                        ? std::string("nullptr")
                        : ("&" + operand(0));
                    source.line("            if (!context.asyncResolve(activeCoroutine, 0x" +
                        hexId(instruction.typeId) + "ULL, " +
                        payloadPtr + ", " + result + ")) return false;");
                    break;
                }
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
                source.line("            result = runtime::detachFrameSliceValue(" +
                    valueExpression(block.terminator.value) + ");");
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
                if (isAsyncFunction) {
                    source.line("                    return context.finishUncaughtAsyncException(activeCoroutine, 0x" +
                        hexId(function.returnTypeId) + "ULL, result);");
                }
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
        std::stable_sort(
            sourceMap.begin(),
            sourceMap.end(),
            [](const SourceMapRecord& left, const SourceMapRecord& right) {
                if (left.symbolId != right.symbolId) return left.symbolId < right.symbolId;
                return left.generatedLine < right.generatedLine;
            });
        sourceMap.erase(
            std::unique(
                sourceMap.begin(),
                sourceMap.end(),
                [](const SourceMapRecord& left, const SourceMapRecord& right) {
                    return left.symbolId == right.symbolId &&
                        left.generatedLine == right.generatedLine;
                }),
            sourceMap.end());
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
        << ",\n  \"sharedGenericBodyCount\": " << generated.sharedGenericBodyCount
        << ",\n  \"foldedGenericFunctionCount\": " << generated.foldedGenericFunctionCount
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

namespace realscript::compiler {

CachedAotBuildResult ModuleBuildCache::buildAot(
    const std::vector<SourceFile>& sources,
    ModuleBuildCacheOptions cacheOptions,
    aot::GenerationOptions aotOptions) const {
    return buildAotWithGenerator(
        sources,
        std::move(cacheOptions),
        std::move(aotOptions),
        +[](const std::vector<mir::Module>& modules,
            diagnostics::DiagnosticBag& diagnostics,
            const aot::GenerationOptions& options) {
            return aot::CppGenerator{}.generate(modules, diagnostics, options);
        });
}

} // namespace realscript::compiler
