#include "realscript/bindgen/RsBindParser.h"
#include "realscript/Version.h"

#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::bindgen {
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
    const detail::HostApiState& state,
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

HostApi::HostApi()
    : state_(std::make_shared<detail::HostApiState>()) {}

HostApi::HostApi(
    std::shared_ptr<runtime::ManagedHeap> heap,
    std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles)
    : state_(std::make_shared<detail::HostApiState>()) {
    if (heap) state_->heap = std::move(heap);
    if (nativeHandles) state_->nativeHandles = std::move(nativeHandles);
}

std::vector<compiler::SourceFile> HostApi::generatedSources() const {
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

std::vector<compiler::SourceFile> HostApi::exportDeclarationStubs() const {
    std::vector<compiler::SourceFile> result;
    result.reserve(state_->modules.size());
    for (const auto& [moduleName, module] : state_->modules) {
        std::ostringstream out;
        out << "// Auto-generated RealScript Host API Stub\n";
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

std::string HostApi::exportProjectConfigJson(
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

std::set<std::string> HostApi::nativeModules() const {
    std::set<std::string> result;
    for (const auto& entry : state_->modules) result.insert(entry.first);
    return result;
}

const std::unordered_set<std::string>& HostApi::nativeBindingNames() const noexcept {
    return state_->bindingNames;
}

bool HostApi::valid() const noexcept { return state_->errors.empty(); }
const std::vector<std::string>& HostApi::errors() const noexcept { return state_->errors; }
std::shared_ptr<runtime::BindingRegistry> HostApi::bindings() const noexcept {
    return state_->bindings;
}
std::shared_ptr<runtime::ManagedHeap> HostApi::heap() const noexcept {
    return state_->heap;
}
std::shared_ptr<runtime::NativeHandleRegistry> HostApi::nativeHandles() const noexcept {
    return state_->nativeHandles;
}

HostApi::HeaderBindingGenerationResult HostApi::generateBindingsFromCppHeader(
    std::string_view headerContent,
    const std::string& defaultModuleName,
    const std::unordered_map<std::string, std::string>& predefinedMacros) {
    return rsbind_detail::generateFromParsedHeader(
        headerContent,
        defaultModuleName,
        predefinedMacros);
}

} // namespace realscript::bindgen
