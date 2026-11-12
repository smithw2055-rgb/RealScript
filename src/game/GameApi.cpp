#include "realscript/game/RsBindParser.h"
#include "realscript/Version.h"
#include "realscript/game/GameScripting.h"

#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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
    case detail::ScriptTypeCategory::Double:
        return type.sourceName == "float" ? "return (float)0.0;" : "return 0.0;";
    case detail::ScriptTypeCategory::String: return "return \"\";";
    case detail::ScriptTypeCategory::Struct:
        if (type.moduleName == "RealScript.Async") {
            if (type.sourceName == "CancellationToken") {
                return "return CancellationToken.None();";
            }
            if (type.sourceName == "Awaitable") {
                return "return Awaitable.Completed();";
            }
            if (type.sourceName.rfind("Awaitable<", 0) == 0) {
                return "return " + type.sourceName + ".FromHandle(0L);";
            }
        }
        return "return new " + type.sourceName + "();";
    case detail::ScriptTypeCategory::NativeObject:
    case detail::ScriptTypeCategory::Array: return "return null;";
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
    if (function.determinism == runtime::BindingDeterminism::NonDeterministic) {
        out << indent << "[NonDeterministic]\n";
    }
    if (!function.requiredCapability.empty()) {
        out << indent << "[Capability(\"" << function.requiredCapability << "\")]\n";
    }
    out << indent << (function.isStatic ? "public static " : "") << function.returnType.sourceName << ' ' << function.name << '(';
    writeParameters(out, function.parameters);
    out << ") { "
        << (function.body.empty() ? dummyReturn(function.returnType) : function.body)
        << " }\n";
}

std::vector<std::size_t> orderModuleNativeTypesBaseFirst(
    const detail::GameApiState& state,
    const std::vector<std::size_t>& nativeTypes) {
    std::unordered_set<std::size_t> inModule(nativeTypes.begin(), nativeTypes.end());
    std::unordered_set<std::size_t> visited;
    std::vector<std::size_t> ordered;
    ordered.reserve(nativeTypes.size());
    const auto visit = [&](std::size_t idx, const auto& self) -> void {
        if (visited.count(idx) > 0 || idx >= state.nativeTypes.size()) return;
        visited.insert(idx);
        const auto& decl = state.nativeTypes[idx];
        if (decl.baseCppType.has_value()) {
            const auto baseIt = state.nativeTypeByCppType.find(*decl.baseCppType);
            if (baseIt != state.nativeTypeByCppType.end() && inModule.count(baseIt->second) > 0) {
                self(baseIt->second, self);
            }
        }
        ordered.push_back(idx);
    };
    for (const auto idx : nativeTypes) {
        visit(idx, visit);
    }
    return ordered;
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
        std::set<std::string> emittedImports = module.imports;
        for (const auto typeIndex : orderModuleNativeTypesBaseFirst(*state_, module.nativeTypes)) {
            if (typeIndex >= state_->nativeTypes.size()) continue;
            const auto& type = state_->nativeTypes[typeIndex];
            if (!type.baseCppType.has_value()) continue;
            const auto baseIt = state_->nativeTypeByCppType.find(*type.baseCppType);
            if (baseIt != state_->nativeTypeByCppType.end()) {
                const auto& baseDecl = state_->nativeTypes[baseIt->second];
                if (!baseDecl.moduleName.empty() && baseDecl.moduleName != moduleName) {
                    emittedImports.insert(baseDecl.moduleName);
                }
            }
        }
        for (const auto& imported : emittedImports) {
            out << "import " << imported << ";\n";
        }
        if (!emittedImports.empty()) out << '\n';

        for (const auto structIndex : module.nativeStructs) {
            if (structIndex >= state_->nativeStructs.size()) continue;
            const auto& st = state_->nativeStructs[structIndex];
            out << "public struct " << st.name << "\n{\n";
            for (const auto& fld : st.fields) {
                out << "    public " << fld.type.sourceName << " " << fld.name << ";\n";
            }
            out << "}\n\n";
        }
        for (const auto typeIndex : orderModuleNativeTypesBaseFirst(*state_, module.nativeTypes)) {
            if (typeIndex >= state_->nativeTypes.size()) continue;
            const auto& type = state_->nativeTypes[typeIndex];
            std::string baseClause;
            if (type.baseCppType.has_value()) {
                const auto baseIt = state_->nativeTypeByCppType.find(*type.baseCppType);
                if (baseIt != state_->nativeTypeByCppType.end()) {
                    baseClause = " : " + state_->nativeTypes[baseIt->second].name;
                }
            }
            out << "class " << type.name << baseClause << "\n{\n";
            if (baseClause.empty()) {
                out << "    handle __native;\n";
            }
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
            "$generated/" + sanitizeFileName(moduleName) + ".native.rscript",
            out.str(),
        });
    }
    return result;
}

std::vector<compiler::SourceFile> GameApi::exportDeclarationStubs() const {
    std::vector<compiler::SourceFile> result;
    result.reserve(state_->modules.size());
    for (const auto& [moduleName, module] : state_->modules) {
        std::ostringstream out;
        out << "// Auto-generated RealScript Engine API Stub\n";
        out << "module " << moduleName << ";\n";
        std::set<std::string> emittedImports = module.imports;
        for (const auto typeIndex : module.nativeTypes) {
            if (typeIndex >= state_->nativeTypes.size()) continue;
            const auto& type = state_->nativeTypes[typeIndex];
            if (!type.baseCppType.has_value()) continue;
            const auto baseIt = state_->nativeTypeByCppType.find(*type.baseCppType);
            if (baseIt != state_->nativeTypeByCppType.end()) {
                const auto& baseDecl = state_->nativeTypes[baseIt->second];
                if (!baseDecl.moduleName.empty() && baseDecl.moduleName != moduleName) {
                    emittedImports.insert(baseDecl.moduleName);
                }
            }
        }
        for (const auto& imported : emittedImports) {
            out << "import " << imported << ";\n";
        }
        if (!emittedImports.empty()) out << '\n';

        for (const auto structIndex : module.nativeStructs) {
            if (structIndex >= state_->nativeStructs.size()) continue;
            const auto& st = state_->nativeStructs[structIndex];
            out << "public struct " << st.name << "\n{\n";
            for (const auto& fld : st.fields) {
                out << "    public " << fld.type.sourceName << " " << fld.name << ";\n";
            }
            out << "}\n\n";
        }
        for (const auto typeIndex : orderModuleNativeTypesBaseFirst(*state_, module.nativeTypes)) {
            if (typeIndex >= state_->nativeTypes.size()) continue;
            const auto& type = state_->nativeTypes[typeIndex];
            std::string baseClause;
            if (type.baseCppType.has_value()) {
                const auto baseIt = state_->nativeTypeByCppType.find(*type.baseCppType);
                if (baseIt != state_->nativeTypeByCppType.end()) {
                    baseClause = " : " + state_->nativeTypes[baseIt->second].name;
                }
            }
            out << "class " << type.name << baseClause << "\n{\n";
            if (baseClause.empty()) {
                out << "    handle __native;\n";
            }
            for (const auto& method : type.methods) {
                out << "    " << (method.isStatic ? "public static " : "") << method.returnType.sourceName << ' ' << method.name << '(';
                writeParameters(out, method.parameters);
                out << ") { " << dummyReturn(method.returnType) << " }\n";
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
            if (function.name.rfind("__rs_native_", 0) == 0) continue;
            out << function.returnType.sourceName << ' ' << function.name << '(';
            writeParameters(out, function.parameters);
            out << ") { " << dummyReturn(function.returnType) << " }\n";
        }

        result.push_back({
            sanitizeFileName(moduleName) + ".d.rscript",
            out.str(),
        });
    }
    return result;
}

std::string GameApi::exportProjectConfigJson(
    const std::string& projectName,
    const std::vector<std::string>& sourcePaths) const {
    const auto escapeJson = [](const std::string& text) {
        std::string out;
        out.reserve(text.size() + 8);
        for (const char ch : text) {
            switch (ch) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
            }
        }
        return out;
    };

    const auto stubs = exportDeclarationStubs();
    std::ostringstream out;
    out << "{\n";
    out << "  \"projectName\": \"" << escapeJson(projectName) << "\",\n";
    out << "  \"sdkVersion\": \"" << kVersionString << "\",\n";
    out << "  \"nativeModules\": [";
    bool firstModule = true;
    for (const auto& [moduleName, module] : state_->modules) {
        (void)module;
        if (!firstModule) out << ", ";
        firstModule = false;
        out << "\"" << escapeJson(moduleName) << "\"";
    }
    out << "],\n";
    out << "  \"sources\": [";
    for (std::size_t i = 0; i < sourcePaths.size(); ++i) {
        if (i != 0) out << ", ";
        out << "\"" << escapeJson(sourcePaths[i]) << "\"";
    }
    out << "],\n";
    out << "  \"ambientStubs\": [\n";
    for (std::size_t i = 0; i < stubs.size(); ++i) {
        out << "    {\"path\": \"" << escapeJson(stubs[i].path)
            << "\", \"content\": \"" << escapeJson(stubs[i].content) << "\"}";
        if (i + 1 < stubs.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}";
    return out.str();
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

GameApi::HeaderBindingGenerationResult GameApi::generateBindingsFromCppHeader(
    std::string_view headerContent,
    const std::string& defaultModuleName,
    const std::unordered_map<std::string, std::string>& predefinedMacros) {
    return rsbind_detail::generateFromParsedHeader(
        headerContent,
        defaultModuleName,
        predefinedMacros);
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
