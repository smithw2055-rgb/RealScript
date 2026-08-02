#include "realscript/game/GameScripting.h"

#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace realscript::game {
namespace {

std::string sanitizeFileName(std::string value) {
    for (auto& character : value) {
        if (!(character >= 'a' && character <= 'z') &&
            !(character >= 'A' && character <= 'Z') &&
            !(character >= '0' && character <= '9') &&
            character != '_' && character != '-') {
            character = '_';
        }
    }
    return value;
}

std::string dummyReturn(const detail::ScriptTypeRef& type) {
    switch (type.category) {
    case detail::ScriptTypeCategory::Void: return "return;";
    case detail::ScriptTypeCategory::Bool: return "return false;";
    case detail::ScriptTypeCategory::Int:
    case detail::ScriptTypeCategory::Long: return "return 0;";
    case detail::ScriptTypeCategory::Double: return "return 0.0;";
    case detail::ScriptTypeCategory::String: return "return \"\";";
    case detail::ScriptTypeCategory::NativeObject: return "return null;";
    }
    return "return;";
}

void writeParameters(
    std::ostringstream& out,
    const std::vector<detail::ParameterDeclaration>& parameters) {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index != 0) out << ", ";
        out << parameters[index].type.sourceName << ' ' << parameters[index].name;
    }
}

void writeFunction(
    std::ostringstream& out,
    const detail::FunctionDeclaration& function,
    const char* indent) {
    out << indent << function.returnType.sourceName << ' ' << function.name << '(';
    writeParameters(out, function.parameters);
    out << ") { "
        << (function.body.empty() ? dummyReturn(function.returnType) : function.body)
        << " }\n";
}

void collectLanguageMetadata(
    GameLanguageMetadata& metadata,
    const std::vector<compiler::LanguageExpansionResult>& expansions) {
    for (const auto& expansion : expansions) {
        metadata.attributes.insert(
            metadata.attributes.end(),
            expansion.attributes.begin(),
            expansion.attributes.end());
        metadata.interfaces.insert(
            metadata.interfaces.end(),
            expansion.interfaces.begin(),
            expansion.interfaces.end());
        metadata.genericInstantiations.insert(
            metadata.genericInstantiations.end(),
            expansion.genericInstantiations.begin(),
            expansion.genericInstantiations.end());
    }

    std::stable_sort(
        metadata.attributes.begin(), metadata.attributes.end(),
        [](const auto& left, const auto& right) {
            if (left.target != right.target) return left.target < right.target;
            return left.name < right.name;
        });
    std::sort(
        metadata.interfaces.begin(), metadata.interfaces.end(),
        [](const auto& left, const auto& right) {
            return left.typeName < right.typeName;
        });
    std::sort(
        metadata.genericInstantiations.begin(),
        metadata.genericInstantiations.end(),
        [](const auto& left, const auto& right) {
            if (left.generatedName != right.generatedName) {
                return left.generatedName < right.generatedName;
            }
            return left.genericName < right.genericName;
        });
    metadata.genericInstantiations.erase(
        std::unique(
            metadata.genericInstantiations.begin(),
            metadata.genericInstantiations.end(),
            [](const auto& left, const auto& right) {
                return left.generatedName == right.generatedName &&
                    left.genericName == right.genericName &&
                    left.arguments == right.arguments;
            }),
        metadata.genericInstantiations.end());
}

} // namespace

GameApi::GameApi()
    : state_(std::make_shared<detail::GameApiState>()) {}

GameApi::GameApi(
    std::shared_ptr<runtime::ManagedHeap> heap,
    std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles)
    : state_(std::make_shared<detail::GameApiState>()) {
    if (heap) state_->heap = std::move(heap);
    if (nativeHandles) state_->nativeHandles = std::move(nativeHandles);
}

std::vector<compiler::SourceFile> GameApi::generatedSources() const {
    std::vector<compiler::SourceFile> result;
    result.reserve(state_->modules.size());
    for (const auto& [moduleName, module] : state_->modules) {
        std::ostringstream out;
        out << "module " << moduleName << ";\n";
        for (const auto& imported : module.imports) {
            out << "import " << imported << ";\n";
        }
        if (!module.imports.empty()) out << '\n';

        for (const auto typeIndex : module.nativeTypes) {
            if (typeIndex >= state_->nativeTypes.size()) continue;
            const auto& type = state_->nativeTypes[typeIndex];
            out << "class " << type.name << "\n{\n";
            out << "    handle __native;\n";
            for (const auto& method : type.methods) {
                writeFunction(out, method, "    ");
            }
            for (const auto& property : type.properties) {
                out << "    " << property.type.sourceName << ' ' << property.name << "\n    {\n";
                if (property.getter) {
                    out << "        get { "
                        << (property.getterBody.empty()
                            ? dummyReturn(property.type)
                            : property.getterBody)
                        << " }\n";
                }
                if (property.setter) {
                    out << "        set { "
                        << (property.setterBody.empty()
                            ? std::string{"return;"}
                            : property.setterBody)
                        << " }\n";
                }
                out << "    }\n";
            }
            out << "}\n\n";
        }
        for (const auto& function : module.functions) {
            writeFunction(out, function, "");
        }

        result.push_back({
            "$generated/" + sanitizeFileName(moduleName) + ".native.rs",
            out.str(),
        });
    }
    return result;
}

std::set<std::string> GameApi::nativeModules() const {
    std::set<std::string> result;
    for (const auto& entry : state_->modules) result.insert(entry.first);
    return result;
}

const std::unordered_set<std::string>& GameApi::nativeBindingNames() const noexcept {
    return state_->bindingNames;
}

bool GameApi::valid() const noexcept { return state_->errors.empty(); }
const std::vector<std::string>& GameApi::errors() const noexcept { return state_->errors; }
std::shared_ptr<runtime::BindingRegistry> GameApi::bindings() const noexcept {
    return state_->bindings;
}
std::shared_ptr<runtime::ManagedHeap> GameApi::heap() const noexcept {
    return state_->heap;
}
std::shared_ptr<runtime::NativeHandleRegistry> GameApi::nativeHandles() const noexcept {
    return state_->nativeHandles;
}

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
    collectLanguageMetadata(
        result.languageMetadata,
        compilation.languageExpansions());

    auto build = compilation.build();
    result.languageMetadata.attributes.insert(
        result.languageMetadata.attributes.end(),
        build.nativeAttributes.begin(),
        build.nativeAttributes.end());
    std::stable_sort(
        result.languageMetadata.attributes.begin(),
        result.languageMetadata.attributes.end(),
        [](const auto& left, const auto& right) {
            if (left.target != right.target) return left.target < right.target;
            if (left.name != right.name) return left.name < right.name;
            if (left.sourceName != right.sourceName) {
                return left.sourceName < right.sourceName;
            }
            return left.offset < right.offset;
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
