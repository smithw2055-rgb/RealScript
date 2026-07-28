#include "realscript/game/GameScripting.h"

#include "realscript/diagnostics/Diagnostic.h"

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
    out << ") { " << dummyReturn(function.returnType) << " }\n";
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
                    out << "        get { " << dummyReturn(property.type) << " }\n";
                }
                if (property.setter) {
                    out << "        set { return; }\n";
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

    auto build = compilation.build();
    result.diagnostics.append(build.diagnostics);
    if (result.diagnostics.hasErrors()) return result;

    const auto nativeModules = api_.nativeModules();
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> modules;
    modules.reserve(build.modules.size());
    for (const auto& mirModule : build.modules) {
        auto module = lowerer.lower(mirModule);
        if (nativeModules.find(module.name) != nativeModules.end()) {
            // Generated declarations describe the host API to the compiler. Their
            // bodies are deliberately removed so calls cross the native binding
            // boundary rather than executing the placeholder implementations.
            module.functions.clear();
            module.functionReferences.clear();
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
    return result;
}

} // namespace realscript::game
