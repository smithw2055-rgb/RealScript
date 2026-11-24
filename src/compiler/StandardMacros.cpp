#include "realscript/compiler/StandardMacros.h"

#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realscript::compiler {
namespace {

[[nodiscard]] std::string trimAscii(std::string_view sv) {
    std::size_t start = 0;
    while (start < sv.size() &&
           (sv[start] == ' ' || sv[start] == '\t' || sv[start] == '\r' || sv[start] == '\n')) {
        ++start;
    }
    std::size_t end = sv.size();
    while (end > start &&
           (sv[end - 1] == ' ' || sv[end - 1] == '\t' || sv[end - 1] == '\r' ||
            sv[end - 1] == '\n')) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

[[nodiscard]] std::string sanitizePathComponent(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (const char ch : sv) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? std::string("module") : out;
}

[[nodiscard]] std::string escapeLineDirectivePath(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (const char ch : path) {
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

[[nodiscard]] std::string stripAttributeSuffix(std::string_view raw) {
    constexpr std::string_view kSuffix = "Attribute";
    if (raw.size() > kSuffix.size() &&
        raw.compare(raw.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        return std::string(raw.substr(0, raw.size() - kSuffix.size()));
    }
    return std::string(raw);
}

[[nodiscard]] std::string formatTypeSyntaxFallback(const syntax::TypeSyntax& type) {
    std::string out = type.name.text;
    if (!type.typeArguments.empty() || type.isGeneric()) {
        out.push_back('<');
        for (std::size_t i = 0; i < type.typeArguments.size(); ++i) {
            if (i > 0) out += ", ";
            out += formatTypeSyntaxFallback(type.typeArguments[i]);
        }
        out.push_back('>');
    }
    if (type.isNullable()) out.push_back('?');
    if (type.isArray()) out += "[]";
    return out;
}

[[nodiscard]] std::string renderTypeSyntax(
    const text::SourceText* sourceText,
    const syntax::TypeSyntax& type) {
    if (sourceText != nullptr) {
        const auto sp = type.span();
        if (!sp.empty()) {
            auto text = trimAscii(sourceText->view(sp));
            if (!text.empty()) return text;
        }
    }
    return formatTypeSyntaxFallback(type);
}

[[nodiscard]] std::string renderTypeParameters(const std::vector<syntax::SyntaxToken>& typeParams) {
    if (typeParams.empty()) return {};
    std::string out = "<";
    for (std::size_t i = 0; i < typeParams.size(); ++i) {
        if (i > 0) out += ", ";
        out += typeParams[i].text;
    }
    out.push_back('>');
    return out;
}

[[nodiscard]] std::string renderGenericConstraints(
    const text::SourceText* sourceText,
    const std::vector<syntax::GenericConstraintClauseSyntax>& constraints) {
    if (constraints.empty() || sourceText == nullptr) return {};
    std::string out;
    for (const auto& c : constraints) {
        const auto sp = c.span();
        if (!sp.empty()) {
            const auto piece = trimAscii(sourceText->view(sp));
            if (!piece.empty()) {
                out.push_back(' ');
                out += piece;
            }
        }
    }
    return out;
}

[[nodiscard]] std::string normalizedAccessPrefix(const syntax::FunctionDeclarationSyntax& fn) {
    if (semantic::hasModifier(fn.modifiers, syntax::SyntaxKind::PublicKeyword)) {
        return "public ";
    }
    if (semantic::hasModifier(fn.modifiers, syntax::SyntaxKind::ProtectedKeyword)) {
        return "protected ";
    }
    if (semantic::hasModifier(fn.modifiers, syntax::SyntaxKind::InternalKeyword)) {
        return "internal ";
    }
    return {};
}

[[nodiscard]] std::string extractAttrStringValue(
    const text::SourceText* sourceText,
    const syntax::AttributeArgumentSyntax& arg) {
    if (arg.valueTokens.size() == 1 &&
        arg.valueTokens[0].kind == syntax::SyntaxKind::StringLiteralToken &&
        std::holds_alternative<std::string>(arg.valueTokens[0].value)) {
        return std::get<std::string>(arg.valueTokens[0].value);
    }
    if (arg.valueTokens.size() == 4 &&
        arg.valueTokens[0].text == "nameof" &&
        arg.valueTokens[1].kind == syntax::SyntaxKind::OpenParenToken &&
        arg.valueTokens[3].kind == syntax::SyntaxKind::CloseParenToken) {
        return arg.valueTokens[2].text;
    }
    if (arg.valueTokens.size() == 1) {
        return arg.valueTokens[0].text;
    }
    if (sourceText != nullptr) {
        std::string raw = trimAscii(sourceText->view(arg.valueSpan()));
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
            return raw.substr(1, raw.size() - 2);
        }
        return raw;
    }
    return {};
}

[[nodiscard]] std::string extractRelayCanExecuteTarget(
    const text::SourceText* sourceText,
    const syntax::AttributeSyntax& attr) {
    std::string firstPositional;
    for (const auto& arg : attr.arguments) {
        if (arg.nameToken) {
            if (arg.nameToken->text == "CanExecute" || arg.nameToken->text == "canExecute") {
                return extractAttrStringValue(sourceText, arg);
            }
        } else if (firstPositional.empty()) {
            firstPositional = extractAttrStringValue(sourceText, arg);
        }
    }
    return firstPositional;
}

struct RecordIndex {
    std::vector<const syntax::ClassDeclarationSyntax*> classParts;
    std::vector<const syntax::StructDeclarationSyntax*> structParts;
    const text::SourceText* anySourceText = nullptr;
};

enum class CanExecuteKind {
    None,
    ZeroArgMethod,
    OneArgMethod,
    MemberAccess,
};

[[nodiscard]] CanExecuteKind classifyCanExecuteMember(
    const RecordIndex& index,
    const std::string& targetName) {
    if (targetName.empty()) return CanExecuteKind::None;

    const auto scanParts = [&](const auto& parts) -> CanExecuteKind {
        for (const auto* rec : parts) {
            if (!rec) continue;
            for (const auto& m : rec->methods) {
                if (m.identifierToken.text == targetName) {
                    return m.parameters.empty()
                        ? CanExecuteKind::ZeroArgMethod
                        : CanExecuteKind::OneArgMethod;
                }
            }
            for (const auto& p : rec->properties) {
                if (p.identifierToken.text == targetName) {
                    return CanExecuteKind::MemberAccess;
                }
            }
            for (const auto& f : rec->fields) {
                if (f.identifierToken.text == targetName) {
                    return CanExecuteKind::MemberAccess;
                }
            }
        }
        return CanExecuteKind::None;
    };

    if (const auto k = scanParts(index.classParts); k != CanExecuteKind::None) {
        return k;
    }
    if (const auto k = scanParts(index.structParts); k != CanExecuteKind::None) {
        return k;
    }
    // Fallback to 0-arg method if inherited / external
    return CanExecuteKind::ZeroArgMethod;
}

class StandardReactiveMacroPlugin final : public ICompilerMacroPlugin {
public:
    [[nodiscard]] std::string name() const override {
        return "realscript.standard_reactive_macros";
    }

    [[nodiscard]] std::vector<std::string> macroNames() const override {
        return {"Signal", "RelayCommand"};
    }

    [[nodiscard]] std::uint64_t fingerprint() const override {
        return stableFingerprint("realscript.standard_reactive_macros.v1");
    }

    bool isModuleLocal() const noexcept override {
        return true;
    }

    void expand(
        const MacroExpansionContext& context,
        std::vector<SourceFile>& outGeneratedSources,
        diagnostics::DiagnosticBag& diagnostics) const override {
        // Index all partial parts & imports by module so CanExecute lookup
        // sees members declared across 3-way partial files (`*.g.rs`, `*.rs`, ...).
        struct ModuleInfo {
            std::vector<std::string> imports;
            std::unordered_set<std::string> importSet;
            std::unordered_map<std::string, RecordIndex> records;
        };
        std::unordered_map<std::string, ModuleInfo> moduleMap;

        for (const auto& unit : context.parsedUnits) {
            if (!unit.syntaxTree) continue;
            auto& mod = moduleMap[unit.moduleName];
            for (const auto& imp : unit.imports) {
                if (mod.importSet.insert(imp).second) {
                    mod.imports.push_back(imp);
                }
            }
            for (const auto& c : unit.syntaxTree->classes) {
                const std::string key =
                    c.identifierToken.text + "#" + std::to_string(c.typeParameters.size());
                auto& rec = mod.records[key];
                rec.classParts.push_back(&c);
                if (!rec.anySourceText) rec.anySourceText = unit.sourceText;
            }
            for (const auto& s : unit.syntaxTree->structs) {
                const std::string key =
                    s.identifierToken.text + "#" + std::to_string(s.typeParameters.size());
                auto& rec = mod.records[key];
                rec.structParts.push_back(&s);
                if (!rec.anySourceText) rec.anySourceText = unit.sourceText;
            }
        }

        const auto reportNonMethodAttrs = [&](const MacroAstUnitView& unit,
                                              const std::vector<syntax::AttributeListSyntax>& lists,
                                              const char* targetDesc) {
            for (const auto& list : lists) {
                for (const auto& attr : list.attributes) {
                    const auto shortName = stripAttributeSuffix(attr.nameToken.text);
                    if (shortName == "Signal") {
                        diagnostics.report(
                            "RS9501",
                            std::string("[Signal] can only be applied to a partial void method, not a ") +
                                targetDesc,
                            attr.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            unit.sourcePath);
                    } else if (shortName == "RelayCommand") {
                        diagnostics.report(
                            "RS9508",
                            std::string("[RelayCommand] can only be applied to a method inside a partial class or struct, not a ") +
                                targetDesc,
                            attr.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            unit.sourcePath);
                    }
                }
            }
        };

        std::size_t generatedCounter = 0;
        for (const auto& unit : context.parsedUnits) {
            if (!unit.syntaxTree || !unit.sourceText) continue;
            const auto& cu = *unit.syntaxTree;
            const auto& modInfo = moduleMap[unit.moduleName];

            // Validate invalid non-method placements cleanly
            for (const auto& fn : cu.functions) {
                reportNonMethodAttrs(unit, fn.attributes, "top-level function");
            }
            for (const auto& en : cu.enums) {
                reportNonMethodAttrs(unit, en.attributes, "enum");
            }
            for (const auto& iface : cu.interfaces) {
                reportNonMethodAttrs(unit, iface.attributes, "interface");
                for (const auto& m : iface.methods) {
                    reportNonMethodAttrs(unit, m.attributes, "interface method");
                }
            }
            for (const auto& del : cu.delegates) {
                reportNonMethodAttrs(unit, del.attributes, "delegate");
            }

            std::ostringstream topDecls;
            std::ostringstream recordDecls;
            bool hasGenerated = false;

            const auto processRecordList = [&](const auto& decls, bool isStruct) {
                const char* kindKw = isStruct ? "struct" : "class";
                for (const auto& rec : decls) {
                    reportNonMethodAttrs(unit, rec.attributes, isStruct ? "struct" : "class");
                    for (const auto& f : rec.fields) {
                        reportNonMethodAttrs(unit, f.attributes, "field");
                    }
                    for (const auto& p : rec.properties) {
                        reportNonMethodAttrs(unit, p.attributes, "property");
                    }
                    for (const auto& e : rec.events) {
                        reportNonMethodAttrs(unit, e.attributes, "event");
                    }
                    for (const auto& c : rec.constructors) {
                        reportNonMethodAttrs(unit, c.attributes, "constructor");
                    }

                    const std::string& typeName = rec.identifierToken.text;
                    const std::string recKey =
                        typeName + "#" + std::to_string(rec.typeParameters.size());
                    const auto recIt = modInfo.records.find(recKey);
                    const RecordIndex* recIndex =
                        (recIt != modInfo.records.end()) ? &recIt->second : nullptr;

                    bool typeIsPartial = semantic::hasModifier(
                        rec.modifiers, syntax::SyntaxKind::PartialKeyword);
                    if (!typeIsPartial && recIndex != nullptr) {
                        for (const auto* part : recIndex->classParts) {
                            if (part &&
                                semantic::hasModifier(
                                    part->modifiers, syntax::SyntaxKind::PartialKeyword)) {
                                typeIsPartial = true;
                                break;
                            }
                        }
                        if (!typeIsPartial) {
                            for (const auto* part : recIndex->structParts) {
                                if (part &&
                                    semantic::hasModifier(
                                        part->modifiers, syntax::SyntaxKind::PartialKeyword)) {
                                    typeIsPartial = true;
                                    break;
                                }
                            }
                        }
                    }

                    const std::string typeParamsSuffix = renderTypeParameters(rec.typeParameters);
                    const std::string constraintsSuffix =
                        renderGenericConstraints(unit.sourceText, rec.constraints);

                    std::ostringstream bodyMembers;
                    bool recordHasGenerated = false;

                    for (const auto& m : rec.methods) {
                        const syntax::AttributeSyntax* signalAttr = nullptr;
                        const syntax::AttributeSyntax* relayAttr = nullptr;
                        for (const auto& list : m.attributes) {
                            for (const auto& attr : list.attributes) {
                                const auto shortName = stripAttributeSuffix(attr.nameToken.text);
                                if (shortName == "Signal" && signalAttr == nullptr) {
                                    signalAttr = &attr;
                                } else if (shortName == "RelayCommand" && relayAttr == nullptr) {
                                    relayAttr = &attr;
                                }
                            }
                        }
                        if (!signalAttr && !relayAttr) continue;

                        if (signalAttr && relayAttr) {
                            diagnostics.report(
                                "RS9509",
                                "[Signal] and [RelayCommand] cannot both be applied to method '" +
                                    m.identifierToken.text + "'",
                                signalAttr->span(),
                                diagnostics::DiagnosticSeverity::Error,
                                unit.sourcePath);
                            continue;
                        }

                        if (!typeIsPartial) {
                            const auto errSpan = signalAttr ? signalAttr->span() : relayAttr->span();
                            diagnostics.report(
                                signalAttr ? "RS9502" : "RS9510",
                                std::string(signalAttr ? "[Signal]" : "[RelayCommand]") +
                                    " requires enclosing " + kindKw + " '" + typeName +
                                    "' to be declared partial",
                                errSpan,
                                diagnostics::DiagnosticSeverity::Error,
                                unit.sourcePath);
                            continue;
                        }

                        const auto line1 =
                            unit.sourceText->linePosition(m.identifierToken.span.start).line + 1;
                        const std::string escapedPath = escapeLineDirectivePath(unit.sourcePath);
                        const std::string& methodName = m.identifierToken.text;
                        const std::string accPrefix = normalizedAccessPrefix(m);
                        const bool isStatic =
                            m.staticKeyword.has_value() ||
                            semantic::hasModifier(m.modifiers, syntax::SyntaxKind::StaticKeyword);
                        const bool isAbstract = semantic::hasModifier(
                            m.modifiers, syntax::SyntaxKind::AbstractKeyword);

                        if (signalAttr != nullptr) {
                            if (!semantic::hasModifier(m.modifiers, syntax::SyntaxKind::PartialKeyword)) {
                                diagnostics.report(
                                    "RS9503",
                                    "[Signal] method '" + methodName +
                                        "' must be declared 'partial void'",
                                    signalAttr->span(),
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }
                            if (m.returnType.name.text != "void" ||
                                m.refReturnKeyword.has_value()) {
                                diagnostics.report(
                                    "RS9504",
                                    "[Signal] method '" + methodName + "' must return void",
                                    m.returnType.span(),
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }
                            if (!m.semicolonToken.has_value()) {
                                diagnostics.report(
                                    "RS9505",
                                    "[Signal] method '" + methodName +
                                        "' must be a defining declaration ending in ';'",
                                    m.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }
                            if (isStatic) {
                                diagnostics.report(
                                    "RS9506",
                                    "[Signal] method '" + methodName + "' cannot be static",
                                    m.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }
                            if (!m.typeParameters.empty()) {
                                diagnostics.report(
                                    "RS9507",
                                    "[Signal] method '" + methodName +
                                        "' cannot declare method-level type parameters",
                                    m.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }
                            bool badParamMod = false;
                            for (const auto& p : m.parameters) {
                                if (p.modifierToken) {
                                    diagnostics.report(
                                        "RS9507",
                                        "[Signal] parameter '" + p.identifierToken.text +
                                            "' cannot use 'ref' or 'out'",
                                        p.modifierToken->span,
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit.sourcePath);
                                    badParamMod = true;
                                }
                            }
                            if (badParamMod) continue;

                            std::string paramDeclList;
                            std::string argNameList;
                            for (std::size_t pi = 0; pi < m.parameters.size(); ++pi) {
                                if (pi > 0) {
                                    paramDeclList += ", ";
                                    argNameList += ", ";
                                }
                                const auto& p = m.parameters[pi];
                                paramDeclList += renderTypeSyntax(unit.sourceText, p.type);
                                paramDeclList.push_back(' ');
                                paramDeclList += p.identifierToken.text;
                                argNameList += p.identifierToken.text;
                            }

                            const std::string delegateBase =
                                typeName + "__" + methodName + "SignalHandler";
                            const std::string delegateType = delegateBase + typeParamsSuffix;
                            const std::string eventName = methodName + "Event";
                            const std::string structSlotName = "_signal_" + methodName;

                            topDecls << "#line " << line1 << " \"" << escapedPath << "\"\n";
                            topDecls << "public delegate void " << delegateBase << typeParamsSuffix
                                     << "(" << paramDeclList << ")" << constraintsSuffix << ";\n";
                            topDecls << "#line default\n";

                            bodyMembers << "#line " << line1 << " \"" << escapedPath << "\"\n";
                            if (!isStruct) {
                                bodyMembers << "    public event " << delegateType << " "
                                            << eventName << ";\n";
                                bodyMembers << "    public void Subscribe" << methodName << "("
                                            << delegateType << " handler) {\n";
                                bodyMembers << "        this." << eventName << " += handler;\n";
                                bodyMembers << "    }\n";
                                bodyMembers << "    public void Unsubscribe" << methodName << "("
                                            << delegateType << " handler) {\n";
                                bodyMembers << "        this." << eventName << " -= handler;\n";
                                bodyMembers << "    }\n";
                                bodyMembers << "    " << accPrefix << "partial void " << methodName
                                            << "(" << paramDeclList << ") {\n";
                                bodyMembers << "        " << eventName << "(" << argNameList
                                            << ");\n";
                                bodyMembers << "    }\n";
                            } else {
                                // Struct owners forbid `event` (RS8315); use multicast delegate field.
                                bodyMembers << "    private " << delegateType << " "
                                            << structSlotName << ";\n";
                                bodyMembers << "    public void Subscribe" << methodName << "("
                                            << delegateType << " handler) {\n";
                                bodyMembers << "        if (handler == null) { return; }\n";
                                bodyMembers << "        if (this." << structSlotName
                                            << " == null) {\n";
                                bodyMembers << "            this." << structSlotName
                                            << " = handler;\n";
                                bodyMembers << "        } else {\n";
                                bodyMembers << "            this." << structSlotName
                                            << " = this." << structSlotName << " + handler;\n";
                                bodyMembers << "        }\n";
                                bodyMembers << "    }\n";
                                bodyMembers << "    public void Unsubscribe" << methodName << "("
                                            << delegateType << " handler) {\n";
                                bodyMembers << "        if (handler == null || this."
                                            << structSlotName << " == null) { return; }\n";
                                bodyMembers << "        this." << structSlotName << " = this."
                                            << structSlotName << " - handler;\n";
                                bodyMembers << "    }\n";
                                bodyMembers << "    " << accPrefix << "partial void " << methodName
                                            << "(" << paramDeclList << ") {\n";
                                bodyMembers << "        if (this." << structSlotName
                                            << " != null) {\n";
                                bodyMembers << "            var __rs_sig_handler = this."
                                            << structSlotName << ";\n";
                                bodyMembers << "            __rs_sig_handler(" << argNameList
                                            << ");\n";
                                bodyMembers << "        }\n";
                                bodyMembers << "    }\n";
                            }
                            bodyMembers << "#line default\n";
                            recordHasGenerated = true;
                        } else if (relayAttr != nullptr) {
                            if (isStatic || isAbstract) {
                                diagnostics.report(
                                    "RS9511",
                                    "[RelayCommand] method '" + methodName +
                                        "' must be a concrete instance method",
                                    relayAttr->span(),
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }
                            if (!m.typeParameters.empty()) {
                                diagnostics.report(
                                    "RS9512",
                                    "[RelayCommand] method '" + methodName +
                                        "' cannot declare method-level type parameters",
                                    m.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }
                            if (m.parameters.size() > 1) {
                                diagnostics.report(
                                    "RS9513",
                                    "[RelayCommand] method '" + methodName +
                                        "' accepts at most one parameter",
                                    relayAttr->span(),
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }
                            if (m.parameters.size() == 1 && m.parameters[0].modifierToken) {
                                diagnostics.report(
                                    "RS9513",
                                    "[RelayCommand] parameter '" +
                                        m.parameters[0].identifierToken.text +
                                        "' cannot use 'ref' or 'out'",
                                    m.parameters[0].modifierToken->span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    unit.sourcePath);
                                continue;
                            }

                            std::string paramDecl;
                            std::string argName;
                            if (m.parameters.size() == 1) {
                                paramDecl =
                                    renderTypeSyntax(unit.sourceText, m.parameters[0].type) +
                                    " " + m.parameters[0].identifierToken.text;
                                argName = m.parameters[0].identifierToken.text;
                            }

                            const std::string canExecuteTarget =
                                extractRelayCanExecuteTarget(unit.sourceText, *relayAttr);
                            std::string canExecuteExpr = "true";
                            if (!canExecuteTarget.empty()) {
                                const CanExecuteKind ceKind = recIndex
                                    ? classifyCanExecuteMember(*recIndex, canExecuteTarget)
                                    : CanExecuteKind::ZeroArgMethod;
                                switch (ceKind) {
                                case CanExecuteKind::MemberAccess:
                                    canExecuteExpr = "this." + canExecuteTarget;
                                    break;
                                case CanExecuteKind::OneArgMethod:
                                    canExecuteExpr = "this." + canExecuteTarget + "(" + argName + ")";
                                    break;
                                case CanExecuteKind::ZeroArgMethod:
                                    canExecuteExpr = "this." + canExecuteTarget + "()";
                                    break;
                                case CanExecuteKind::None:
                                    canExecuteExpr = "true";
                                    break;
                                }
                            }

                            std::string commandBase = methodName;
                            constexpr std::string_view kCmdSuffix = "Command";
                            if (commandBase.size() <= kCmdSuffix.size() ||
                                commandBase.compare(
                                    commandBase.size() - kCmdSuffix.size(),
                                    kCmdSuffix.size(),
                                    kCmdSuffix) != 0) {
                                commandBase += "Command";
                            } else {
                                commandBase += "RelayCommand";
                            }

                            const std::string cmdHandlerBase =
                                typeName + "__" + methodName + "CanExecuteChangedHandler";
                            const std::string cmdHandlerType = cmdHandlerBase + typeParamsSuffix;
                            const std::string cmdClassBase = typeName + "__" + commandBase;
                            const std::string cmdClassType = cmdClassBase + typeParamsSuffix;
                            const std::string cacheFieldName = "_" + methodName + "_cmd_cache";
                            const std::string ownerTypeRef = typeName + typeParamsSuffix;

                            topDecls << "#line " << line1 << " \"" << escapedPath << "\"\n";
                            topDecls << "public delegate void " << cmdHandlerBase << typeParamsSuffix
                                     << "()" << constraintsSuffix << ";\n";
                            topDecls << "public class " << cmdClassBase << typeParamsSuffix
                                     << constraintsSuffix << " {\n";
                            topDecls << "    private " << ownerTypeRef << " _owner;\n";
                            topDecls << "    public event " << cmdHandlerType
                                     << " CanExecuteChanged;\n";
                            topDecls << "    public " << cmdClassBase << "(" << ownerTypeRef
                                     << " owner) {\n";
                            topDecls << "        this._owner = owner;\n";
                            topDecls << "    }\n";
                            topDecls << "    public bool CanExecute(" << paramDecl << ") {\n";
                            topDecls << "        return this._owner.CanExecute" << methodName << "("
                                     << argName << ");\n";
                            topDecls << "    }\n";
                            topDecls << "    public void Execute(" << paramDecl << ") {\n";
                            topDecls << "        this._owner.Execute" << methodName << "("
                                     << argName << ");\n";
                            topDecls << "    }\n";
                            topDecls << "    public void SubscribeCanExecuteChanged(" << cmdHandlerType
                                     << " handler) {\n";
                            topDecls << "        this.CanExecuteChanged += handler;\n";
                            topDecls << "    }\n";
                            topDecls << "    public void UnsubscribeCanExecuteChanged("
                                     << cmdHandlerType << " handler) {\n";
                            topDecls << "        this.CanExecuteChanged -= handler;\n";
                            topDecls << "    }\n";
                            topDecls << "    public void RaiseCanExecuteChanged() {\n";
                            topDecls << "        CanExecuteChanged();\n";
                            topDecls << "    }\n";
                            topDecls << "}\n";
                            topDecls << "#line default\n";

                            bodyMembers << "#line " << line1 << " \"" << escapedPath << "\"\n";
                            bodyMembers << "    public bool CanExecute" << methodName << "("
                                        << paramDecl << ") {\n";
                            bodyMembers << "        return " << canExecuteExpr << ";\n";
                            bodyMembers << "    }\n";
                            bodyMembers << "    public void Execute" << methodName << "("
                                        << paramDecl << ") {\n";
                            bodyMembers << "        if (!this.CanExecute" << methodName << "("
                                        << argName << ")) { return; }\n";
                            bodyMembers << "        this." << methodName << "(" << argName << ");\n";
                            bodyMembers << "    }\n";
                            if (!isStruct) {
                                bodyMembers << "    private " << cmdClassType << " "
                                            << cacheFieldName << ";\n";
                                bodyMembers << "    public " << cmdClassType << " " << commandBase
                                            << " {\n";
                                bodyMembers << "        get {\n";
                                bodyMembers << "            if (this." << cacheFieldName
                                            << " == null) {\n";
                                bodyMembers << "                this." << cacheFieldName
                                            << " = new " << cmdClassType << "(this);\n";
                                bodyMembers << "            }\n";
                                bodyMembers << "            return this." << cacheFieldName << ";\n";
                                bodyMembers << "        }\n";
                                bodyMembers << "    }\n";
                                bodyMembers << "    public void Notify" << methodName
                                            << "CanExecuteChanged() {\n";
                                bodyMembers << "        if (this." << cacheFieldName
                                            << " != null) {\n";
                                bodyMembers << "            this." << cacheFieldName
                                            << ".RaiseCanExecuteChanged();\n";
                                bodyMembers << "        }\n";
                                bodyMembers << "    }\n";
                            } else {
                                bodyMembers << "    public " << cmdClassType << " " << commandBase
                                            << " {\n";
                                bodyMembers << "        get { return new " << cmdClassType
                                            << "(this); }\n";
                                bodyMembers << "    }\n";
                                bodyMembers << "    public void Notify" << methodName
                                            << "CanExecuteChanged() {}\n";
                            }
                            bodyMembers << "#line default\n";
                            recordHasGenerated = true;
                        }
                    }

                    if (recordHasGenerated) {
                        recordDecls << "partial " << kindKw << " " << typeName << typeParamsSuffix
                                    << " {\n";
                        recordDecls << bodyMembers.str();
                        recordDecls << "}\n";
                        hasGenerated = true;
                    }
                }
            };

            processRecordList(cu.classes, false);
            processRecordList(cu.structs, true);

            if (!hasGenerated) continue;

            std::ostringstream fileOut;
            if (cu.moduleDeclaration && !unit.moduleName.empty() && unit.moduleName != "$global") {
                fileOut << "module " << unit.moduleName << ";\n";
            }
            for (const auto& imp : modInfo.imports) {
                fileOut << "import " << imp << ";\n";
            }
            fileOut << "\n" << topDecls.str() << "\n" << recordDecls.str();

            outGeneratedSources.push_back(SourceFile{
                "$generated/standard_macros/" + sanitizePathComponent(unit.moduleName) + "_" +
                    std::to_string(generatedCounter++) + ".g.rscript",
                fileOut.str()});
        }
    }
};

} // namespace

std::shared_ptr<const ICompilerMacroPlugin> createStandardReactiveMacroPlugin() {
    return std::make_shared<StandardReactiveMacroPlugin>();
}

} // namespace realscript::compiler
