#include "realscript/game/GameScripting.h"

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

} // namespace realscript::game
