#!/usr/bin/env python3
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE = "fb50d53a3d85776796e90e9a95fd322aeb03cb7f"


def baseline(path: str) -> str:
    return subprocess.check_output(
        ["git", "show", f"{BASE}:{path}"],
        cwd=ROOT,
        text=True,
    )


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(text: str, old: str, new: str, path: str) -> str:
    if old not in text:
        raise RuntimeError(f"anchor missing in {path}: {old[:120]!r}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# Public attribute metadata and expansion defaults
# ---------------------------------------------------------------------------
path = "include/realscript/compiler/LanguageExpansion.h"
text = baseline(path)
text = replace_once(
    text,
    '''struct LanguageAttributeRecord {
    std::string target;
    std::string name;
    std::vector<LanguageAttributeArgument> arguments;
};
''',
    '''struct LanguageAttributeRecord {
    std::string target;
    std::string name;
    std::vector<LanguageAttributeArgument> arguments;
    std::string sourceName;
    std::size_t offset = 0;
};
''',
    path)
text = text.replace("    bool sourceAttributes = true;", "    bool sourceAttributes = false;")
write(path, text)


# ---------------------------------------------------------------------------
# Syntax model
# ---------------------------------------------------------------------------
path = "include/realscript/syntax/Syntax.h"
text = baseline(path)
text = replace_once(
    text,
    "    CompilationUnit,\n    ModuleDeclaration,",
    "    CompilationUnit,\n"
    "    AttributeList,\n"
    "    Attribute,\n"
    "    AttributeArgument,\n"
    "    ModuleDeclaration,",
    path)

attribute_nodes = '''struct AttributeArgumentSyntax final : SyntaxNode {
    std::optional<SyntaxToken> nameToken;
    std::optional<SyntaxToken> equalsToken;
    std::vector<SyntaxToken> valueTokens;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::AttributeArgument;
    }
    [[nodiscard]] text::TextSpan valueSpan() const noexcept;
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct AttributeSyntax final : SyntaxNode {
    SyntaxToken nameToken;
    std::optional<SyntaxToken> openParenToken;
    std::vector<AttributeArgumentSyntax> arguments;
    std::vector<SyntaxToken> commaTokens;
    std::optional<SyntaxToken> closeParenToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::Attribute;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct AttributeListSyntax final : SyntaxNode {
    SyntaxToken openBracketToken;
    std::vector<AttributeSyntax> attributes;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeBracketToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::AttributeList;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

'''
text = replace_once(
    text,
    "struct ExpressionSyntax : SyntaxNode {};\nstruct StatementSyntax : SyntaxNode {};\n\n",
    "struct ExpressionSyntax : SyntaxNode {};\n"
    "struct StatementSyntax : SyntaxNode {};\n\n" + attribute_nodes,
    path)

for anchor in [
    "struct FieldDeclarationSyntax final : SyntaxNode {\n",
    "struct ConstructorDeclarationSyntax final : SyntaxNode {\n",
    "struct PropertyDeclarationSyntax final : SyntaxNode {\n",
    "struct ClassDeclarationSyntax final : SyntaxNode {\n",
    "struct StructDeclarationSyntax final : SyntaxNode {\n",
    "struct InterfaceMethodDeclarationSyntax final : SyntaxNode {\n",
    "struct InterfaceDeclarationSyntax final : SyntaxNode {\n",
    "struct EnumMemberDeclarationSyntax final : SyntaxNode {\n",
    "struct EnumDeclarationSyntax final : SyntaxNode {\n",
    "struct FunctionDeclarationSyntax final : SyntaxNode {\n",
]:
    text = replace_once(
        text,
        anchor,
        anchor + "    std::vector<AttributeListSyntax> attributes;\n",
        path)

text = replace_once(
    text,
    "    [[nodiscard]] ClassDeclarationSyntax parseClassDeclaration();\n",
    "    [[nodiscard]] std::vector<AttributeListSyntax> parseAttributeLists();\n"
    "    [[nodiscard]] AttributeListSyntax parseAttributeList();\n"
    "    [[nodiscard]] AttributeSyntax parseAttribute();\n"
    "    [[nodiscard]] AttributeArgumentSyntax parseAttributeArgument();\n"
    "    [[nodiscard]] ClassDeclarationSyntax parseClassDeclaration(\n"
    "        std::vector<AttributeListSyntax> attributes);\n",
    path)
text = replace_once(
    text,
    "    [[nodiscard]] StructDeclarationSyntax parseStructDeclaration();\n",
    "    [[nodiscard]] StructDeclarationSyntax parseStructDeclaration(\n"
    "        std::vector<AttributeListSyntax> attributes);\n",
    path)
text = replace_once(
    text,
    "    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration();\n",
    "    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration(\n"
    "        std::vector<AttributeListSyntax> attributes);\n",
    path)
text = replace_once(
    text,
    "    [[nodiscard]] InterfaceDeclarationSyntax parseInterfaceDeclaration();\n",
    "    [[nodiscard]] InterfaceDeclarationSyntax parseInterfaceDeclaration(\n"
    "        std::vector<AttributeListSyntax> attributes);\n",
    path)
text = replace_once(
    text,
    "    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration();\n",
    "    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration(\n"
    "        std::vector<AttributeListSyntax> attributes);\n",
    path)
text = replace_once(
    text,
    "    [[nodiscard]] FieldDeclarationSyntax parseFieldDeclaration(TypeSyntax type, SyntaxToken identifier);\n",
    "    [[nodiscard]] FieldDeclarationSyntax parseFieldDeclaration(\n"
    "        TypeSyntax type,\n"
    "        SyntaxToken identifier,\n"
    "        std::vector<AttributeListSyntax> attributes);\n",
    path)
text = replace_once(
    text,
    "        std::optional<SyntaxToken> identifier = std::nullopt);\n",
    "        std::optional<SyntaxToken> identifier = std::nullopt,\n"
    "        std::vector<AttributeListSyntax> attributes = {});\n",
    path)
text = replace_once(
    text,
    "    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration();\n",
    "    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration(\n"
    "        std::vector<AttributeListSyntax> attributes);\n",
    path)
text = replace_once(
    text,
    "        TypeSyntax type,\n        SyntaxToken identifier);\n",
    "        TypeSyntax type,\n"
    "        SyntaxToken identifier,\n"
    "        std::vector<AttributeListSyntax> attributes);\n",
    path)
write(path, text)


# ---------------------------------------------------------------------------
# Syntax facts
# ---------------------------------------------------------------------------
path = "src/syntax/SyntaxFacts.cpp"
text = baseline(path)
text = replace_once(
    text,
    "        RS_KIND(CompilationUnit);\n        RS_KIND(ModuleDeclaration);",
    "        RS_KIND(CompilationUnit);\n"
    "        RS_KIND(AttributeList);\n"
    "        RS_KIND(Attribute);\n"
    "        RS_KIND(AttributeArgument);\n"
    "        RS_KIND(ModuleDeclaration);",
    path)
write(path, text)


# ---------------------------------------------------------------------------
# Syntax spans
# ---------------------------------------------------------------------------
path = "src/syntax/SyntaxNodes.cpp"
text = baseline(path)
text = replace_once(
    text,
    "std::string joinQualifiedName(const std::vector<SyntaxToken>& parts) {",
    '''text::TextSpan declarationStart(
    const std::vector<AttributeListSyntax>& attributes,
    text::TextSpan fallback) noexcept {
    return attributes.empty()
        ? fallback
        : text::TextSpan::fromBounds(
            attributes.front().span().start,
            fallback.end());
}

std::string joinQualifiedName(const std::vector<SyntaxToken>& parts) {''',
    path)
text = replace_once(
    text,
    "text::TextSpan TypeSyntax::span() const noexcept {",
    '''text::TextSpan AttributeArgumentSyntax::valueSpan() const noexcept {
    if (valueTokens.empty()) return {};
    return combine(valueTokens.front().span, valueTokens.back().span);
}

text::TextSpan AttributeArgumentSyntax::span() const noexcept {
    const auto value = valueSpan();
    if (nameToken) {
        return combine(
            nameToken->span,
            value.empty() ? nameToken->span : value);
    }
    return value;
}

text::TextSpan AttributeSyntax::span() const noexcept {
    return closeParenToken
        ? combine(nameToken.span, closeParenToken->span)
        : nameToken.span;
}

text::TextSpan AttributeListSyntax::span() const noexcept {
    return combine(openBracketToken.span, closeBracketToken.span);
}

text::TextSpan TypeSyntax::span() const noexcept {''',
    path)

replacements = [
    (
        '''text::TextSpan FieldDeclarationSyntax::span() const noexcept {
    return combine(type.span(), semicolonToken.span);
}
''',
        '''text::TextSpan FieldDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, type.span()),
        semicolonToken.span);
}
'''),
    (
        '''text::TextSpan ConstructorDeclarationSyntax::span() const noexcept {
    return combine(identifierToken.span, body.span());
}
''',
        '''text::TextSpan ConstructorDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, identifierToken.span),
        body.span());
}
'''),
    (
        '''text::TextSpan PropertyDeclarationSyntax::span() const noexcept {
    return combine(type.span(), closeBraceToken.span);
}
''',
        '''text::TextSpan PropertyDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, type.span()),
        closeBraceToken.span);
}
'''),
    (
        '''text::TextSpan ClassDeclarationSyntax::span() const noexcept {
    return combine(classKeyword.span, closeBraceToken.span);
}
''',
        '''text::TextSpan ClassDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, classKeyword.span),
        closeBraceToken.span);
}
'''),
    (
        '''text::TextSpan StructDeclarationSyntax::span() const noexcept {
    return combine(structKeyword.span, closeBraceToken.span);
}
''',
        '''text::TextSpan StructDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, structKeyword.span),
        closeBraceToken.span);
}
'''),
    (
        '''text::TextSpan InterfaceMethodDeclarationSyntax::span() const noexcept {
    return combine(returnType.span(), semicolonToken.span);
}
''',
        '''text::TextSpan InterfaceMethodDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, returnType.span()),
        semicolonToken.span);
}
'''),
    (
        '''text::TextSpan InterfaceDeclarationSyntax::span() const noexcept {
    return combine(interfaceKeyword.span, closeBraceToken.span);
}
''',
        '''text::TextSpan InterfaceDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, interfaceKeyword.span),
        closeBraceToken.span);
}
'''),
    (
        '''text::TextSpan EnumMemberDeclarationSyntax::span() const noexcept {
    if (commaToken) return combine(identifierToken.span, commaToken->span);
    if (valueToken) return combine(identifierToken.span, valueToken->span);
    return identifierToken.span;
}
''',
        '''text::TextSpan EnumMemberDeclarationSyntax::span() const noexcept {
    text::TextSpan end = identifierToken.span;
    if (commaToken) end = commaToken->span;
    else if (valueToken) end = valueToken->span;
    return combine(
        declarationStart(attributes, identifierToken.span),
        end);
}
'''),
    (
        '''text::TextSpan EnumDeclarationSyntax::span() const noexcept {
    return combine(enumKeyword.span, closeBraceToken.span);
}
''',
        '''text::TextSpan EnumDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, enumKeyword.span),
        closeBraceToken.span);
}
'''),
    (
        '''text::TextSpan FunctionDeclarationSyntax::span() const noexcept {
    return combine(returnType.span(), body.span());
}
''',
        '''text::TextSpan FunctionDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, returnType.span()),
        body.span());
}
'''),
]
for old, new in replacements:
    text = replace_once(text, old, new, path)
write(path, text)


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------
path = "src/syntax/Parser.cpp"
text = baseline(path)
text = replace_once(
    text,
    '''    while (current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        if (current().kind == SyntaxKind::ClassKeyword) {
            result.classes.push_back(parseClassDeclaration());
        } else if (current().kind == SyntaxKind::StructKeyword) {
            result.structs.push_back(parseStructDeclaration());
        } else if (current().kind == SyntaxKind::EnumKeyword) {
            result.enums.push_back(parseEnumDeclaration());
        } else if (current().kind == SyntaxKind::InterfaceKeyword) {
            result.interfaces.push_back(parseInterfaceDeclaration());
        } else {
            result.functions.push_back(parseFunctionDeclaration());
        }
''',
    '''    while (current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        auto attributes = parseAttributeLists();
        if (current().kind == SyntaxKind::ClassKeyword) {
            result.classes.push_back(
                parseClassDeclaration(std::move(attributes)));
        } else if (current().kind == SyntaxKind::StructKeyword) {
            result.structs.push_back(
                parseStructDeclaration(std::move(attributes)));
        } else if (current().kind == SyntaxKind::EnumKeyword) {
            result.enums.push_back(
                parseEnumDeclaration(std::move(attributes)));
        } else if (current().kind == SyntaxKind::InterfaceKeyword) {
            result.interfaces.push_back(
                parseInterfaceDeclaration(std::move(attributes)));
        } else {
            result.functions.push_back(parseFunctionDeclaration(
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::move(attributes)));
        }
''',
    path)

attribute_parser = '''std::vector<AttributeListSyntax> Parser::parseAttributeLists() {
    std::vector<AttributeListSyntax> result;
    while (current().kind == SyntaxKind::OpenBracketToken) {
        result.push_back(parseAttributeList());
    }
    return result;
}

AttributeArgumentSyntax Parser::parseAttributeArgument() {
    AttributeArgumentSyntax result;
    if (current().kind == SyntaxKind::IdentifierToken &&
        peek(1).kind == SyntaxKind::EqualsToken) {
        result.nameToken = nextToken();
        result.equalsToken = nextToken();
    }
    int parens = 0;
    int brackets = 0;
    int braces = 0;
    while (current().kind != SyntaxKind::EndOfFileToken) {
        if (parens == 0 && brackets == 0 && braces == 0 &&
            (current().kind == SyntaxKind::CommaToken ||
             current().kind == SyntaxKind::CloseParenToken)) {
            break;
        }
        if (current().kind == SyntaxKind::OpenParenToken) ++parens;
        else if (current().kind == SyntaxKind::CloseParenToken &&
                 parens > 0) --parens;
        else if (current().kind == SyntaxKind::OpenBracketToken) ++brackets;
        else if (current().kind == SyntaxKind::CloseBracketToken &&
                 brackets > 0) --brackets;
        else if (current().kind == SyntaxKind::OpenBraceToken) ++braces;
        else if (current().kind == SyntaxKind::CloseBraceToken &&
                 braces > 0) --braces;
        result.valueTokens.push_back(nextToken());
    }
    if (result.valueTokens.empty()) {
        diagnostics_.report(
            "RS1112",
            "attribute argument requires a constant value",
            current().span);
    }
    return result;
}

AttributeSyntax Parser::parseAttribute() {
    AttributeSyntax result;
    result.nameToken = match(SyntaxKind::IdentifierToken);
    if (current().kind == SyntaxKind::OpenParenToken) {
        result.openParenToken = nextToken();
        if (current().kind != SyntaxKind::CloseParenToken &&
            current().kind != SyntaxKind::EndOfFileToken) {
            result.arguments.push_back(parseAttributeArgument());
            while (current().kind == SyntaxKind::CommaToken) {
                result.commaTokens.push_back(nextToken());
                result.arguments.push_back(parseAttributeArgument());
            }
        }
        result.closeParenToken = match(SyntaxKind::CloseParenToken);
    }
    return result;
}

AttributeListSyntax Parser::parseAttributeList() {
    AttributeListSyntax result;
    result.openBracketToken = match(SyntaxKind::OpenBracketToken);
    if (current().kind != SyntaxKind::CloseBracketToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.attributes.push_back(parseAttribute());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.attributes.push_back(parseAttribute());
        }
    }
    result.closeBracketToken = match(SyntaxKind::CloseBracketToken);
    return result;
}

'''
text = replace_once(
    text,
    "TypeSyntax Parser::parseType() {",
    attribute_parser + "TypeSyntax Parser::parseType() {",
    path)

text = replace_once(
    text,
    '''FieldDeclarationSyntax Parser::parseFieldDeclaration(
    TypeSyntax type,
    SyntaxToken identifier) {
    FieldDeclarationSyntax result;
''',
    '''FieldDeclarationSyntax Parser::parseFieldDeclaration(
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes) {
    FieldDeclarationSyntax result;
    result.attributes = std::move(attributes);
''',
    path)
text = replace_once(
    text,
    '''ConstructorDeclarationSyntax Parser::parseConstructorDeclaration() {
    ConstructorDeclarationSyntax result;
''',
    '''ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    ConstructorDeclarationSyntax result;
    result.attributes = std::move(attributes);
''',
    path)
text = replace_once(
    text,
    '''PropertyDeclarationSyntax Parser::parsePropertyDeclaration(
    std::optional<SyntaxToken> staticKeyword,
    TypeSyntax type,
    SyntaxToken identifier) {
    PropertyDeclarationSyntax result;
''',
    '''PropertyDeclarationSyntax Parser::parsePropertyDeclaration(
    std::optional<SyntaxToken> staticKeyword,
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes) {
    PropertyDeclarationSyntax result;
    result.attributes = std::move(attributes);
''',
    path)
text = replace_once(
    text,
    '''FunctionDeclarationSyntax Parser::parseFunctionDeclaration(
    std::optional<SyntaxToken> staticKeyword,
    std::optional<TypeSyntax> returnType,
    std::optional<SyntaxToken> identifier) {
    FunctionDeclarationSyntax result;
''',
    '''FunctionDeclarationSyntax Parser::parseFunctionDeclaration(
    std::optional<SyntaxToken> staticKeyword,
    std::optional<TypeSyntax> returnType,
    std::optional<SyntaxToken> identifier,
    std::vector<AttributeListSyntax> attributes) {
    FunctionDeclarationSyntax result;
    result.attributes = std::move(attributes);
''',
    path)
text = replace_once(
    text,
    "ClassDeclarationSyntax Parser::parseClassDeclaration() {\n"
    "    ClassDeclarationSyntax result;\n",
    "ClassDeclarationSyntax Parser::parseClassDeclaration(\n"
    "    std::vector<AttributeListSyntax> attributes) {\n"
    "    ClassDeclarationSyntax result;\n"
    "    result.attributes = std::move(attributes);\n",
    path)
text = replace_once(
    text,
    "StructDeclarationSyntax Parser::parseStructDeclaration() {\n"
    "    StructDeclarationSyntax result;\n",
    "StructDeclarationSyntax Parser::parseStructDeclaration(\n"
    "    std::vector<AttributeListSyntax> attributes) {\n"
    "    StructDeclarationSyntax result;\n"
    "    result.attributes = std::move(attributes);\n",
    path)
text = replace_once(
    text,
    "InterfaceMethodDeclarationSyntax Parser::parseInterfaceMethodDeclaration() {\n"
    "    InterfaceMethodDeclarationSyntax result;\n",
    "InterfaceMethodDeclarationSyntax Parser::parseInterfaceMethodDeclaration(\n"
    "    std::vector<AttributeListSyntax> attributes) {\n"
    "    InterfaceMethodDeclarationSyntax result;\n"
    "    result.attributes = std::move(attributes);\n",
    path)
text = replace_once(
    text,
    "InterfaceDeclarationSyntax Parser::parseInterfaceDeclaration() {\n"
    "    InterfaceDeclarationSyntax result;\n",
    "InterfaceDeclarationSyntax Parser::parseInterfaceDeclaration(\n"
    "    std::vector<AttributeListSyntax> attributes) {\n"
    "    InterfaceDeclarationSyntax result;\n"
    "    result.attributes = std::move(attributes);\n",
    path)
text = replace_once(
    text,
    "EnumDeclarationSyntax Parser::parseEnumDeclaration() {\n"
    "    EnumDeclarationSyntax result;\n",
    "EnumDeclarationSyntax Parser::parseEnumDeclaration(\n"
    "    std::vector<AttributeListSyntax> attributes) {\n"
    "    EnumDeclarationSyntax result;\n"
    "    result.attributes = std::move(attributes);\n",
    path)

# Class member attributes.
class_start = text.index("ClassDeclarationSyntax Parser::parseClassDeclaration")
class_end = text.index("StructDeclarationSyntax Parser::parseStructDeclaration", class_start)
class_text = text[class_start:class_end]
class_text = replace_once(
    class_text,
    "        const auto before = position_;\n"
    "        std::optional<SyntaxToken> staticKeyword;\n",
    "        const auto before = position_;\n"
    "        auto memberAttributes = parseAttributeLists();\n"
    "        std::optional<SyntaxToken> staticKeyword;\n",
    path)
class_text = class_text.replace(
    "result.constructors.push_back(parseConstructorDeclaration());",
    "result.constructors.push_back(parseConstructorDeclaration(\n"
    "                std::move(memberAttributes)));",
    1)
class_text = class_text.replace(
    "std::move(staticKeyword), std::move(type), std::move(identifier)))",
    "std::move(staticKeyword),\n"
    "                    std::move(type),\n"
    "                    std::move(identifier),\n"
    "                    std::move(memberAttributes)))",
    1)
class_text = class_text.replace(
    "std::move(staticKeyword), std::move(type), std::move(identifier)))",
    "std::move(staticKeyword),\n"
    "                    std::move(type),\n"
    "                    std::move(identifier),\n"
    "                    std::move(memberAttributes)))",
    1)
class_text = class_text.replace(
    "std::move(type), std::move(identifier)))",
    "std::move(type),\n"
    "                    std::move(identifier),\n"
    "                    std::move(memberAttributes)))",
    1)
text = text[:class_start] + class_text + text[class_end:]

# Struct member attributes.
struct_start = text.index("StructDeclarationSyntax Parser::parseStructDeclaration")
struct_end = text.index("void Parser::parseInterfaceList", struct_start)
struct_text = text[struct_start:struct_end]
struct_text = replace_once(
    struct_text,
    "        const auto before = position_;\n"
    "        std::optional<SyntaxToken> staticKeyword;\n",
    "        const auto before = position_;\n"
    "        auto memberAttributes = parseAttributeLists();\n"
    "        std::optional<SyntaxToken> staticKeyword;\n",
    path)
struct_text = struct_text.replace(
    "result.constructors.push_back(parseConstructorDeclaration());",
    "result.constructors.push_back(parseConstructorDeclaration(\n"
    "                std::move(memberAttributes)));",
    1)
struct_text = struct_text.replace(
    "std::move(staticKeyword), std::move(type), std::move(identifier)))",
    "std::move(staticKeyword),\n"
    "                    std::move(type),\n"
    "                    std::move(identifier),\n"
    "                    std::move(memberAttributes)))",
    1)
struct_text = struct_text.replace(
    "std::move(staticKeyword), std::move(type), std::move(identifier)))",
    "std::move(staticKeyword),\n"
    "                    std::move(type),\n"
    "                    std::move(identifier),\n"
    "                    std::move(memberAttributes)))",
    1)
struct_text = struct_text.replace(
    "std::move(type), std::move(identifier)))",
    "std::move(type),\n"
    "                    std::move(identifier),\n"
    "                    std::move(memberAttributes)))",
    1)
text = text[:struct_start] + struct_text + text[struct_end:]

text = replace_once(
    text,
    "        result.methods.push_back(parseInterfaceMethodDeclaration());\n",
    "        result.methods.push_back(parseInterfaceMethodDeclaration(\n"
    "            parseAttributeLists()));\n",
    path)
text = replace_once(
    text,
    "        EnumMemberDeclarationSyntax member;\n"
    "        member.identifierToken = match(SyntaxKind::IdentifierToken);\n",
    "        EnumMemberDeclarationSyntax member;\n"
    "        member.attributes = parseAttributeLists();\n"
    "        member.identifierToken = match(SyntaxKind::IdentifierToken);\n",
    path)
write(path, text)


# ---------------------------------------------------------------------------
# Generic expansion compatibility: preserve native attribute token lists
# ---------------------------------------------------------------------------
path = "src/compiler/LanguageExpansion.cpp"
text = baseline(path)
text = replace_once(
    text,
    "void collectGenericDeclarationsWithInterfaces(\n",
    '''std::size_t genericDeclarationStart(
    const std::vector<Token>& tokens,
    std::size_t declaration) {
    auto start = declaration;
    while (start > 0 && symbol(tokens[start - 1], "]")) {
        auto cursor = start - 1;
        int depth = 1;
        while (cursor > 0 && depth != 0) {
            --cursor;
            if (symbol(tokens[cursor], "]")) ++depth;
            else if (symbol(tokens[cursor], "[")) --depth;
        }
        if (depth != 0) break;
        start = cursor;
    }
    return start;
}

void collectGenericDeclarationsWithInterfaces(
''',
    path)
text = replace_once(
    text,
    '''            GenericDecl declaration;
            declaration.kind = GenericDecl::Kind::Type;
            declaration.name = tokens[index + 1].text;
            declaration.parameters = parseTypeParameterNames(
                tokens, index + 2, angleClose);
            declaration.tokens.assign(
                tokens.begin() + static_cast<std::ptrdiff_t>(index),
                tokens.begin() + static_cast<std::ptrdiff_t>(bodyClose + 1));
            context.generics[declaration.name] = std::move(declaration);
            remove.push_back({index, bodyClose + 1});
''',
    '''            const auto declarationStart =
                genericDeclarationStart(tokens, index);
            GenericDecl declaration;
            declaration.kind = GenericDecl::Kind::Type;
            declaration.name = tokens[index + 1].text;
            declaration.parameters = parseTypeParameterNames(
                tokens, index + 2, angleClose);
            declaration.tokens.assign(
                tokens.begin() +
                    static_cast<std::ptrdiff_t>(declarationStart),
                tokens.begin() +
                    static_cast<std::ptrdiff_t>(bodyClose + 1));
            context.generics[declaration.name] = std::move(declaration);
            remove.push_back({declarationStart, bodyClose + 1});
''',
    path)
write(path, text)


# ---------------------------------------------------------------------------
# Build and Game SDK metadata
# ---------------------------------------------------------------------------
path = "include/realscript/compiler/Compilation.h"
text = baseline(path)
text = replace_once(
    text,
    "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n",
    "    std::vector<LanguageAttributeRecord> nativeAttributes;\n"
    "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n",
    path)
write(path, text)

path = "src/compiler/Compilation.cpp"
text = baseline(path)
attribute_helpers = '''std::string attributeValueText(
    const syntax::AttributeArgumentSyntax& argument,
    const text::SourceText& source) {
    const auto span = argument.valueSpan();
    return span.empty()
        ? std::string{}
        : std::string(source.view(span));
}

void appendNativeAttributes(
    std::vector<LanguageAttributeRecord>& output,
    const std::vector<syntax::AttributeListSyntax>& lists,
    const std::string& target,
    const text::SourceText& source) {
    for (const auto& list : lists) {
        for (const auto& attribute : list.attributes) {
            LanguageAttributeRecord record;
            record.target = target;
            record.name = attribute.nameToken.text;
            record.sourceName = source.name();
            record.offset = attribute.nameToken.span.start;
            std::size_t positional = 0;
            for (const auto& argument : attribute.arguments) {
                LanguageAttributeArgument value;
                value.name = argument.nameToken
                    ? argument.nameToken->text
                    : "arg" + std::to_string(positional++);
                value.value = attributeValueText(argument, source);
                record.arguments.push_back(std::move(value));
            }
            output.push_back(std::move(record));
        }
    }
}

std::string memberAttributeTarget(
    const std::string& owner,
    const std::string& kind,
    const std::string& name,
    std::size_t arity = 0) {
    std::ostringstream out;
    out << owner << "::" << kind << ':' << name;
    if (kind == "method" || kind == "ctor" ||
        kind == "function") {
        out << '#' << arity;
    }
    return out.str();
}

'''
text = replace_once(
    text,
    "std::string canonicalInterfaceName(\n",
    attribute_helpers + "std::string canonicalInterfaceName(\n",
    path)

collection = '''        // Collect native attributes from original syntax declarations.
        for (const auto* unit : module.units) {
            const auto collectTypeAttributes = [&](auto const& declarations) {
                for (const auto& declaration : declarations) {
                    const auto* owner =
                        findOwnType(module, declaration.identifierToken.text);
                    if (!owner) continue;
                    const auto ownerName = semantic::canonicalTypeName(*owner);
                    appendNativeAttributes(
                        result.nativeAttributes,
                        declaration.attributes,
                        ownerName,
                        *unit->source);
                    for (const auto& field : declaration.fields) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            field.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "field",
                                field.identifierToken.text),
                            *unit->source);
                    }
                    for (const auto& method : declaration.methods) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            method.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "method",
                                method.identifierToken.text,
                                method.parameters.size()),
                            *unit->source);
                    }
                    for (const auto& constructor : declaration.constructors) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            constructor.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "ctor",
                                declaration.identifierToken.text,
                                constructor.parameters.size()),
                            *unit->source);
                    }
                    for (const auto& property : declaration.properties) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            property.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "property",
                                property.identifierToken.text),
                            *unit->source);
                    }
                }
            };
            collectTypeAttributes(unit->syntaxTree->classes);
            collectTypeAttributes(unit->syntaxTree->structs);
            for (const auto& enumeration : unit->syntaxTree->enums) {
                const auto* owner =
                    findOwnType(module, enumeration.identifierToken.text);
                if (!owner) continue;
                const auto ownerName = semantic::canonicalTypeName(*owner);
                appendNativeAttributes(
                    result.nativeAttributes,
                    enumeration.attributes,
                    ownerName,
                    *unit->source);
                for (const auto& member : enumeration.members) {
                    appendNativeAttributes(
                        result.nativeAttributes,
                        member.attributes,
                        memberAttributeTarget(
                            ownerName,
                            "enum",
                            member.identifierToken.text),
                        *unit->source);
                }
            }
            for (const auto& interfaceSyntax : unit->syntaxTree->interfaces) {
                const auto interfaceName = canonicalInterfaceName(
                    moduleName,
                    interfaceSyntax.identifierToken.text);
                appendNativeAttributes(
                    result.nativeAttributes,
                    interfaceSyntax.attributes,
                    interfaceName,
                    *unit->source);
                for (const auto& method : interfaceSyntax.methods) {
                    appendNativeAttributes(
                        result.nativeAttributes,
                        method.attributes,
                        memberAttributeTarget(
                            interfaceName,
                            "method",
                            method.identifierToken.text,
                            method.parameters.size()),
                        *unit->source);
                }
            }
            for (const auto& function : unit->syntaxTree->functions) {
                appendNativeAttributes(
                    result.nativeAttributes,
                    function.attributes,
                    memberAttributeTarget(
                        moduleName,
                        "function",
                        function.identifierToken.text,
                        function.parameters.size()),
                    *unit->source);
            }
        }

'''
text = replace_once(
    text,
    "        const auto validateInterfaces = [&](auto const& declarations) {\n",
    collection +
    "        const auto validateInterfaces = [&](auto const& declarations) {\n",
    path)
text = replace_once(
    text,
    "        for (const auto& implementation : result.nativeInterfaces) {\n",
    '''        for (const auto& attribute : result.nativeAttributes) {
            if (attribute.target.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream attributeSignature;
            attributeSignature << "attribute:"
                << attribute.target << ':' << attribute.name << '(';
            for (const auto& argument : attribute.arguments) {
                attributeSignature << argument.name << '='
                    << argument.value << ',';
            }
            attributeSignature << ')';
            signatures.push_back(attributeSignature.str());
        }
        for (const auto& implementation : result.nativeInterfaces) {
''',
    path)
text = replace_once(
    text,
    "    std::stable_sort(\n        result.nativeInterfaces.begin(),\n",
    '''    std::stable_sort(
        result.nativeAttributes.begin(),
        result.nativeAttributes.end(),
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
    std::stable_sort(
        result.nativeInterfaces.begin(),
''',
    path)
write(path, text)

path = "src/game/GameApi.cpp"
text = baseline(path)
text = replace_once(
    text,
    "    auto build = compilation.build();\n"
    "    result.languageMetadata.interfaces.insert(\n",
    '''    auto build = compilation.build();
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
    result.languageMetadata.interfaces.insert(
''',
    path)
write(path, text)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
path = "tests/phase11_17_language_expansion_tests.cpp"
text = baseline(path)
start = text.index("void testExpansionMetadata() {")
end = text.index(
    "\n}\n\nvoid testExpansionOptionsRefreshExistingSources",
    start) + 3
text = text[:start] + '''void testExpansionMetadata() {
    realscript::compiler::Compilation compilation({{
        "metadata.rs",
        "module Meta; [Replicated(channel = \\\"state\\\")] "
        "class Unit { int health; }"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "native metadata compilation failed:\\n" +
            diagnosticsText(build.diagnostics));
    require(build.nativeAttributes.size() == 1,
        "native source attribute was not captured");
    require(build.nativeAttributes.front().target == "Meta::Unit",
        "source attribute target was not module-qualified");
    require(build.nativeAttributes.front().name == "Replicated",
        "source attribute name changed");
    require(build.nativeAttributes.front().arguments.size() == 1 &&
            build.nativeAttributes.front().arguments.front().name ==
                "channel" &&
            build.nativeAttributes.front().arguments.front().value ==
                "\\\"state\\\"",
        "source attribute arguments were not captured");
}
''' + text[end:]
write(path, text)

path = "tests/phase18_native_control_flow_tests.cpp"
text = baseline(path)
insert = r'''
void testNativeAttributes() {
    realscript::compiler::Compilation compilation({{"attributes.rs", R"(
module Phase18.Attributes;
[Serializable(version = 2), Editor(category = "combat")]
class Unit
{
    [Replicated(channel = "state")]
    int health;

    [Command]
    int Damage(int amount)
    {
        health = health - amount;
        return health;
    }
}
)"}});
    const auto build = compilation.build();
    require(
        !build.diagnostics.hasErrors(),
        "native attributes failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    require(
        build.nativeAttributes.size() == 4,
        "native declaration attributes were not retained");
    require(
        build.nativeAttributes.front().target ==
            "Phase18.Attributes::Unit",
        "native type attribute target was not canonical");
    bool replicated = false;
    bool command = false;
    for (const auto& attribute : build.nativeAttributes) {
        replicated = replicated ||
            (attribute.name == "Replicated" &&
             attribute.target.find("field:health") !=
                 std::string::npos);
        command = command ||
            (attribute.name == "Command" &&
             attribute.target.find("method:Damage#1") !=
                 std::string::npos);
    }
    require(
        replicated && command,
        "native member attribute targets were not retained");
}

void testAttributesBypassExpansion() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "attributes.rs",
        "module Native; [Serializable] class Unit { int health; }");
    require(
        !expansion.changed,
        "native attributes still used source expansion");
}
'''
text = replace_once(
    text,
    "\n} // namespace\n\nint main() {",
    insert + "\n} // namespace\n\nint main() {",
    path)
text = replace_once(
    text,
    '    run("interfaces bypass expansion", testInterfaceBypassesExpansion);\n',
    '    run("interfaces bypass expansion", testInterfaceBypassesExpansion);\n'
    '    run("native attributes", testNativeAttributes);\n'
    '    run("attributes bypass expansion", testAttributesBypassExpansion);\n',
    path)
write(path, text)

print("Native Phase 18 attributes rebuilt from clean interface baseline")
