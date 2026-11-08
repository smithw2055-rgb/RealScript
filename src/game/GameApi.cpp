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
    if (function.determinism == runtime::BindingDeterminism::NonDeterministic) {
        out << indent << "[NonDeterministic]\n";
    }
    if (!function.requiredCapability.empty()) {
        out << indent << "[Capability(\"" << function.requiredCapability << "\")]\n";
    }
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
    std::ostringstream cppBodyOut;
    std::ostringstream cppHelpersOut;
    std::ostringstream rsBodyOut;
    bool usesMathTypes = false;

    const auto trim = [](std::string_view s) -> std::string {
        std::size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
        std::size_t e = s.size();
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
        return std::string(s.substr(b, e - b));
    };

    const auto stripQuotes = [&](std::string s) -> std::string {
        s = trim(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            return s.substr(1, s.size() - 2);
        }
        return s;
    };

    const auto consumeMacro = [&](std::string& line, const std::string& macroName, std::vector<std::string>& outArgs) -> bool {
        outArgs.clear();
        std::size_t mPos = 0;
        while (true) {
            mPos = line.find(macroName, mPos);
            if (mPos == std::string::npos) return false;
            const bool validStart = (mPos == 0) ||
                !(std::isalnum(static_cast<unsigned char>(line[mPos - 1])) || line[mPos - 1] == '_');
            const std::size_t afterMacro = mPos + macroName.size();
            const bool validEnd = (afterMacro >= line.size()) ||
                !(std::isalnum(static_cast<unsigned char>(line[afterMacro])) || line[afterMacro] == '_');
            if (validStart && validEnd) break;
            mPos = afterMacro;
        }
        std::size_t cursor = mPos + macroName.size();
        while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) ++cursor;
        if (cursor < line.size() && line[cursor] == '(') {
            const auto openP = cursor;
            int depth = 0;
            std::size_t closeP = std::string::npos;
            for (std::size_t i = openP; i < line.size(); ++i) {
                if (line[i] == '(') ++depth;
                else if (line[i] == ')') {
                    --depth;
                    if (depth == 0) {
                        closeP = i;
                        break;
                    }
                }
            }
            if (closeP != std::string::npos) {
                if (closeP > openP + 1) {
                    const std::string raw = trim(line.substr(openP + 1, closeP - openP - 1));
                    std::size_t start = 0;
                    while (start < raw.size()) {
                        std::size_t comma = raw.find(',', start);
                        if (comma == std::string::npos) comma = raw.size();
                        outArgs.push_back(stripQuotes(raw.substr(start, comma - start)));
                        start = comma + 1;
                    }
                }
                cursor = closeP + 1;
            }
        }
        line = trim(line.substr(0, mPos) + " " + line.substr(cursor));
        return true;
    };

    // Strip C and C++ comments while preserving newlines
    std::string cleaned;
    cleaned.reserve(headerContent.size());
    for (std::size_t i = 0; i < headerContent.size();) {
        if (i + 1 < headerContent.size() && headerContent[i] == '/' && headerContent[i + 1] == '/') {
            i += 2;
            while (i < headerContent.size() && headerContent[i] != '\n') ++i;
        } else if (i + 1 < headerContent.size() && headerContent[i] == '/' && headerContent[i + 1] == '*') {
            i += 2;
            while (i + 1 < headerContent.size() && !(headerContent[i] == '*' && headerContent[i + 1] == '/')) {
                if (headerContent[i] == '\n') cleaned.push_back('\n');
                ++i;
            }
            if (i + 1 < headerContent.size()) i += 2;
        } else {
            cleaned.push_back(headerContent[i]);
            ++i;
        }
    }

    const auto mapCppTypeToRs = [&](std::string cppType) -> std::string {
        cppType = trim(cppType);
        for (const char* kw : {"virtual ", "inline ", "constexpr ", "static ", "explicit ", "const "}) {
            const std::size_t kwLen = std::strlen(kw);
            if (cppType.rfind(kw, 0) == 0) cppType = trim(cppType.substr(kwLen));
        }
        if (cppType.rfind("std::shared_ptr<", 0) == 0 && cppType.back() == '>') {
            cppType = trim(cppType.substr(16, cppType.size() - 17));
        } else if (cppType.rfind("shared_ptr<", 0) == 0 && cppType.back() == '>') {
            cppType = trim(cppType.substr(11, cppType.size() - 12));
        }
        while (!cppType.empty() && (cppType.back() == '&' || cppType.back() == '*' || cppType.back() == ' ')) {
            cppType.pop_back();
        }
        if (cppType.size() > 6 && cppType.compare(cppType.size() - 6, 6, " const") == 0) {
            cppType = trim(cppType.substr(0, cppType.size() - 6));
        }
        if (cppType == "int" || cppType == "int32_t" || cppType == "std::int32_t") return "int";
        if (cppType == "int64_t" || cppType == "std::int64_t" || cppType == "long" || cppType == "long long") return "long";
        if (cppType == "uint32_t" || cppType == "std::uint32_t" || cppType == "unsigned int") return "uint";
        if (cppType == "uint64_t" || cppType == "std::uint64_t" || cppType == "unsigned long long") return "ulong";
        if (cppType == "uint8_t" || cppType == "std::uint8_t" || cppType == "unsigned char") return "byte";
        if (cppType == "int8_t" || cppType == "std::int8_t" || cppType == "signed char") return "sbyte";
        if (cppType == "int16_t" || cppType == "std::int16_t" || cppType == "short") return "short";
        if (cppType == "uint16_t" || cppType == "std::uint16_t" || cppType == "unsigned short") return "ushort";
        if (cppType == "float") return "float";
        if (cppType == "double") return "double";
        if (cppType == "bool") return "bool";
        if (cppType == "void") return "void";
        if (cppType.find("string") != std::string::npos) return "string";
        if (cppType.find("Vec2") != std::string::npos) { usesMathTypes = true; return "Vec2"; }
        if (cppType.find("Vec3") != std::string::npos) { usesMathTypes = true; return "Vec3"; }
        if (cppType.find("Quat") != std::string::npos) { usesMathTypes = true; return "Quat"; }
        if (cppType.find("Fixed16") != std::string::npos) { usesMathTypes = true; return "Fixed16"; }
        if (cppType.find("Fixed") != std::string::npos) { usesMathTypes = true; return "Fixed"; }
        const auto nsPos = cppType.rfind("::");
        if (nsPos != std::string::npos) {
            return cppType.substr(nsPos + 2);
        }
        return cppType;
    };

    const auto mapRsTypeToNativeFieldKind = [](const std::string& rsType) -> std::string {
        if (rsType == "int" || rsType == "uint" || rsType == "short" || rsType == "ushort" || rsType == "byte" || rsType == "sbyte") {
            return "realscript::runtime::NativeFieldKind::Int32";
        }
        if (rsType == "long" || rsType == "ulong") return "realscript::runtime::NativeFieldKind::Int64";
        if (rsType == "float") return "realscript::runtime::NativeFieldKind::Float32";
        if (rsType == "double") return "realscript::runtime::NativeFieldKind::Float64";
        if (rsType == "bool") return "realscript::runtime::NativeFieldKind::Bool";
        if (rsType == "Fixed") return "realscript::runtime::NativeFieldKind::Fixed";
        if (rsType == "Fixed16") return "realscript::runtime::NativeFieldKind::Fixed16";
        if (rsType == "Vec2") return "realscript::runtime::NativeFieldKind::Vec2";
        if (rsType == "Vec3") return "realscript::runtime::NativeFieldKind::Vec3";
        if (rsType == "Quat") return "realscript::runtime::NativeFieldKind::Quat";
        return "realscript::runtime::NativeFieldKind::Int32";
    };

    std::unordered_set<std::string> exportedStructs;
    std::unordered_set<std::string> exportedClasses;
    std::unordered_map<std::string, std::string> exportedEnumFirstMember;

    const auto dummyRsReturn = [&](const std::string& rsRet) -> std::string {
        if (rsRet == "void") return "return;";
        if (rsRet == "bool") return "return false;";
        if (rsRet == "string") return "return \"\";";
        if (rsRet == "float") return "return (float)0.0;";
        if (rsRet == "double") return "return 0.0;";
        if (rsRet == "int" || rsRet == "long") return "return 0;";
        if (rsRet == "uint" || rsRet == "ulong" || rsRet == "byte" ||
            rsRet == "sbyte" || rsRet == "short" || rsRet == "ushort") {
            return "return (" + rsRet + ")0;";
        }
        if (rsRet == "Vec2" || rsRet == "Vec3" || rsRet == "Quat" ||
            rsRet == "Fixed" || rsRet == "Fixed16" ||
            exportedStructs.count(rsRet) > 0) {
            return "return new " + rsRet + "();";
        }
        if (const auto it = exportedEnumFirstMember.find(rsRet);
            it != exportedEnumFirstMember.end() && !it->second.empty()) {
            return "return " + rsRet + "." + it->second + ";";
        }
        if (exportedClasses.count(rsRet) > 0) {
            return "return null;";
        }
        return "return null;";
    };

    struct NamespaceFrame {
        std::string name;
        int depthBeforeOpen = 0;
    };
    std::vector<NamespaceFrame> namespaceStack;
    int braceDepth = 0;
    const auto currentNamespacePrefix = [&]() -> std::string {
        std::string out;
        for (const auto& ns : namespaceStack) {
            if (!ns.name.empty()) out += ns.name + "::";
        }
        return out;
    };
    const auto updateBraceDepth = [&](std::string_view textLine) {
        for (const char ch : textLine) {
            if (ch == '{') ++braceDepth;
            else if (ch == '}') {
                --braceDepth;
                while (!namespaceStack.empty() && braceDepth <= namespaceStack.back().depthBeforeOpen) {
                    namespaceStack.pop_back();
                }
            }
        }
    };

    std::string currentClass;
    std::string currentClassQualified;
    std::string currentClassModule = defaultModuleName;
    int currentClassBraceDepth = 0;
    bool currentIsEcsStruct = false;

    bool pendingClassExport = false;
    bool pendingEcsStructExport = false;
    bool pendingEnumExport = false;
    bool pendingMethodExport = false;
    bool pendingFuncExport = false;
    bool pendingNonDeterministic = false;
    std::string pendingCapability;
    std::string pendingFuncModule = defaultModuleName;

    struct PendingProperty {
        std::string propName;
        std::string getterMethod;
        std::string setterMethod;
        std::string rsType = "int";
    };
    std::vector<PendingProperty> pendingClassProperties;

    struct EcsFieldInfo {
        std::string name;
        std::string rsType;
    };
    std::vector<EcsFieldInfo> currentEcsFields;

    std::size_t pos = 0;
    while (pos < cleaned.size()) {
        std::size_t endLine = cleaned.find('\n', pos);
        if (endLine == std::string::npos) endLine = cleaned.size();
        std::string line = trim(cleaned.substr(pos, endLine - pos));
        pos = endLine + 1;
        if (line.empty()) continue;

        // Track C++ namespace declarations when outside a class/struct
        if (currentClass.empty() && line.rfind("namespace ", 0) == 0 && line.find('{') != std::string::npos) {
            const auto openBrace = line.find('{');
            std::string nsName = trim(line.substr(10, openBrace - 10));
            if (!nsName.empty()) {
                namespaceStack.push_back(NamespaceFrame{nsName, braceDepth});
            }
            updateBraceDepth(line);
            continue;
        }

        std::vector<std::string> macroArgs;
        if (consumeMacro(line, "RS_EXPORT_CLASS", macroArgs)) {
            pendingClassExport = true;
            pendingEcsStructExport = false;
            currentClassModule = (!macroArgs.empty() && !macroArgs[0].empty()) ? macroArgs[0] : defaultModuleName;
            if (line.empty()) continue;
        }
        if (consumeMacro(line, "RS_EXPORT_ECS_COMPONENT", macroArgs) ||
            consumeMacro(line, "RS_EXPORT_STRUCT", macroArgs)) {
            pendingEcsStructExport = true;
            pendingClassExport = false;
            currentClassModule = (!macroArgs.empty() && !macroArgs[0].empty()) ? macroArgs[0] : defaultModuleName;
            if (line.empty()) continue;
        }
        if (consumeMacro(line, "RS_EXPORT_ENUM", macroArgs)) {
            pendingEnumExport = true;
            if (line.empty()) continue;
        }
        if (consumeMacro(line, "RS_EXPORT_PROPERTY", macroArgs)) {
            if (macroArgs.size() >= 2 && !currentClass.empty()) {
                PendingProperty prop;
                prop.propName = macroArgs[0];
                prop.getterMethod = macroArgs[1];
                if (macroArgs.size() >= 3) prop.setterMethod = macroArgs[2];
                if (macroArgs.size() >= 4) prop.rsType = mapCppTypeToRs(macroArgs[3]);
                pendingClassProperties.push_back(std::move(prop));
            }
            if (line.empty()) continue;
        }
        if (consumeMacro(line, "RS_EXPORT_METHOD_EX", macroArgs) ||
            consumeMacro(line, "RS_EXPORT_METHOD", macroArgs)) {
            pendingMethodExport = true;
            pendingNonDeterministic = false;
            pendingCapability.clear();
            for (const auto& a : macroArgs) {
                if (a == "NonDeterministic") pendingNonDeterministic = true;
                else if (!a.empty()) pendingCapability = a;
            }
            if (line.empty()) continue;
        }
        if (consumeMacro(line, "RS_EXPORT_FUNCTION_EX", macroArgs) ||
            consumeMacro(line, "RS_EXPORT_FUNCTION", macroArgs)) {
            pendingFuncExport = true;
            pendingNonDeterministic = false;
            pendingCapability.clear();
            pendingFuncModule = defaultModuleName;
            for (std::size_t i = 0; i < macroArgs.size(); ++i) {
                if (macroArgs[i] == "NonDeterministic") pendingNonDeterministic = true;
                else if (i == 0 && macroArgs[i].find('.') != std::string::npos) pendingFuncModule = macroArgs[i];
                else if (!macroArgs[i].empty()) pendingCapability = macroArgs[i];
            }
            if (line.empty()) continue;
        }

        if (pendingEnumExport && line.rfind("enum ", 0) == 0) {
            pendingEnumExport = false;
            std::string fullEnum = line;
            while (fullEnum.find('}') == std::string::npos && pos < cleaned.size()) {
                std::size_t nextNl = cleaned.find('\n', pos);
                if (nextNl == std::string::npos) nextNl = cleaned.size();
                fullEnum += " " + trim(cleaned.substr(pos, nextNl - pos));
                pos = nextNl + 1;
            }
            const auto openBrace = fullEnum.find('{');
            const auto closeBrace = fullEnum.find('}', openBrace);
            if (openBrace != std::string::npos && closeBrace != std::string::npos) {
                std::string headerPart = trim(fullEnum.substr(0, openBrace));
                if (headerPart.rfind("enum class ", 0) == 0) {
                    headerPart = trim(headerPart.substr(11));
                } else if (headerPart.rfind("enum ", 0) == 0) {
                    headerPart = trim(headerPart.substr(5));
                }
                const auto colonPos = headerPart.find(':');
                std::string enumName = trim(colonPos == std::string::npos ? headerPart : headerPart.substr(0, colonPos));
                std::string bodyPart = trim(fullEnum.substr(openBrace + 1, closeBrace - openBrace - 1));
                rsBodyOut << "enum " << enumName << " {\n";
                std::size_t mStart = 0;
                std::int64_t nextVal = 0;
                std::string firstMember;
                while (mStart < bodyPart.size()) {
                    std::size_t comma = bodyPart.find(',', mStart);
                    if (comma == std::string::npos) comma = bodyPart.size();
                    std::string item = trim(bodyPart.substr(mStart, comma - mStart));
                    mStart = comma + 1;
                    if (item.empty()) continue;
                    const auto eqPos = item.find('=');
                    if (eqPos != std::string::npos) {
                        std::string mName = trim(item.substr(0, eqPos));
                        std::string mVal = trim(item.substr(eqPos + 1));
                        if (firstMember.empty()) firstMember = mName;
                        rsBodyOut << "    " << mName << " = " << mVal << ",\n";
                        try { nextVal = std::stoll(mVal) + 1; } catch (...) { ++nextVal; }
                    } else {
                        if (firstMember.empty()) firstMember = item;
                        rsBodyOut << "    " << item << " = " << nextVal++ << ",\n";
                    }
                }
                rsBodyOut << "}\n\n";
                if (!enumName.empty() && !firstMember.empty()) {
                    exportedEnumFirstMember[enumName] = firstMember;
                }
                ++res.enumsExported;
            }
            updateBraceDepth(fullEnum);
            continue;
        }

        if ((pendingClassExport || pendingEcsStructExport) &&
            (line.rfind("class ", 0) == 0 || line.rfind("struct ", 0) == 0)) {
            currentIsEcsStruct = pendingEcsStructExport;
            pendingClassExport = false;
            pendingEcsStructExport = false;
            currentClassBraceDepth = braceDepth;
            const auto spacePos = line.find(' ');
            std::string namePart = trim(line.substr(spacePos + 1));
            const auto endName = namePart.find_first_of(" :{");
            currentClass = endName == std::string::npos ? namePart : namePart.substr(0, endName);
            currentClassQualified = currentNamespacePrefix() + currentClass;
            pendingClassProperties.clear();
            currentEcsFields.clear();
            if (currentIsEcsStruct) {
                ++res.structsExported;
                exportedStructs.insert(currentClass);
                rsBodyOut << "struct " << currentClass << " {\n";
            } else {
                ++res.classesExported;
                exportedClasses.insert(currentClass);
                cppBodyOut << "    auto builder_" << currentClass << " = api.type<" << currentClassQualified
                           << ">(\"" << currentClassModule << "\", \"" << currentClass << "\");\n";
                rsBodyOut << "class " << currentClass << " {\n    handle __native;\n";
            }
            updateBraceDepth(line);
            continue;
        }

        if (!currentClass.empty() && line.rfind("};", 0) == 0) {
            updateBraceDepth(line);
            if (braceDepth > currentClassBraceDepth) {
                continue;
            }
            if (currentIsEcsStruct) {
                cppHelpersOut << "inline bool bindEcsColumn_" << currentClass << "(\n"
                              << "    realscript::game::ScriptRuntime& runtime,\n"
                              << "    " << currentClassQualified << "* data,\n"
                              << "    std::size_t count,\n"
                              << "    realscript::runtime::RuntimeError& error,\n"
                              << "    bool readOnly = false) {\n"
                              << "    static_assert(std::is_standard_layout_v<" << currentClassQualified << ">,\n"
                              << "        \"ECS component struct must satisfy std::is_standard_layout_v\");\n"
                              << "    const std::vector<realscript::runtime::NativeFieldLayout> fields = {\n";
                for (const auto& f : currentEcsFields) {
                    cppHelpersOut << "        {offsetof(" << currentClassQualified << ", " << f.name << "), "
                                  << mapRsTypeToNativeFieldKind(f.rsType) << "},\n";
                }
                cppHelpersOut << "    };\n"
                              << "    return runtime.bindEcsRawColumn(\n"
                              << "        \"" << currentClassModule << "::" << currentClass << "\",\n"
                              << "        data,\n"
                              << "        count,\n"
                              << "        sizeof(" << currentClassQualified << "),\n"
                              << "        fields,\n"
                              << "        error,\n"
                              << "        readOnly);\n"
                              << "}\n\n";
            } else {
                for (const auto& prop : pendingClassProperties) {
                    ++res.propertiesExported;
                    if (!prop.setterMethod.empty()) {
                        cppBodyOut << "    builder_" << currentClass << ".property<&" << currentClassQualified
                                   << "::" << prop.getterMethod << ", &" << currentClassQualified
                                   << "::" << prop.setterMethod << ">(\"" << prop.propName << "\");\n";
                        rsBodyOut << "    " << prop.rsType << " " << prop.propName << " {\n"
                                  << "        get { " << dummyRsReturn(prop.rsType) << " }\n"
                                  << "        set { return; }\n"
                                  << "    }\n";
                    } else {
                        cppBodyOut << "    builder_" << currentClass << ".property<&" << currentClassQualified
                                   << "::" << prop.getterMethod << ">(\"" << prop.propName << "\");\n";
                        rsBodyOut << "    " << prop.rsType << " " << prop.propName << " {\n"
                                  << "        get { " << dummyRsReturn(prop.rsType) << " }\n"
                                  << "    }\n";
                    }
                }
            }
            pendingClassProperties.clear();
            currentEcsFields.clear();
            currentClass.clear();
            currentClassQualified.clear();
            currentIsEcsStruct = false;
            rsBodyOut << "}\n\n";
            continue;
        }

        // Parse ECS struct fields inside RS_EXPORT_ECS_COMPONENT / RS_EXPORT_STRUCT
        if (!currentClass.empty() && currentIsEcsStruct) {
            std::string fieldLine = line;
            if (fieldLine == "public:" || fieldLine == "private:" || fieldLine == "protected:") {
                updateBraceDepth(line);
                continue;
            }
            std::vector<std::string> fieldMacroArgs;
            consumeMacro(fieldLine, "RS_EXPORT_FIELD", fieldMacroArgs);
            if (!fieldLine.empty() && fieldLine.back() == ';') {
                fieldLine.pop_back();
                const auto eqPos = fieldLine.find('=');
                if (eqPos != std::string::npos) fieldLine = trim(fieldLine.substr(0, eqPos));
                const auto bracePos = fieldLine.find('{');
                if (bracePos != std::string::npos) fieldLine = trim(fieldLine.substr(0, bracePos));
                if (fieldLine.find('(') == std::string::npos) {
                    const auto lastSpace = fieldLine.rfind(' ');
                    if (lastSpace != std::string::npos) {
                        std::string fType = trim(fieldLine.substr(0, lastSpace));
                        std::string fName = trim(fieldLine.substr(lastSpace + 1));
                        if (!fType.empty() && !fName.empty()) {
                            const std::string rsFieldType = mapCppTypeToRs(fType);
                            currentEcsFields.push_back({fName, rsFieldType});
                            rsBodyOut << "    public " << rsFieldType << " " << fName << ";\n";
                        }
                    }
                }
            }
            updateBraceDepth(line);
            continue;
        }

        if (pendingMethodExport || pendingFuncExport) {
            std::string decl = line;
            while ((decl.find('(') == std::string::npos ||
                    decl.find(')', decl.find('(')) == std::string::npos) &&
                   pos < cleaned.size()) {
                std::size_t nextNl = cleaned.find('\n', pos);
                if (nextNl == std::string::npos) nextNl = cleaned.size();
                decl += " " + trim(cleaned.substr(pos, nextNl - pos));
                pos = nextNl + 1;
            }
            updateBraceDepth(decl);
            const bool isMethod = pendingMethodExport && !currentClass.empty();
            const bool isNonDet = pendingNonDeterministic;
            const std::string cap = pendingCapability;
            pendingMethodExport = false;
            pendingFuncExport = false;
            pendingNonDeterministic = false;
            pendingCapability.clear();

            const auto openParen = decl.find('(');
            const auto closeParen = decl.find(')', openParen);
            if (openParen == std::string::npos || closeParen == std::string::npos) continue;
            const std::string prefix = trim(decl.substr(0, openParen));
            const auto lastSpace = prefix.rfind(' ');
            if (lastSpace == std::string::npos) continue;
            const std::string retCpp = trim(prefix.substr(0, lastSpace));
            std::string fnName = trim(prefix.substr(lastSpace + 1));
            while (!fnName.empty() && (fnName.front() == '*' || fnName.front() == '&')) {
                fnName.erase(fnName.begin());
            }
            const std::string paramsStr = trim(decl.substr(openParen + 1, closeParen - openParen - 1));

            std::vector<std::pair<std::string, std::string>> rsParams;
            if (!paramsStr.empty() && paramsStr != "void") {
                std::size_t pStart = 0;
                int idx = 0;
                while (pStart < paramsStr.size()) {
                    int depth = 0;
                    std::size_t comma = pStart;
                    while (comma < paramsStr.size()) {
                        const char c = paramsStr[comma];
                        if (c == '<' || c == '(' || c == '{') ++depth;
                        else if (c == '>' || c == ')' || c == '}') --depth;
                        else if (c == ',' && depth == 0) break;
                        ++comma;
                    }
                    std::string pItem = trim(paramsStr.substr(pStart, comma - pStart));
                    pStart = comma + 1;
                    const auto eqPos = pItem.find('=');
                    if (eqPos != std::string::npos) {
                        pItem = trim(pItem.substr(0, eqPos));
                    }
                    const auto pSpace = pItem.rfind(' ');
                    if (pSpace != std::string::npos) {
                        std::string pName = trim(pItem.substr(pSpace + 1));
                        std::string pType = trim(pItem.substr(0, pSpace));
                        while (!pName.empty() && (pName.front() == '&' || pName.front() == '*')) {
                            pType.push_back(pName.front());
                            pName.erase(pName.begin());
                        }
                        if (pName.empty()) pName = "arg" + std::to_string(idx);
                        rsParams.push_back({mapCppTypeToRs(pType), std::move(pName)});
                    } else {
                        rsParams.push_back({mapCppTypeToRs(pItem), "arg" + std::to_string(idx)});
                    }
                    ++idx;
                }
            }

            const std::string rsRet = mapCppTypeToRs(retCpp);
            for (auto& prop : pendingClassProperties) {
                if (prop.getterMethod == fnName) {
                    prop.rsType = rsRet;
                }
            }

            std::string detArg;
            if (isNonDet || !cap.empty()) {
                detArg = isNonDet
                    ? ", realscript::runtime::BindingDeterminism::NonDeterministic"
                    : ", realscript::runtime::BindingDeterminism::Deterministic";
                if (!cap.empty()) {
                    detArg += ", \"" + cap + "\"";
                }
            }

            const std::string indent = isMethod ? "    " : "";
            if (isNonDet) rsBodyOut << indent << "[NonDeterministic]\n";
            if (!cap.empty()) rsBodyOut << indent << "[Capability(\"" << cap << "\")]\n";

            if (isMethod) {
                ++res.methodsExported;
                cppBodyOut << "    builder_" << currentClass << ".method<&" << currentClassQualified
                           << "::" << fnName << ">(\"" << fnName << "\"" << detArg << ");\n";
                rsBodyOut << "    " << rsRet << " " << fnName << "(";
            } else {
                ++res.functionsExported;
                const std::string qualifiedFn = currentNamespacePrefix() + fnName;
                cppBodyOut << "    api.function(\"" << pendingFuncModule << "\", \""
                           << fnName << "\", &" << qualifiedFn << detArg << ");\n";
                rsBodyOut << rsRet << " " << fnName << "(";
            }
            for (std::size_t i = 0; i < rsParams.size(); ++i) {
                if (i != 0) rsBodyOut << ", ";
                rsBodyOut << rsParams[i].first << " " << rsParams[i].second;
            }
            rsBodyOut << ") { " << dummyRsReturn(rsRet) << " }\n";
            continue;
        }
        updateBraceDepth(line);
    }

    std::ostringstream cppOut;
    cppOut << "// Auto-generated RealScript C++ Binding Registration & Zero-Copy ECS Helpers\n";
    cppOut << "#pragma once\n\n";
    cppOut << "#include <cstddef>\n";
    cppOut << "#include <type_traits>\n";
    cppOut << "#include <vector>\n";
    cppOut << "#include \"realscript/game/GameScripting.h\"\n\n";
    if (!cppHelpersOut.str().empty()) {
        cppOut << cppHelpersOut.str();
    }
    cppOut << "inline void registerGeneratedHeaderBindings(realscript::game::GameApi& api) {\n";
    cppOut << "    (void)api;\n";
    cppOut << cppBodyOut.str();
    cppOut << "}\n";

    std::ostringstream rsOut;
    rsOut << "// Auto-generated RealScript Stub from C++ Header\n";
    rsOut << "module " << defaultModuleName << ";\n";
    if (usesMathTypes && defaultModuleName != "RealScript.Math") {
        rsOut << "import RealScript.Math;\n";
    }
    rsOut << "\n" << rsBodyOut.str();

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
