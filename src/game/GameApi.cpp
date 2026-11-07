#include "realscript/Version.h"
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
    case detail::ScriptTypeCategory::Struct:
        return "return new " + type.sourceName + "();";
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

std::vector<compiler::SourceFile> GameApi::exportDeclarationStubs() const {
    std::vector<compiler::SourceFile> result;
    result.reserve(state_->modules.size());
    for (const auto& [moduleName, module] : state_->modules) {
        std::ostringstream out;
        out << "// Auto-generated RealScript Engine API Stub\n";
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
                out << "    " << method.returnType.sourceName << ' ' << method.name << '(';
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
            sanitizeFileName(moduleName) + ".d.rs",
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
    const std::string& defaultModuleName) {
    HeaderBindingGenerationResult res;
    std::ostringstream cppOut;
    std::ostringstream rsOut;

    const auto trim = [](std::string_view s) -> std::string {
        std::size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
        std::size_t e = s.size();
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
        return std::string(s.substr(b, e - b));
    };

    const auto mapCppTypeToRs = [&](std::string cppType) -> std::string {
        if (cppType.rfind("const ", 0) == 0) {
            cppType = trim(cppType.substr(6));
        }
        while (!cppType.empty() && (cppType.back() == '&' || cppType.back() == '*' || cppType.back() == ' ')) {
            cppType.pop_back();
        }
        if (cppType == "int" || cppType == "int32_t" || cppType == "std::int32_t") return "int";
        if (cppType == "int64_t" || cppType == "std::int64_t" || cppType == "long") return "long";
        if (cppType == "float") return "float";
        if (cppType == "double") return "double";
        if (cppType == "bool") return "bool";
        if (cppType == "void") return "void";
        if (cppType.find("string") != std::string::npos) return "string";
        if (cppType.find("Vec2") != std::string::npos) return "Vec2";
        if (cppType.find("Vec3") != std::string::npos) return "Vec3";
        if (cppType.find("Quat") != std::string::npos) return "Quat";
        if (cppType.find("Fixed") != std::string::npos) return "Fixed";
        return cppType;
    };

    cppOut << "// Auto-generated RealScript C++ Binding Registration\n";
    cppOut << "inline void registerGeneratedHeaderBindings(realscript::game::GameApi& api) {\n";

    rsOut << "// Auto-generated RealScript Stub from C++ Header\n";
    rsOut << "module " << defaultModuleName << ";\n\n";

    std::string currentClass;
    std::string currentClassModule = defaultModuleName;
    bool pendingClassExport = false;
    bool pendingMethodExport = false;
    bool pendingFuncExport = false;

    std::size_t pos = 0;
    while (pos < headerContent.size()) {
        std::size_t endLine = headerContent.find('\n', pos);
        if (endLine == std::string_view::npos) endLine = headerContent.size();
        std::string line = trim(headerContent.substr(pos, endLine - pos));
        pos = endLine + 1;
        if (line.empty()) continue;

        if (line.find("RS_EXPORT_CLASS") != std::string::npos) {
            pendingClassExport = true;
            const auto openP = line.find('(');
            const auto closeP = line.find(')');
            if (openP != std::string::npos && closeP != std::string::npos && closeP > openP + 1) {
                currentClassModule = trim(line.substr(openP + 1, closeP - openP - 1));
                if (currentClassModule.size() >= 2 && currentClassModule.front() == '"') {
                    currentClassModule = currentClassModule.substr(1, currentClassModule.size() - 2);
                }
            } else {
                currentClassModule = defaultModuleName;
            }
            continue;
        }
        if (line.find("RS_EXPORT_METHOD") != std::string::npos) {
            pendingMethodExport = true;
            continue;
        }
        if (line.find("RS_EXPORT_FUNCTION") != std::string::npos) {
            pendingFuncExport = true;
            continue;
        }

        if (pendingClassExport && (line.rfind("class ", 0) == 0 || line.rfind("struct ", 0) == 0)) {
            pendingClassExport = false;
            const auto spacePos = line.find(' ');
            std::string namePart = trim(line.substr(spacePos + 1));
            const auto endName = namePart.find_first_of(" :{");
            currentClass = endName == std::string::npos ? namePart : namePart.substr(0, endName);
            ++res.classesExported;
            cppOut << "    auto builder_" << currentClass << " = api.type<" << currentClass
                   << ">(\"" << currentClassModule << "\", \"" << currentClass << "\");\n";
            rsOut << "class " << currentClass << " {\n    handle __native;\n";
            continue;
        }

        if (!currentClass.empty() && line.rfind("};", 0) == 0) {
            currentClass.clear();
            rsOut << "}\n\n";
            continue;
        }

        if ((pendingMethodExport || pendingFuncExport) && line.find('(') != std::string::npos) {
            const bool isMethod = pendingMethodExport && !currentClass.empty();
            pendingMethodExport = false;
            pendingFuncExport = false;

            const auto openParen = line.find('(');
            const auto closeParen = line.find(')', openParen);
            if (closeParen == std::string::npos) continue;
            const std::string prefix = trim(line.substr(0, openParen));
            const auto lastSpace = prefix.rfind(' ');
            if (lastSpace == std::string::npos) continue;
            const std::string retCpp = trim(prefix.substr(0, lastSpace));
            const std::string fnName = trim(prefix.substr(lastSpace + 1));
            const std::string paramsStr = trim(line.substr(openParen + 1, closeParen - openParen - 1));

            std::vector<std::pair<std::string, std::string>> rsParams;
            if (!paramsStr.empty() && paramsStr != "void") {
                std::size_t pStart = 0;
                int idx = 0;
                while (pStart < paramsStr.size()) {
                    std::size_t comma = paramsStr.find(',', pStart);
                    if (comma == std::string::npos) comma = paramsStr.size();
                    std::string pItem = trim(paramsStr.substr(pStart, comma - pStart));
                    pStart = comma + 1;
                    const auto pSpace = pItem.rfind(' ');
                    if (pSpace != std::string::npos) {
                        std::string pName = trim(pItem.substr(pSpace + 1));
                        while (!pName.empty() && (pName.front() == '&' || pName.front() == '*')) {
                            pName.erase(pName.begin());
                        }
                        rsParams.push_back({
                            mapCppTypeToRs(trim(pItem.substr(0, pSpace))),
                            std::move(pName)});
                    } else {
                        rsParams.push_back({mapCppTypeToRs(pItem), "arg" + std::to_string(idx)});
                    }
                    ++idx;
                }
            }

            const std::string rsRet = mapCppTypeToRs(retCpp);
            if (isMethod) {
                ++res.methodsExported;
                cppOut << "    builder_" << currentClass << ".method<&" << currentClass
                       << "::" << fnName << ">(\"" << fnName << "\");\n";
                rsOut << "    " << rsRet << " " << fnName << "(";
            } else {
                ++res.functionsExported;
                cppOut << "    api.function(\"" << defaultModuleName << "\", \""
                       << fnName << "\", &" << fnName << ");\n";
                rsOut << rsRet << " " << fnName << "(";
            }
            for (std::size_t i = 0; i < rsParams.size(); ++i) {
                if (i != 0) rsOut << ", ";
                rsOut << rsParams[i].first << " " << rsParams[i].second;
            }
            rsOut << ") { " << (rsRet == "void" ? "return;" : (rsRet == "bool" ? "return false;" : "return 0;")) << " }\n";
        }
    }

    cppOut << "}\n";
    res.cppRegistrationSource = cppOut.str();
    res.scriptDeclarationStub = rsOut.str();
    return res;
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
