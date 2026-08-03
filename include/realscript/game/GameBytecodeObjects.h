#pragma once

#include "realscript/game/GameProductization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realscript::game {
namespace detail {

inline std::string bytecodeExactTypeName(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    const std::unordered_map<semantic::SymbolId, semantic::TypeSymbol*>& types) {
    if (!semantic::isExactType(type) || typeId == 0) return {};
    const auto found = types.find(typeId);
    return found == types.end()
        ? std::string{}
        : semantic::canonicalTypeName(*found->second);
}

inline semantic::FunctionSymbol bytecodeFunctionSymbol(
    const bytecode::Module& module,
    const bytecode::Function& function,
    semantic::TypeSymbol& owner,
    const std::unordered_map<semantic::SymbolId, semantic::TypeSymbol*>& types) {
    semantic::FunctionSymbol result;
    result.id = function.symbolId;
    result.moduleName = module.name;
    const auto ownerPrefix = owner.name + ".";
    result.name = function.name.rfind(ownerPrefix, 0) == 0
        ? function.name.substr(ownerPrefix.size())
        : function.name;
    result.ownerTypeName = owner.name;
    result.ownerTypeId = owner.id;
    result.returnType = function.returnType;
    result.returnTypeName = bytecodeExactTypeName(
        function.returnType, function.returnTypeId, types);
    result.method = true;
    result.staticMethod = false;

    result.parameters.reserve(function.parameterTypes.size());
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        semantic::VariableSymbol parameter;
        parameter.name = index == 0
            ? std::string{"this"}
            : "arg" + std::to_string(index);
        parameter.type = function.parameterTypes[index];
        parameter.typeName = bytecodeExactTypeName(
            parameter.type,
            index < function.parameterTypeIds.size()
                ? function.parameterTypeIds[index]
                : 0,
            types);
        parameter.index = index;
        parameter.parameter = true;
        parameter.id = semantic::stableTypeId(
            std::to_string(result.id) + "::parameter:" +
            std::to_string(index));
        result.parameters.push_back(std::move(parameter));
    }
    return result;
}

inline semantic::PropertySymbol* findOrCreateProperty(
    semantic::TypeSymbol& owner,
    const std::string& name) {
    const auto found = std::find_if(
        owner.properties.begin(), owner.properties.end(),
        [&](const semantic::PropertySymbol& value) {
            return value.name == name;
        });
    if (found != owner.properties.end()) return &*found;

    semantic::PropertySymbol property;
    property.name = name;
    property.id = semantic::stableTypeId(
        semantic::canonicalTypeName(owner) + "::property:" + name);
    owner.properties.push_back(std::move(property));
    return &owner.properties.back();
}

inline void sortObjectMetadata(semantic::TypeSymbol& type) {
    const auto functionLess = [](const semantic::FunctionSymbol& left,
                                 const semantic::FunctionSymbol& right) {
        return left.id < right.id;
    };
    std::sort(type.methods.begin(), type.methods.end(), functionLess);
    type.methods.erase(
        std::unique(
            type.methods.begin(), type.methods.end(),
            [](const auto& left, const auto& right) {
                return left.id == right.id;
            }),
        type.methods.end());
    std::sort(type.constructors.begin(), type.constructors.end(), functionLess);
    type.constructors.erase(
        std::unique(
            type.constructors.begin(), type.constructors.end(),
            [](const auto& left, const auto& right) {
                return left.id == right.id;
            }),
        type.constructors.end());
    std::sort(
        type.properties.begin(), type.properties.end(),
        [](const semantic::PropertySymbol& left,
           const semantic::PropertySymbol& right) {
            return left.name < right.name;
        });
}

} // namespace detail

// RSBC 0.7 persists complete object-member descriptors. Legacy RSBC artifacts
// through 0.6 preserve executable function signatures and exact receiver type
// identities but predate those descriptors, so rebuild instance methods for
// legacy modules without changing the bytecode format.
// A candidate is accepted only when its recomputed method SymbolId equals the
// encoded function SymbolId, so free functions that merely take an object as
// their first parameter are not misclassified.
inline void rehydrateGameObjectMetadata(
    std::vector<bytecode::Module>& modules) {
    std::unordered_map<semantic::SymbolId, semantic::TypeSymbol*> types;
    for (auto& module : modules) {
        for (auto& type : module.types) types.emplace(type.id, &type);
    }

    std::unordered_set<semantic::SymbolId> existing;
    for (auto& module : modules) {
        for (auto& type : module.types) {
            for (const auto& method : type.methods) existing.insert(method.id);
            for (const auto& constructor : type.constructors) {
                existing.insert(constructor.id);
            }
            for (const auto& property : type.properties) {
                if (property.getter) existing.insert(property.getter->id);
                if (property.setter) existing.insert(property.setter->id);
            }
        }
    }

    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            if (function.symbolId == 0 || function.parameterTypes.empty() ||
                function.parameterTypeIds.empty()) {
                continue;
            }
            const auto receiverType = function.parameterTypes.front();
            if (receiverType != semantic::PrimitiveType::Object &&
                receiverType != semantic::PrimitiveType::Struct) {
                continue;
            }
            const auto ownerFound = types.find(function.parameterTypeIds.front());
            if (ownerFound == types.end()) continue;
            auto& owner = *ownerFound->second;
            if ((receiverType == semantic::PrimitiveType::Object &&
                 owner.kind != semantic::TypeKind::Class) ||
                (receiverType == semantic::PrimitiveType::Struct &&
                 owner.kind != semantic::TypeKind::Struct)) {
                continue;
            }

            auto symbol = detail::bytecodeFunctionSymbol(
                module, function, owner, types);
            if (semantic::stableFunctionId(symbol) != function.symbolId ||
                !existing.insert(function.symbolId).second) {
                continue;
            }

            if (symbol.name == ".ctor") {
                symbol.constructor = true;
                owner.constructors.push_back(std::move(symbol));
                continue;
            }
            if (symbol.name.rfind("get_", 0) == 0 &&
                symbol.name.size() > 4 && symbol.parameters.size() == 1) {
                symbol.propertyGetter = true;
                auto* property = detail::findOrCreateProperty(
                    owner, symbol.name.substr(4));
                property->type = symbol.returnType;
                property->typeName = symbol.returnTypeName;
                property->getter = std::move(symbol);
                continue;
            }
            if (symbol.name.rfind("set_", 0) == 0 &&
                symbol.name.size() > 4 && symbol.parameters.size() == 2) {
                symbol.propertySetter = true;
                auto* property = detail::findOrCreateProperty(
                    owner, symbol.name.substr(4));
                property->type = symbol.parameters.back().type;
                property->typeName = symbol.parameters.back().typeName;
                property->setter = std::move(symbol);
                continue;
            }
            owner.methods.push_back(std::move(symbol));
        }
    }

    for (auto& module : modules) {
        for (auto& type : module.types) detail::sortObjectMetadata(type);
    }
}

inline GameProgramLoadResult loadGameObjectBytecodeModules(
    const GameApi& api,
    const std::vector<std::vector<std::uint8_t>>& encodedModules) {
    GameProgramLoadResult result;
    if (!api.valid()) {
        for (const auto& message : api.errors()) {
            result.diagnostics.report("RS7110", message, {});
        }
        return result;
    }
    if (encodedModules.empty()) {
        result.diagnostics.report(
            "RS7111", "no game bytecode modules were supplied", {});
        return result;
    }

    result.modules.reserve(encodedModules.size());
    for (const auto& bytes : encodedModules) {
        bytecode::Module module;
        if (!bytecode::decodeModule(bytes, module, result.diagnostics)) {
            return result;
        }
        result.modules.push_back(std::move(module));
    }
    std::sort(
        result.modules.begin(), result.modules.end(),
        [](const bytecode::Module& left, const bytecode::Module& right) {
            return left.name < right.name;
        });
    rehydrateGameObjectMetadata(result.modules);

    runtime::RuntimeError linkError;
    auto image = runtime::ProgramImage::link(result.modules, linkError);
    if (!image) {
        result.diagnostics.report("RS7112", linkError.message, {});
        return result;
    }

    result.package.program =
        std::make_shared<runtime::ProgramImage>(std::move(*image));
    result.package.bindings = api.bindings();
    result.package.heap = api.heap();
    result.package.nativeHandles = api.nativeHandles();
    result.package.programContentHash = stableProgramContentHash(result.modules);
    result.package.hostApiHash = stableGameApiHash(api);
    return result;
}

} // namespace realscript::game
