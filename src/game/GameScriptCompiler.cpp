#include "realscript/game/GameScripting.h"

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <utility>

namespace realscript::game {

GameCompileResult GameScriptCompiler::compile(
    const std::vector<compiler::SourceFile>& sources) const {
    GameCompileResult result;
    for (const auto& message : api_.errors()) {
        result.diagnostics.report("RS7000", message, {});
    }
    if (result.diagnostics.hasErrors()) return result;

    compiler::Compilation compilation;
    for (auto source : api_.generatedSources()) {
        compilation.addSource(std::move(source));
    }
    for (const auto& source : sources) compilation.addSource(source);
    auto build = compilation.build();
    result.languageMetadata.genericInstantiations.insert(
        result.languageMetadata.genericInstantiations.end(),
        build.nativeGenericInstantiations.begin(),
        build.nativeGenericInstantiations.end());
    std::sort(
        result.languageMetadata.genericInstantiations.begin(),
        result.languageMetadata.genericInstantiations.end(),
        [](const auto& left, const auto& right) {
            if (left.generatedName != right.generatedName) {
                return left.generatedName < right.generatedName;
            }
            if (left.genericName != right.genericName) {
                return left.genericName < right.genericName;
            }
            return left.arguments < right.arguments;
        });
    result.languageMetadata.genericInstantiations.erase(
        std::unique(
            result.languageMetadata.genericInstantiations.begin(),
            result.languageMetadata.genericInstantiations.end(),
            [](const auto& left, const auto& right) {
                return left.generatedName == right.generatedName &&
                    left.genericName == right.genericName &&
                    left.arguments == right.arguments;
            }),
        result.languageMetadata.genericInstantiations.end());
    result.languageMetadata.attributes.insert(
        result.languageMetadata.attributes.end(),
        build.nativeAttributes.begin(),
        build.nativeAttributes.end());
    std::stable_sort(
        result.languageMetadata.attributes.begin(),
        result.languageMetadata.attributes.end(),
        [](const auto& left, const auto& right) {
            if (left.target != right.target) {
                return left.target < right.target;
            }
            if (left.name != right.name) {
                return left.name < right.name;
            }
            if (left.sourceName != right.sourceName) {
                return left.sourceName < right.sourceName;
            }
            return left.offset < right.offset;
        });
    result.languageMetadata.sequences.insert(
        result.languageMetadata.sequences.end(),
        build.nativeSequences.begin(),
        build.nativeSequences.end());
    std::stable_sort(
        result.languageMetadata.sequences.begin(),
        result.languageMetadata.sequences.end(),
        [](const auto& left, const auto& right) {
            if (left.typeName != right.typeName) {
                return left.typeName < right.typeName;
            }
            return left.name < right.name;
        });
    result.languageMetadata.interfaces.insert(
        result.languageMetadata.interfaces.end(),
        build.nativeInterfaces.begin(),
        build.nativeInterfaces.end());
    std::stable_sort(
        result.languageMetadata.interfaces.begin(),
        result.languageMetadata.interfaces.end(),
        [](const auto& left, const auto& right) {
            return left.typeName < right.typeName;
        });
    result.diagnostics.append(build.diagnostics);
    if (result.diagnostics.hasErrors()) return result;

    const auto nativeModules = api_.nativeModules();
    const auto& nativeBindings = api_.nativeBindingNames();
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> modules;
    modules.reserve(build.modules.size());
    for (const auto& mirModule : build.modules) {
        auto module = lowerer.lower(mirModule);
        if (nativeModules.find(module.name) != nativeModules.end()) {
            module.functions.erase(
                std::remove_if(
                    module.functions.begin(),
                    module.functions.end(),
                    [&](const bytecode::Function& function) {
                        return nativeBindings.find(
                            module.name + "::" + function.name) != nativeBindings.end();
                    }),
                module.functions.end());
        }
        modules.push_back(std::move(module));
    }

    runtime::RuntimeError linkError;
    auto image = runtime::ProgramImage::link(modules, linkError);
    if (!image) {
        result.diagnostics.report("RS7001", linkError.message, {});
        return result;
    }

    result.modules = std::move(modules);
    result.program.program_ =
        std::make_shared<runtime::ProgramImage>(std::move(*image));
    result.program.bindings_ = api_.bindings();
    result.program.heap_ = api_.heap();
    result.program.nativeHandles_ = api_.nativeHandles();
    result.program.languageMetadata_ = result.languageMetadata;
    return result;
}

} // namespace realscript::game
