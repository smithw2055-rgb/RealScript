#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (root / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (root / path).write_text(text, encoding="utf-8", newline="\n")


def patch(path: str, old: str, new: str) -> None:
    text = read(path)
    if old not in text:
        raise RuntimeError(f"anchor missing in {path}: {old[:100]!r}")
    write(path, text.replace(old, new, 1))


# Syntax model ----------------------------------------------------------------
path = "include/realscript/syntax/Syntax.h"
text = read(path)
if "InterfaceKeyword" not in text:
    text = text.replace(
        "    EnumKeyword,\n    StaticKeyword,",
        "    EnumKeyword,\n    InterfaceKeyword,\n    StaticKeyword,",
        1)
    text = text.replace(
        "    EnumDeclaration,\n    EnumMemberDeclaration,",
        "    EnumDeclaration,\n    InterfaceDeclaration,\n    InterfaceMethodDeclaration,\n    EnumMemberDeclaration,",
        1)

    text = text.replace(
        '''struct ClassDeclarationSyntax final : SyntaxNode {
    SyntaxToken classKeyword;
    SyntaxToken identifierToken;
    SyntaxToken openBraceToken;
''',
        '''struct ClassDeclarationSyntax final : SyntaxNode {
    SyntaxToken classKeyword;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> colonToken;
    std::vector<TypeSyntax> interfaces;
    std::vector<SyntaxToken> interfaceCommaTokens;
    SyntaxToken openBraceToken;
''',
        1)
    text = text.replace(
        '''struct StructDeclarationSyntax final : SyntaxNode {
    SyntaxToken structKeyword;
    SyntaxToken identifierToken;
    SyntaxToken openBraceToken;
''',
        '''struct StructDeclarationSyntax final : SyntaxNode {
    SyntaxToken structKeyword;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> colonToken;
    std::vector<TypeSyntax> interfaces;
    std::vector<SyntaxToken> interfaceCommaTokens;
    SyntaxToken openBraceToken;
''',
        1)

    interface_nodes = '''struct InterfaceMethodDeclarationSyntax final : SyntaxNode {
    TypeSyntax returnType;
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;
    std::vector<ParameterSyntax> parameters;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::InterfaceMethodDeclaration;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct InterfaceDeclarationSyntax final : SyntaxNode {
    SyntaxToken interfaceKeyword;
    SyntaxToken identifierToken;
    SyntaxToken openBraceToken;
    std::vector<InterfaceMethodDeclarationSyntax> methods;
    SyntaxToken closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::InterfaceDeclaration;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

'''
    text = text.replace(
        "struct EnumMemberDeclarationSyntax final : SyntaxNode {",
        interface_nodes + "struct EnumMemberDeclarationSyntax final : SyntaxNode {",
        1)
    text = text.replace(
        "    std::vector<EnumDeclarationSyntax> enums;\n",
        "    std::vector<EnumDeclarationSyntax> enums;\n"
        "    std::vector<InterfaceDeclarationSyntax> interfaces;\n",
        1)
    text = text.replace(
        "    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration();\n",
        "    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration();\n"
        "    [[nodiscard]] InterfaceDeclarationSyntax parseInterfaceDeclaration();\n"
        "    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration();\n"
        "    void parseInterfaceList(\n"
        "        std::optional<SyntaxToken>& colonToken,\n"
        "        std::vector<TypeSyntax>& interfaces,\n"
        "        std::vector<SyntaxToken>& commaTokens);\n",
        1)
    write(path, text)

# Syntax facts ----------------------------------------------------------------
path = "src/syntax/SyntaxFacts.cpp"
text = read(path)
if "RS_KIND(InterfaceKeyword)" not in text:
    text = text.replace(
        "        RS_KIND(EnumKeyword);\n        RS_KIND(StaticKeyword);",
        "        RS_KIND(EnumKeyword);\n        RS_KIND(InterfaceKeyword);\n        RS_KIND(StaticKeyword);",
        1)
    text = text.replace(
        "        RS_KIND(EnumDeclaration);\n        RS_KIND(EnumMemberDeclaration);",
        "        RS_KIND(EnumDeclaration);\n"
        "        RS_KIND(InterfaceDeclaration);\n"
        "        RS_KIND(InterfaceMethodDeclaration);\n"
        "        RS_KIND(EnumMemberDeclaration);",
        1)
    text = text.replace(
        '        {"enum", SyntaxKind::EnumKeyword},\n'
        '        {"static", SyntaxKind::StaticKeyword},',
        '        {"enum", SyntaxKind::EnumKeyword},\n'
        '        {"interface", SyntaxKind::InterfaceKeyword},\n'
        '        {"static", SyntaxKind::StaticKeyword},',
        1)
    write(path, text)

# Parser ----------------------------------------------------------------------
path = "src/syntax/Parser.cpp"
text = read(path)
if "parseInterfaceDeclaration" not in text:
    text = text.replace(
        '''        } else if (current().kind == SyntaxKind::EnumKeyword) {
            result.enums.push_back(parseEnumDeclaration());
        } else {
''',
        '''        } else if (current().kind == SyntaxKind::EnumKeyword) {
            result.enums.push_back(parseEnumDeclaration());
        } else if (current().kind == SyntaxKind::InterfaceKeyword) {
            result.interfaces.push_back(parseInterfaceDeclaration());
        } else {
''',
        1)

    text = text.replace(
        '''    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
''',
        '''    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    parseInterfaceList(
        result.colonToken,
        result.interfaces,
        result.interfaceCommaTokens);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
''',
        1)
    # Same anchor appears for struct after class; replace next occurrence.
    text = text.replace(
        '''    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
''',
        '''    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    parseInterfaceList(
        result.colonToken,
        result.interfaces,
        result.interfaceCommaTokens);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
''',
        1)

    anchor = "EnumDeclarationSyntax Parser::parseEnumDeclaration() {"
    interface_parser = '''void Parser::parseInterfaceList(
    std::optional<SyntaxToken>& colonToken,
    std::vector<TypeSyntax>& interfaces,
    std::vector<SyntaxToken>& commaTokens) {
    if (current().kind != SyntaxKind::ColonToken) return;
    colonToken = nextToken();
    interfaces.push_back(parseType());
    while (current().kind == SyntaxKind::CommaToken) {
        commaTokens.push_back(nextToken());
        interfaces.push_back(parseType());
    }
}

InterfaceMethodDeclarationSyntax Parser::parseInterfaceMethodDeclaration() {
    InterfaceMethodDeclarationSyntax result;
    result.returnType = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.parameters.push_back(parseParameter());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.parameters.push_back(parseParameter());
        }
    }
    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

InterfaceDeclarationSyntax Parser::parseInterfaceDeclaration() {
    InterfaceDeclarationSyntax result;
    result.interfaceKeyword = match(SyntaxKind::InterfaceKeyword);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        result.methods.push_back(parseInterfaceMethodDeclaration());
        if (before == position_) {
            diagnostics_.report(
                "RS1111",
                "parser made no progress while reading an interface member",
                current().span);
            nextToken();
        }
    }
    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

'''
    if anchor not in text:
        raise RuntimeError("enum parser anchor missing")
    text = text.replace(anchor, interface_parser + anchor, 1)
    write(path, text)

# Syntax spans ----------------------------------------------------------------
path = "src/syntax/SyntaxNodes.cpp"
text = read(path)
if "InterfaceDeclarationSyntax::span" not in text:
    insertion = '''text::TextSpan InterfaceMethodDeclarationSyntax::span() const noexcept {
    return combine(returnType.span(), semicolonToken.span);
}

text::TextSpan InterfaceDeclarationSyntax::span() const noexcept {
    return combine(interfaceKeyword.span, closeBraceToken.span);
}

'''
    text = text.replace(
        "text::TextSpan EnumMemberDeclarationSyntax::span() const noexcept {",
        insertion + "text::TextSpan EnumMemberDeclarationSyntax::span() const noexcept {",
        1)
    text = text.replace(
        '''    if (!enums.empty()) {
        return combine(enums.front().span(), endOfFileToken.span);
    }
    if (!functions.empty()) {
''',
        '''    if (!enums.empty()) {
        return combine(enums.front().span(), endOfFileToken.span);
    }
    if (!interfaces.empty()) {
        return combine(interfaces.front().span(), endOfFileToken.span);
    }
    if (!functions.empty()) {
''',
        1)
    write(path, text)

# Compilation result ----------------------------------------------------------
path = "include/realscript/compiler/Compilation.h"
text = read(path)
if "nativeInterfaces" not in text:
    text = text.replace(
        "    std::vector<semantic::SymbolOccurrence> symbols;\n    BuildSnapshot snapshot;",
        "    std::vector<semantic::SymbolOccurrence> symbols;\n"
        "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n"
        "    BuildSnapshot snapshot;",
        1)
    write(path, text)

# Compiler interface contracts ------------------------------------------------
path = "src/compiler/Compilation.cpp"
text = read(path)
if "struct InterfaceMethodContract" not in text:
    contracts = '''struct InterfaceTypeRef {
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    std::string typeName;
};

struct InterfaceMethodContract {
    std::string name;
    InterfaceTypeRef returnType;
    std::vector<InterfaceTypeRef> parameters;
    text::TextSpan declarationSpan;
};

struct InterfaceContract {
    std::string moduleName;
    std::string name;
    std::string sourceName;
    text::TextSpan declarationSpan;
    std::vector<InterfaceMethodContract> methods;
};

struct InterfaceDeclarationInput {
    const syntax::InterfaceDeclarationSyntax* syntax = nullptr;
    std::string sourceName;
};

using InterfaceMap = std::map<std::string, InterfaceContract>;

'''
    text = text.replace("struct ParsedUnit {", contracts + "struct ParsedUnit {", 1)
    text = text.replace(
        '''    semantic::TypeSymbolMap visibleTypes;
    std::vector<semantic::FunctionSymbol> declarations;
''',
        '''    semantic::TypeSymbolMap visibleTypes;
    std::vector<InterfaceDeclarationInput> interfaceInputs;
    InterfaceMap interfaces;
    InterfaceMap visibleInterfaces;
    std::vector<semantic::FunctionSymbol> declarations;
''',
        1)

    helpers_anchor = "std::string fieldTypeSignature(const semantic::FieldSymbol& field) {"
    helpers = '''std::string canonicalInterfaceName(
    const std::string& moduleName,
    const std::string& name) {
    return moduleName.empty() ? name : moduleName + "::" + name;
}

void refreshVisibleInterfaces(
    std::map<std::string, ModuleWork>& modules,
    ModuleWork& module) {
    module.visibleInterfaces.clear();
    for (const auto& [name, contract] : module.interfaces) {
        module.visibleInterfaces[name] = contract;
        module.visibleInterfaces[
            canonicalInterfaceName(contract.moduleName, contract.name)] = contract;
    }
    for (const auto& importedName : module.imports) {
        const auto imported = modules.find(importedName);
        if (imported == modules.end()) continue;
        for (const auto& [name, contract] : imported->second.interfaces) {
            module.visibleInterfaces[name] = contract;
            module.visibleInterfaces[
                canonicalInterfaceName(contract.moduleName, contract.name)] = contract;
        }
    }
}

InterfaceTypeRef resolveInterfaceType(
    const syntax::TypeSyntax& syntaxTree,
    const semantic::TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    const std::string& sourceName,
    bool allowVoid) {
    InterfaceTypeRef result;
    auto baseType = semantic::resolvePrimitiveType(syntaxTree.name.text);
    std::string exactName;
    if (baseType == semantic::PrimitiveType::Error) {
        const auto found = visibleTypes.find(syntaxTree.name.text);
        if (found == visibleTypes.end()) {
            diagnostics.report(
                "RS2470",
                "unknown interface signature type '" + syntaxTree.name.text + "'",
                syntaxTree.span(),
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            return result;
        }
        baseType = found->second.kind == semantic::TypeKind::Class
            ? semantic::PrimitiveType::Object
            : found->second.kind == semantic::TypeKind::Struct
                ? semantic::PrimitiveType::Struct
                : semantic::PrimitiveType::Enum;
        exactName = semantic::canonicalTypeName(found->second);
    }
    if (syntaxTree.isArray()) {
        if (baseType == semantic::PrimitiveType::Void ||
            baseType == semantic::PrimitiveType::Array) {
            diagnostics.report(
                "RS2470",
                "invalid interface array type",
                syntaxTree.span(),
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            return result;
        }
        result.type = semantic::PrimitiveType::Array;
        result.typeName = semantic::arrayTypeName(baseType, exactName);
        return result;
    }
    if (baseType == semantic::PrimitiveType::Void && !allowVoid) {
        diagnostics.report(
            "RS2470",
            "void is not valid for an interface parameter",
            syntaxTree.span(),
            diagnostics::DiagnosticSeverity::Error,
            sourceName);
        return result;
    }
    result.type = baseType;
    if (semantic::isExactType(baseType)) result.typeName = exactName;
    return result;
}

bool sameInterfaceType(
    const InterfaceTypeRef& expected,
    semantic::PrimitiveType actual,
    const std::string& actualName) {
    return expected.type == actual &&
        (!semantic::isExactType(expected.type) ||
         expected.typeName == actualName);
}

std::string interfaceMethodSignature(
    const InterfaceMethodContract& method) {
    std::ostringstream out;
    out << method.name << '(';
    for (const auto& parameter : method.parameters) {
        out << semantic::primitiveTypeName(parameter.type) << '#'
            << parameter.typeName << ';';
    }
    out << ")->" << semantic::primitiveTypeName(method.returnType.type)
        << '#' << method.returnType.typeName;
    return out.str();
}

'''
    if helpers_anchor not in text:
        raise RuntimeError("compiler helper anchor missing")
    text = text.replace(helpers_anchor, helpers + helpers_anchor, 1)

    # Register interface declarations during shell collection.
    shell_anchor = '''            for (const auto& node : unit->syntaxTree->enums) {
                const auto before = module.types.size();
                addShell(node, [](const std::string& name, const auto& syntaxNode) {
                    return semantic::declareTypeShell(name, syntaxNode);
                });
                if (module.types.size() != before) {
                    module.types.back().sourceName = unit->source->name();
                    module.types.back().declarationSpan = node.identifierToken.span;
                }
            }
'''
    shell_replacement = shell_anchor + '''            for (const auto& node : unit->syntaxTree->interfaces) {
                if (!typeNames.insert(node.identifierToken.text).second) {
                    result.diagnostics.report(
                        "RS4004",
                        "duplicate type or interface '" +
                            node.identifierToken.text + "'",
                        node.identifierToken.span,
                        diagnostics::DiagnosticSeverity::Error,
                        unit->source->name());
                    module.invalid = true;
                    continue;
                }
                module.interfaceInputs.push_back(
                    InterfaceDeclarationInput{&node, unit->source->name()});
            }
'''
    if shell_anchor not in text:
        raise RuntimeError("shell anchor missing")
    text = text.replace(shell_anchor, shell_replacement, 1)

    # Resolve contracts after visible types exist.
    resolve_anchor = '''    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleTypes(modules, module);
    }

    // Resolve all field layouts and enum values before declaring member signatures.
'''
    resolve_code = '''    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleTypes(modules, module);
    }

    // Resolve native interface contracts after all named type shells exist.
    for (auto& [moduleName, module] : modules) {
        for (const auto& input : module.interfaceInputs) {
            if (!input.syntax) continue;
            InterfaceContract contract;
            contract.moduleName = moduleName;
            contract.name = input.syntax->identifierToken.text;
            contract.sourceName = input.sourceName;
            contract.declarationSpan = input.syntax->identifierToken.span;
            std::unordered_set<std::string> signatures;
            for (const auto& methodSyntax : input.syntax->methods) {
                InterfaceMethodContract method;
                method.name = methodSyntax.identifierToken.text;
                method.declarationSpan = methodSyntax.identifierToken.span;
                method.returnType = resolveInterfaceType(
                    methodSyntax.returnType,
                    module.visibleTypes,
                    result.diagnostics,
                    input.sourceName,
                    true);
                for (const auto& parameter : methodSyntax.parameters) {
                    method.parameters.push_back(resolveInterfaceType(
                        parameter.type,
                        module.visibleTypes,
                        result.diagnostics,
                        input.sourceName,
                        false));
                }
                const auto signature = interfaceMethodSignature(method);
                if (!signatures.insert(signature).second) {
                    result.diagnostics.report(
                        "RS2472",
                        "duplicate interface method '" + signature + "'",
                        methodSyntax.identifierToken.span,
                        diagnostics::DiagnosticSeverity::Error,
                        input.sourceName);
                    module.invalid = true;
                }
                contract.methods.push_back(std::move(method));
            }
            module.interfaces[contract.name] = std::move(contract);
        }
    }
    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleInterfaces(modules, module);
    }

    // Resolve all field layouts and enum values before declaring member signatures.
'''
    if resolve_anchor not in text:
        raise RuntimeError("interface resolution anchor missing")
    text = text.replace(resolve_anchor, resolve_code, 1)

    # Validate interfaces after all members are declared, before public fingerprint.
    fingerprint_anchor = '''        std::vector<std::string> signatures;
        signatures.reserve(module.declarations.size() + module.types.size());
'''
    validation = '''        const auto validateInterfaces = [&](auto const& declarations) {
            for (const auto& typeSyntax : declarations) {
                auto* owner = findOwnType(module, typeSyntax.identifierToken.text);
                if (!owner) continue;
                LanguageInterfaceImplementation implementation;
                implementation.typeName = semantic::canonicalTypeName(*owner);
                std::unordered_set<std::string> implementedNames;
                for (const auto& interfaceSyntax : typeSyntax.interfaces) {
                    const auto found =
                        module.visibleInterfaces.find(interfaceSyntax.name.text);
                    if (found == module.visibleInterfaces.end()) {
                        result.diagnostics.report(
                            "RS2473",
                            "unknown interface '" +
                                interfaceSyntax.name.text + "'",
                            interfaceSyntax.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            owner->sourceName);
                        module.invalid = true;
                        continue;
                    }
                    const auto canonicalName = canonicalInterfaceName(
                        found->second.moduleName,
                        found->second.name);
                    if (!implementedNames.insert(canonicalName).second) {
                        result.diagnostics.report(
                            "RS2474",
                            "interface '" + canonicalName +
                                "' is implemented more than once",
                            interfaceSyntax.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            owner->sourceName);
                        module.invalid = true;
                        continue;
                    }
                    implementation.interfaces.push_back(canonicalName);
                    for (const auto& required : found->second.methods) {
                        bool matched = false;
                        for (const auto& candidate : owner->methods) {
                            if (candidate.staticMethod ||
                                candidate.name != required.name) {
                                continue;
                            }
                            const auto parameterOffset =
                                candidate.method && !candidate.staticMethod
                                    ? std::size_t{1}
                                    : std::size_t{0};
                            if (candidate.parameters.size() < parameterOffset ||
                                candidate.parameters.size() - parameterOffset !=
                                    required.parameters.size()) {
                                continue;
                            }
                            bool compatible = sameInterfaceType(
                                required.returnType,
                                candidate.returnType,
                                candidate.returnTypeName);
                            for (std::size_t parameter = 0;
                                 compatible &&
                                 parameter < required.parameters.size();
                                 ++parameter) {
                                const auto& actual = candidate.parameters[
                                    parameter + parameterOffset];
                                compatible = sameInterfaceType(
                                    required.parameters[parameter],
                                    actual.type,
                                    actual.typeName);
                            }
                            if (compatible) {
                                matched = true;
                                break;
                            }
                        }
                        if (!matched) {
                            result.diagnostics.report(
                                "RS2475",
                                "type '" + implementation.typeName +
                                    "' does not implement interface method '" +
                                    canonicalName + "." +
                                    interfaceMethodSignature(required) + "'",
                                typeSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                owner->sourceName);
                            module.invalid = true;
                        }
                    }
                }
                if (!implementation.interfaces.empty()) {
                    std::sort(
                        implementation.interfaces.begin(),
                        implementation.interfaces.end());
                    result.nativeInterfaces.push_back(
                        std::move(implementation));
                }
            }
        };
        validateInterfaces(
            module.units.empty()
                ? std::vector<syntax::ClassDeclarationSyntax>{}
                : std::vector<syntax::ClassDeclarationSyntax>{});
        for (const auto* unit : module.units) {
            validateInterfaces(unit->syntaxTree->classes);
            validateInterfaces(unit->syntaxTree->structs);
        }

        std::vector<std::string> signatures;
        signatures.reserve(
            module.declarations.size() + module.types.size() +
            module.interfaces.size());
'''
    if fingerprint_anchor not in text:
        raise RuntimeError("fingerprint anchor missing")
    text = text.replace(fingerprint_anchor, validation, 1)

    # Include interface declarations and implementations in public fingerprint.
    signature_anchor = '''        for (const auto& type : module.types) signatures.push_back(typeSignature(type));
        for (const auto& function : module.declarations) {
'''
    signature_new = '''        for (const auto& type : module.types) {
            signatures.push_back(typeSignature(type));
        }
        for (const auto& [interfaceName, contract] : module.interfaces) {
            (void)interfaceName;
            std::ostringstream interfaceSignature;
            interfaceSignature << "interface:"
                << canonicalInterfaceName(contract.moduleName, contract.name)
                << '{';
            for (const auto& method : contract.methods) {
                interfaceSignature << interfaceMethodSignature(method) << ';';
            }
            interfaceSignature << '}';
            signatures.push_back(interfaceSignature.str());
        }
        for (const auto& implementation : result.nativeInterfaces) {
            if (implementation.typeName.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream implementationSignature;
            implementationSignature << "implements:"
                << implementation.typeName << ':';
            for (const auto& interfaceName : implementation.interfaces) {
                implementationSignature << interfaceName << ',';
            }
            signatures.push_back(implementationSignature.str());
        }
        for (const auto& function : module.declarations) {
'''
    if signature_anchor not in text:
        raise RuntimeError("signature anchor missing")
    text = text.replace(signature_anchor, signature_new, 1)

    # Sort metadata before returning from build.
    return_anchor = '''    return result;
}

} // namespace realscript::compiler
'''
    return_new = '''    std::stable_sort(
        result.nativeInterfaces.begin(),
        result.nativeInterfaces.end(),
        [](const auto& left, const auto& right) {
            return left.typeName < right.typeName;
        });
    return result;
}

} // namespace realscript::compiler
'''
    if return_anchor not in text:
        raise RuntimeError("build return anchor missing")
    text = text.replace(return_anchor, return_new, 1)
    write(path, text)

# Disable source interface expansion ------------------------------------------
path = "include/realscript/compiler/LanguageExpansion.h"
text = read(path)
text = text.replace("    bool interfaces = true;", "    bool interfaces = false;")
write(path, text)

# Game SDK merge ---------------------------------------------------------------
path = "src/game/GameApi.cpp"
text = read(path)
if "build.nativeInterfaces" not in text:
    text = text.replace(
        '''    auto build = compilation.build();
    result.diagnostics.append(build.diagnostics);
''',
        '''    auto build = compilation.build();
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
''',
        1)
    write(path, text)

# Native interface tests -------------------------------------------------------
path = "tests/phase18_native_control_flow_tests.cpp"
text = read(path)
if "testNativeInterfaceContracts" not in text:
    insert = r'''
void testNativeInterfaceContracts() {
    const char* contracts = R"(
module Phase18.Contracts;
interface IReader
{
    int Read(int value);
}
)";
    const char* app = R"(
module Phase18.App;
import Phase18.Contracts;
class Reader : IReader
{
    int Read(int value)
    {
        return value + 1;
    }
}
int main()
{
    Reader reader = new Reader();
    return reader.Read(41);
}
)";
    realscript::compiler::Compilation compilation({
        {"contracts.rs", contracts},
        {"app.rs", app},
    });
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "native interface contract failed:\n" +
            diagnosticsText(build.diagnostics));
    require(build.nativeInterfaces.size() == 1,
        "native interface implementation metadata was not retained");
    require(build.nativeInterfaces.front().typeName ==
            "Phase18.App::Reader" &&
            build.nativeInterfaces.front().interfaces.size() == 1 &&
            build.nativeInterfaces.front().interfaces.front() ==
                "Phase18.Contracts::IReader",
        "native interface metadata identity was incorrect");

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        modules.push_back(lowerer.lower(sourceModule));
    }
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase18.App::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "native interface implementation did not execute");
}

void testNativeInterfaceDiagnostics() {
    realscript::compiler::Compilation compilation({{"bad-interface.rs", R"(
module Phase18.Bad;
interface IReader
{
    int Read(int value);
}
class Reader : IReader
{
    long Read(int value)
    {
        return value;
    }
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "invalid native interface implementation was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS2475";
    }
    require(found,
        "native interface signature mismatch did not produce RS2475");
}

void testInterfaceBypassesExpansion() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "interface.rs",
        "module Native; interface IRun { int Run(); } class Runner : IRun { int Run(){return 1;} }");
    require(!expansion.changed,
        "native interfaces still used source expansion");
}
'''
    text = text.replace("} // namespace\n\nint main() {", insert + "\n} // namespace\n\nint main() {", 1)
    text = text.replace(
        '''    run("structured control flow bypasses expansion", testNoStructuredSourceRewrite);
''',
        '''    run("structured control flow bypasses expansion", testNoStructuredSourceRewrite);
    run("native interface contracts", testNativeInterfaceContracts);
    run("native interface diagnostics", testNativeInterfaceDiagnostics);
    run("interfaces bypass expansion", testInterfaceBypassesExpansion);
''',
        1)
    write(path, text)

print("Native Phase 18 interface migration applied")
