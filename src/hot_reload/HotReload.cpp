#include "realscript/hot_reload/HotReload.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_map>

namespace realscript::hot_reload {
namespace {

struct FunctionView {
    const bytecode::Module* module = nullptr;
    const bytecode::Function* function = nullptr;
};
using FunctionMap = std::unordered_map<semantic::SymbolId, FunctionView>;
using TypeMap = std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*>;

std::string moduleSet(const std::vector<bytecode::Module>& modules) {
    std::vector<std::string> names;
    for (const auto& module : modules) names.push_back(module.name);
    std::sort(names.begin(), names.end());
    std::ostringstream output;
    for (const auto& name : names) output << name << '\n';
    return output.str();
}

FunctionMap functions(const std::vector<bytecode::Module>& modules) {
    FunctionMap result;
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            result.emplace(function.symbolId, FunctionView{&module, &function});
        }
    }
    return result;
}

TypeMap types(const std::vector<bytecode::Module>& modules) {
    TypeMap result;
    for (const auto& module : modules) {
        for (const auto& type : module.types) result.emplace(type.id, &type);
    }
    return result;
}

bool sameField(const semantic::FieldSymbol& left, const semantic::FieldSymbol& right) {
    return left.name == right.name && left.type == right.type &&
        left.typeName == right.typeName && left.index == right.index;
}

bool sameType(const semantic::TypeSymbol& left, const semantic::TypeSymbol& right) {
    if (left.id != right.id || left.kind != right.kind ||
        left.baseTypeId != right.baseTypeId ||
        left.baseTypeName != right.baseTypeName ||
        left.moduleName != right.moduleName || left.name != right.name ||
        left.fields.size() != right.fields.size() ||
        left.enumMembers.size() != right.enumMembers.size()) return false;
    for (std::size_t index = 0; index < left.fields.size(); ++index) {
        if (!sameField(left.fields[index], right.fields[index])) return false;
    }
    for (std::size_t index = 0; index < left.enumMembers.size(); ++index) {
        if (left.enumMembers[index].name != right.enumMembers[index].name ||
            left.enumMembers[index].value != right.enumMembers[index].value) return false;
    }
    return true;
}

bool sameSignature(const bytecode::Function& left, const bytecode::Function& right) {
    return left.symbolId == right.symbolId && left.name == right.name &&
        left.returnType == right.returnType && left.returnTypeId == right.returnTypeId &&
        left.parameterTypes == right.parameterTypes &&
        left.parameterTypeIds == right.parameterTypeIds;
}

bool sameLanguageMetadata(
    const compiler::LanguageModuleMetadata& left,
    const compiler::LanguageModuleMetadata& right) {
    if (left.attributes.size() != right.attributes.size() ||
        left.interfaces.size() != right.interfaces.size() ||
        left.genericInstantiations.size() !=
            right.genericInstantiations.size() ||
        left.sequences.size() != right.sequences.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.attributes.size(); ++index) {
        const auto& first = left.attributes[index];
        const auto& second = right.attributes[index];
        if (first.target != second.target || first.name != second.name ||
            first.arguments.size() != second.arguments.size()) {
            return false;
        }
        for (std::size_t argument = 0;
             argument < first.arguments.size(); ++argument) {
            if (first.arguments[argument].name !=
                    second.arguments[argument].name ||
                first.arguments[argument].value !=
                    second.arguments[argument].value) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < left.interfaces.size(); ++index) {
        if (left.interfaces[index].typeName !=
                right.interfaces[index].typeName ||
            left.interfaces[index].interfaces !=
                right.interfaces[index].interfaces) {
            return false;
        }
    }
    for (std::size_t index = 0;
         index < left.genericInstantiations.size(); ++index) {
        const auto& first = left.genericInstantiations[index];
        const auto& second = right.genericInstantiations[index];
        if (first.genericName != second.genericName ||
            first.arguments != second.arguments ||
            first.generatedName != second.generatedName) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.sequences.size(); ++index) {
        const auto& first = left.sequences[index];
        const auto& second = right.sequences[index];
        if (first.typeName != second.typeName ||
            first.name != second.name ||
            first.callbacks != second.callbacks) {
            return false;
        }
    }
    return true;
}

void append(std::uint64_t& hash, std::uint64_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ull;
}

std::uint64_t functionBodyFingerprint(const bytecode::Module& module, const bytecode::Function& function) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto& block : function.blocks) {
        append(hash, block.id);
        for (const auto& parameter : block.parameters) {
            append(hash, parameter.target);
            append(hash, static_cast<std::uint64_t>(parameter.type));
            append(hash, parameter.typeId);
        }
        for (const auto& instruction : block.instructions) {
            append(hash, static_cast<std::uint64_t>(instruction.opcode));
            append(hash, instruction.result);
            append(hash, instruction.index);
            if (instruction.opcode == bytecode::Opcode::Call &&
                instruction.index < module.functionReferences.size()) {
                append(hash, module.functionReferences[instruction.index].symbolId);
            }
            append(hash, instruction.typeIndex);
            append(hash, static_cast<std::uint64_t>(instruction.elementType));
            append(hash, instruction.elementTypeId);
            append(hash, static_cast<std::uint64_t>(instruction.integerImmediate));
            std::uint64_t bits = 0;
            static_assert(sizeof(bits) == sizeof(instruction.doubleImmediate), "double size");
            std::memcpy(&bits, &instruction.doubleImmediate, sizeof(bits));
            append(hash, bits);
            append(hash, instruction.boolImmediate ? 1 : 0);
            for (const auto value : instruction.stringImmediate) append(hash, static_cast<unsigned char>(value));
            for (const auto operand : instruction.operands) append(hash, operand);
        }
        append(hash, static_cast<std::uint64_t>(block.terminator.kind));
        append(hash, block.terminator.condition);
        append(hash, block.terminator.value);
        append(hash, block.terminator.target);
        append(hash, block.terminator.falseTarget);
        for (const auto argument : block.terminator.arguments) append(hash, argument);
        for (const auto argument : block.terminator.falseArguments) append(hash, argument);
    }
    return hash;
}

} // namespace

ReloadPlan prepare(
    const runtime::ProgramImage& current,
    std::vector<bytecode::Module> replacementModules) {
    ReloadPlan plan;
    runtime::RuntimeError error;
    auto linked = runtime::ProgramImage::link(std::move(replacementModules), error);
    if (!linked) {
        plan.issues.push_back({ReloadIssueKind::InvalidProgram, {}, error.message});
        return plan;
    }

    const auto& oldModules = current.modules();
    const auto& newModules = linked->modules();
    if (moduleSet(oldModules) != moduleSet(newModules)) {
        plan.issues.push_back({
            ReloadIssueKind::ModuleSetChanged,
            {},
            "hot reload cannot add or remove modules",
        });
    }
    for (const auto& oldModule : oldModules) {
        const auto replacement = std::find_if(
            newModules.begin(), newModules.end(),
            [&](const bytecode::Module& module) {
                return module.name == oldModule.name;
            });
        if (replacement != newModules.end() &&
            !sameLanguageMetadata(
                oldModule.languageMetadata,
                replacement->languageMetadata)) {
            plan.issues.push_back({
                ReloadIssueKind::LanguageMetadataChanged,
                oldModule.name,
                "language metadata changed during body-only hot reload",
            });
        }
    }

    const auto oldTypes = types(oldModules);
    const auto newTypes = types(newModules);
    if (oldTypes.size() != newTypes.size()) {
        plan.issues.push_back({
            ReloadIssueKind::TypeLayoutChanged,
            {},
            "hot reload cannot add or remove types",
        });
    }
    for (const auto& [id, oldType] : oldTypes) {
        const auto found = newTypes.find(id);
        if (found == newTypes.end() || !sameType(*oldType, *found->second)) {
            plan.issues.push_back({
                ReloadIssueKind::TypeLayoutChanged,
                semantic::canonicalTypeName(*oldType),
                "type layout changed during body-only hot reload",
            });
        }
    }

    const auto oldFunctions = functions(oldModules);
    const auto newFunctions = functions(newModules);
    if (oldFunctions.size() != newFunctions.size()) {
        plan.issues.push_back({
            ReloadIssueKind::FunctionSetChanged,
            {},
            "hot reload cannot add or remove functions",
        });
    }
    for (const auto& [id, oldFunction] : oldFunctions) {
        const auto found = newFunctions.find(id);
        if (found == newFunctions.end()) {
            plan.issues.push_back({
                ReloadIssueKind::FunctionSetChanged,
                oldFunction.function->name,
                "function was removed during hot reload",
            });
            continue;
        }
        if (!sameSignature(*oldFunction.function, *found->second.function)) {
            plan.issues.push_back({
                ReloadIssueKind::FunctionSignatureChanged,
                oldFunction.function->name,
                "function signature changed",
            });
            continue;
        }
        if (functionBodyFingerprint(*oldFunction.module, *oldFunction.function) !=
            functionBodyFingerprint(*found->second.module, *found->second.function)) {
            plan.changedFunctions.push_back(id);
        }
    }

    plan.compatible = plan.issues.empty();
    if (plan.compatible) {
        plan.program = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
    }
    return plan;
}

ReloadPlan apply(
    runtime::EngineRuntime& runtime,
    std::vector<bytecode::Module> replacementModules) {
    auto current = runtime.programSnapshot();
    if (!current) {
        ReloadPlan plan;
        plan.issues.push_back({
            ReloadIssueKind::InvalidProgram,
            {},
            "runtime has no current program image",
        });
        return plan;
    }
    auto plan = prepare(*current, std::move(replacementModules));
    if (plan.compatible) runtime.replaceProgram(plan.program);
    return plan;
}

const char* reloadIssueKindName(ReloadIssueKind kind) noexcept {
    switch (kind) {
    case ReloadIssueKind::InvalidProgram: return "invalid-program";
    case ReloadIssueKind::ModuleSetChanged: return "module-set-changed";
    case ReloadIssueKind::TypeLayoutChanged: return "type-layout-changed";
    case ReloadIssueKind::FunctionSetChanged: return "function-set-changed";
    case ReloadIssueKind::FunctionSignatureChanged: return "function-signature-changed";
    case ReloadIssueKind::LanguageMetadataChanged: return "language-metadata-changed";
    }
    return "unknown";
}

} // namespace realscript::hot_reload
