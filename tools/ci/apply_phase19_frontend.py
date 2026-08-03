#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(path):
    return (ROOT / path).read_text(encoding="utf-8")


def save(path, text):
    (ROOT / path).write_text(text, encoding="utf-8")


def once(path, old, new):
    text = load(path)
    if new in text:
        return
    if old not in text:
        raise RuntimeError(f"anchor missing in {path}: {old[:100]!r}")
    save(path, text.replace(old, new, 1))


# Syntax tokens/nodes.
p = "include/realscript/syntax/Syntax.h"
once(p, """    EventKeyword,
    StaticKeyword,
    GetKeyword,
""", """    EventKeyword,
    StaticKeyword,
    PublicKeyword,
    PrivateKeyword,
    ProtectedKeyword,
    InternalKeyword,
    AbstractKeyword,
    VirtualKeyword,
    OverrideKeyword,
    SealedKeyword,
    BaseKeyword,
    GetKeyword,
""")
once(p, """    MemberCallExpression,
    ThisExpression,
    MemberAccessExpression,
""", """    MemberCallExpression,
    ThisExpression,
    BaseExpression,
    MemberAccessExpression,
""")
once(p, """[[nodiscard]] bool isPrimitiveTypeKeyword(SyntaxKind kind) noexcept;
""", """[[nodiscard]] bool isPrimitiveTypeKeyword(SyntaxKind kind) noexcept;
[[nodiscard]] bool isDeclarationModifier(SyntaxKind kind) noexcept;
""")
once(p, """struct ThisExpressionSyntax final : ExpressionSyntax {
    SyntaxToken thisKeyword;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ThisExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override { return thisKeyword.span; }
};

struct MemberCallExpressionSyntax final : ExpressionSyntax {
""", """struct ThisExpressionSyntax final : ExpressionSyntax {
    SyntaxToken thisKeyword;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ThisExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override { return thisKeyword.span; }
};

struct BaseExpressionSyntax final : ExpressionSyntax {
    SyntaxToken baseKeyword;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::BaseExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override { return baseKeyword.span; }
};

struct MemberCallExpressionSyntax final : ExpressionSyntax {
""")

for name in (
    "FieldDeclarationSyntax", "EventDeclarationSyntax",
    "ConstructorDeclarationSyntax", "PropertyDeclarationSyntax",
    "ClassDeclarationSyntax", "StructDeclarationSyntax",
    "InterfaceMethodDeclarationSyntax", "InterfaceDeclarationSyntax",
    "EnumDeclarationSyntax", "FunctionDeclarationSyntax",
    "DelegateDeclarationSyntax", "SequenceDeclarationSyntax"):
    old = f"struct {name} final : SyntaxNode {{\n    std::vector<AttributeListSyntax> attributes;\n"
    new = old + "    std::vector<SyntaxToken> modifiers;\n"
    once(p, old, new)

once(p, """    SyntaxToken closeParenToken;
    BlockStatementSyntax body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ConstructorDeclaration; }
""", """    SyntaxToken closeParenToken;
    std::optional<SyntaxToken> initializerColonToken;
    std::optional<SyntaxToken> baseKeyword;
    std::optional<SyntaxToken> initializerOpenParenToken;
    std::vector<std::unique_ptr<ExpressionSyntax>> baseArguments;
    std::vector<SyntaxToken> baseArgumentCommaTokens;
    std::optional<SyntaxToken> initializerCloseParenToken;
    BlockStatementSyntax body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ConstructorDeclaration; }
""")
once(p, """    SyntaxToken closeParenToken;
    BlockStatementSyntax body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::FunctionDeclaration; }
""", """    SyntaxToken closeParenToken;
    std::optional<SyntaxToken> semicolonToken;
    BlockStatementSyntax body;

    [[nodiscard]] bool hasBody() const noexcept { return !semicolonToken.has_value(); }
    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::FunctionDeclaration; }
""")

# Parser API.
once(p, """    [[nodiscard]] std::vector<AttributeListSyntax> parseAttributeLists();
""", """    [[nodiscard]] std::vector<AttributeListSyntax> parseAttributeLists();
    [[nodiscard]] std::vector<SyntaxToken> parseModifiers();
""")
once(p, """    [[nodiscard]] ClassDeclarationSyntax parseClassDeclaration(
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] StructDeclarationSyntax parseStructDeclaration(
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration(
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] InterfaceDeclarationSyntax parseInterfaceDeclaration(
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration(
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] DelegateDeclarationSyntax parseDelegateDeclaration(
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] EventDeclarationSyntax parseEventDeclaration(
        std::vector<AttributeListSyntax> attributes);
""", """    [[nodiscard]] ClassDeclarationSyntax parseClassDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] StructDeclarationSyntax parseStructDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] InterfaceDeclarationSyntax parseInterfaceDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers = {});
    [[nodiscard]] DelegateDeclarationSyntax parseDelegateDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] EventDeclarationSyntax parseEventDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
""")
once(p, """    [[nodiscard]] FieldDeclarationSyntax parseFieldDeclaration(
        TypeSyntax type,
        SyntaxToken identifier,
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] FunctionDeclarationSyntax parseFunctionDeclaration(
        std::optional<SyntaxToken> staticKeyword = std::nullopt,
        std::optional<TypeSyntax> returnType = std::nullopt,
        std::optional<SyntaxToken> identifier = std::nullopt,
        std::vector<AttributeListSyntax> attributes = {});
    [[nodiscard]] SequenceDeclarationSyntax parseSequenceDeclaration(
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration(
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] PropertyDeclarationSyntax parsePropertyDeclaration(
        std::optional<SyntaxToken> staticKeyword,
        TypeSyntax type,
        SyntaxToken identifier,
        std::vector<AttributeListSyntax> attributes);
""", """    [[nodiscard]] FieldDeclarationSyntax parseFieldDeclaration(
        TypeSyntax type,
        SyntaxToken identifier,
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] FunctionDeclarationSyntax parseFunctionDeclaration(
        std::vector<SyntaxToken> modifiers = {},
        std::optional<TypeSyntax> returnType = std::nullopt,
        std::optional<SyntaxToken> identifier = std::nullopt,
        std::vector<AttributeListSyntax> attributes = {});
    [[nodiscard]] SequenceDeclarationSyntax parseSequenceDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] PropertyDeclarationSyntax parsePropertyDeclaration(
        std::vector<SyntaxToken> modifiers,
        TypeSyntax type,
        SyntaxToken identifier,
        std::vector<AttributeListSyntax> attributes);
""")

# Syntax facts.
p = "src/syntax/SyntaxFacts.cpp"
once(p, """        RS_KIND(EventKeyword);
        RS_KIND(StaticKeyword);
        RS_KIND(GetKeyword);
""", """        RS_KIND(EventKeyword);
        RS_KIND(StaticKeyword);
        RS_KIND(PublicKeyword);
        RS_KIND(PrivateKeyword);
        RS_KIND(ProtectedKeyword);
        RS_KIND(InternalKeyword);
        RS_KIND(AbstractKeyword);
        RS_KIND(VirtualKeyword);
        RS_KIND(OverrideKeyword);
        RS_KIND(SealedKeyword);
        RS_KIND(BaseKeyword);
        RS_KIND(GetKeyword);
""")
once(p, """        RS_KIND(MemberCallExpression);
        RS_KIND(ThisExpression);
        RS_KIND(MemberAccessExpression);
""", """        RS_KIND(MemberCallExpression);
        RS_KIND(ThisExpression);
        RS_KIND(BaseExpression);
        RS_KIND(MemberAccessExpression);
""")
once(p, """        {"event", SyntaxKind::EventKeyword},
        {"static", SyntaxKind::StaticKeyword},
        {"get", SyntaxKind::GetKeyword},
""", """        {"event", SyntaxKind::EventKeyword},
        {"static", SyntaxKind::StaticKeyword},
        {"public", SyntaxKind::PublicKeyword},
        {"private", SyntaxKind::PrivateKeyword},
        {"protected", SyntaxKind::ProtectedKeyword},
        {"internal", SyntaxKind::InternalKeyword},
        {"abstract", SyntaxKind::AbstractKeyword},
        {"virtual", SyntaxKind::VirtualKeyword},
        {"override", SyntaxKind::OverrideKeyword},
        {"sealed", SyntaxKind::SealedKeyword},
        {"base", SyntaxKind::BaseKeyword},
        {"get", SyntaxKind::GetKeyword},
""")
once(p, """bool isPrimitiveTypeKeyword(SyntaxKind kind) noexcept {
""", """bool isDeclarationModifier(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::StaticKeyword:
    case SyntaxKind::PublicKeyword:
    case SyntaxKind::PrivateKeyword:
    case SyntaxKind::ProtectedKeyword:
    case SyntaxKind::InternalKeyword:
    case SyntaxKind::AbstractKeyword:
    case SyntaxKind::VirtualKeyword:
    case SyntaxKind::OverrideKeyword:
    case SyntaxKind::SealedKeyword:
        return true;
    default:
        return false;
    }
}

bool isPrimitiveTypeKeyword(SyntaxKind kind) noexcept {
""")

# Spans.
p = "src/syntax/SyntaxNodes.cpp"
once(p, """text::TextSpan FunctionDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, returnType.span()),
        body.span());
}
""", """text::TextSpan FunctionDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, returnType.span()),
        semicolonToken ? semicolonToken->span : body.span());
}
""")

# Parser.
p = "src/syntax/Parser.cpp"
once(p, """#include "realscript/syntax/Syntax.h"
""", """#include "realscript/syntax/Syntax.h"

#include <algorithm>
""")
once(p, """bool isIdentifierLike(SyntaxKind kind) noexcept {
    return kind == SyntaxKind::IdentifierToken || isPrimitiveTypeKeyword(kind);
}
""", """bool isIdentifierLike(SyntaxKind kind) noexcept {
    return kind == SyntaxKind::IdentifierToken || isPrimitiveTypeKeyword(kind);
}

std::optional<SyntaxToken> findModifier(
    const std::vector<SyntaxToken>& modifiers,
    SyntaxKind kind) {
    for (const auto& modifier : modifiers) {
        if (modifier.kind == kind) return modifier;
    }
    return std::nullopt;
}
""")
once(p, """        auto attributes = parseAttributeLists();
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
        } else if (current().kind == SyntaxKind::DelegateKeyword) {
            result.delegates.push_back(
                parseDelegateDeclaration(std::move(attributes)));
        } else {
            result.functions.push_back(parseFunctionDeclaration(
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::move(attributes)));
        }
""", """        auto attributes = parseAttributeLists();
        auto modifiers = parseModifiers();
        if (current().kind == SyntaxKind::ClassKeyword) {
            result.classes.push_back(parseClassDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else if (current().kind == SyntaxKind::StructKeyword) {
            result.structs.push_back(parseStructDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else if (current().kind == SyntaxKind::EnumKeyword) {
            result.enums.push_back(parseEnumDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else if (current().kind == SyntaxKind::InterfaceKeyword) {
            result.interfaces.push_back(parseInterfaceDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else if (current().kind == SyntaxKind::DelegateKeyword) {
            result.delegates.push_back(parseDelegateDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else {
            result.functions.push_back(parseFunctionDeclaration(
                std::move(modifiers), std::nullopt, std::nullopt,
                std::move(attributes)));
        }
""")
once(p, """std::vector<AttributeListSyntax> Parser::parseAttributeLists() {
""", """std::vector<SyntaxToken> Parser::parseModifiers() {
    std::vector<SyntaxToken> result;
    while (isDeclarationModifier(current().kind)) {
        auto modifier = nextToken();
        if (std::any_of(result.begin(), result.end(),
                [&](const auto& existing) {
                    return existing.kind == modifier.kind;
                })) {
            diagnostics_.report(
                "RS1116",
                "duplicate declaration modifier '" + modifier.text + "'",
                modifier.span);
        }
        result.push_back(std::move(modifier));
    }
    return result;
}

std::vector<AttributeListSyntax> Parser::parseAttributeLists() {
""")

# Simple declaration signatures.
replacements = [
("DelegateDeclarationSyntax Parser::parseDelegateDeclaration(\n    std::vector<AttributeListSyntax> attributes) {\n    DelegateDeclarationSyntax result;\n    result.attributes = std::move(attributes);",
 "DelegateDeclarationSyntax Parser::parseDelegateDeclaration(\n    std::vector<AttributeListSyntax> attributes,\n    std::vector<SyntaxToken> modifiers) {\n    DelegateDeclarationSyntax result;\n    result.attributes = std::move(attributes);\n    result.modifiers = std::move(modifiers);"),
("EventDeclarationSyntax Parser::parseEventDeclaration(\n    std::vector<AttributeListSyntax> attributes) {\n    EventDeclarationSyntax result;\n    result.attributes = std::move(attributes);",
 "EventDeclarationSyntax Parser::parseEventDeclaration(\n    std::vector<AttributeListSyntax> attributes,\n    std::vector<SyntaxToken> modifiers) {\n    EventDeclarationSyntax result;\n    result.attributes = std::move(attributes);\n    result.modifiers = std::move(modifiers);"),
("SequenceDeclarationSyntax Parser::parseSequenceDeclaration(\n    std::vector<AttributeListSyntax> attributes) {\n    SequenceDeclarationSyntax result;\n    result.attributes = std::move(attributes);",
 "SequenceDeclarationSyntax Parser::parseSequenceDeclaration(\n    std::vector<AttributeListSyntax> attributes,\n    std::vector<SyntaxToken> modifiers) {\n    SequenceDeclarationSyntax result;\n    result.attributes = std::move(attributes);\n    result.modifiers = std::move(modifiers);"),
("ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(\n    std::vector<AttributeListSyntax> attributes) {\n    ConstructorDeclarationSyntax result;\n    result.attributes = std::move(attributes);",
 "ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(\n    std::vector<AttributeListSyntax> attributes,\n    std::vector<SyntaxToken> modifiers) {\n    ConstructorDeclarationSyntax result;\n    result.attributes = std::move(attributes);\n    result.modifiers = std::move(modifiers);"),
("InterfaceMethodDeclarationSyntax Parser::parseInterfaceMethodDeclaration(\n    std::vector<AttributeListSyntax> attributes) {\n    InterfaceMethodDeclarationSyntax result;\n    result.attributes = std::move(attributes);",
 "InterfaceMethodDeclarationSyntax Parser::parseInterfaceMethodDeclaration(\n    std::vector<AttributeListSyntax> attributes,\n    std::vector<SyntaxToken> modifiers) {\n    InterfaceMethodDeclarationSyntax result;\n    result.attributes = std::move(attributes);\n    result.modifiers = std::move(modifiers);"),
]
for old, new in replacements:
    once(p, old, new)

once(p, """FieldDeclarationSyntax Parser::parseFieldDeclaration(
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes) {
    FieldDeclarationSyntax result;
    result.attributes = std::move(attributes);
""", """FieldDeclarationSyntax Parser::parseFieldDeclaration(
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    FieldDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
""")
once(p, """PropertyDeclarationSyntax Parser::parsePropertyDeclaration(
    std::optional<SyntaxToken> staticKeyword,
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes) {
    PropertyDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.staticKeyword = std::move(staticKeyword);
""", """PropertyDeclarationSyntax Parser::parsePropertyDeclaration(
    std::vector<SyntaxToken> modifiers,
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes) {
    PropertyDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.staticKeyword = findModifier(result.modifiers, SyntaxKind::StaticKeyword);
""")
once(p, """FunctionDeclarationSyntax Parser::parseFunctionDeclaration(
    std::optional<SyntaxToken> staticKeyword,
    std::optional<TypeSyntax> returnType,
    std::optional<SyntaxToken> identifier,
    std::vector<AttributeListSyntax> attributes) {
    FunctionDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.staticKeyword = std::move(staticKeyword);
""", """FunctionDeclarationSyntax Parser::parseFunctionDeclaration(
    std::vector<SyntaxToken> modifiers,
    std::optional<TypeSyntax> returnType,
    std::optional<SyntaxToken> identifier,
    std::vector<AttributeListSyntax> attributes) {
    FunctionDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.staticKeyword = findModifier(result.modifiers, SyntaxKind::StaticKeyword);
""")

# Class/struct/interface/enum declarations.
for kind in ("Class", "Struct", "Interface", "Enum"):
    lower = kind.lower()
    old = f"{kind}DeclarationSyntax Parser::parse{kind}Declaration(\n    std::vector<AttributeListSyntax> attributes) {{\n    {kind}DeclarationSyntax result;\n    result.attributes = std::move(attributes);"
    new = f"{kind}DeclarationSyntax Parser::parse{kind}Declaration(\n    std::vector<AttributeListSyntax> attributes,\n    std::vector<SyntaxToken> modifiers) {{\n    {kind}DeclarationSyntax result;\n    result.attributes = std::move(attributes);\n    result.modifiers = std::move(modifiers);"
    once(p, old, new)

# Abstract method body and base constructor initializer.
once(p, """    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    result.body = parseBlockStatement();
    return result;
}

ClassDeclarationSyntax Parser::parseClassDeclaration(
""", """    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    if (current().kind == SyntaxKind::SemicolonToken) {
        result.semicolonToken = nextToken();
    } else {
        result.body = parseBlockStatement();
    }
    return result;
}

ClassDeclarationSyntax Parser::parseClassDeclaration(
""")
# Constructor occurrence precedes AccessorDeclaration and still has unconditional body.
once(p, """    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    result.body = parseBlockStatement();
    return result;
}

AccessorDeclarationSyntax Parser::parseAccessorDeclaration() {
""", """    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    if (current().kind == SyntaxKind::ColonToken) {
        result.initializerColonToken = nextToken();
        result.baseKeyword = match(SyntaxKind::BaseKeyword);
        result.initializerOpenParenToken = match(SyntaxKind::OpenParenToken);
        SyntaxToken close;
        parseArgumentList(
            result.baseArguments, nullptr,
            result.baseArgumentCommaTokens, close);
        result.initializerCloseParenToken = std::move(close);
    }
    result.body = parseBlockStatement();
    return result;
}

AccessorDeclarationSyntax Parser::parseAccessorDeclaration() {
""")

# Replace class/struct member loop blocks by marker-based transformation.
text = load(p)
old_block = '''        auto memberAttributes = parseAttributeLists();
        if (current().kind == SyntaxKind::EventKeyword) {
            result.events.push_back(parseEventDeclaration(
                std::move(memberAttributes)));
            continue;
        }
        std::optional<SyntaxToken> staticKeyword;
        if (current().kind == SyntaxKind::StaticKeyword) {
            staticKeyword = nextToken();
        }
        if (!staticKeyword &&
            current().kind == SyntaxKind::SequenceKeyword) {
            result.sequences.push_back(parseSequenceDeclaration(
                std::move(memberAttributes)));
        } else if (!staticKeyword && current().kind == SyntaxKind::IdentifierToken &&
            current().text == typeName && peek(1).kind == SyntaxKind::OpenParenToken) {
            result.constructors.push_back(parseConstructorDeclaration(
                std::move(memberAttributes)));
        } else {
            auto type = parseType();
            auto identifier = match(SyntaxKind::IdentifierToken);
            if (current().kind == SyntaxKind::OpenParenToken) {
                result.methods.push_back(parseFunctionDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
            } else if (current().kind == SyntaxKind::OpenBraceToken) {
                result.properties.push_back(parsePropertyDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
            } else {
                if (staticKeyword) {
                    diagnostics_.report("RS1106", "static fields are not supported", staticKeyword->span);
                }
                result.fields.push_back(parseFieldDeclaration(
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
            }
        }
'''
old_struct = old_block.replace(
'''        if (current().kind == SyntaxKind::StaticKeyword) {
            staticKeyword = nextToken();
        }
''', '''        if (current().kind == SyntaxKind::StaticKeyword) staticKeyword = nextToken();
''').replace(
'''                if (staticKeyword) {
                    diagnostics_.report("RS1106", "static fields are not supported", staticKeyword->span);
                }
                result.fields.push_back(parseFieldDeclaration(
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
''', '''                if (staticKeyword) diagnostics_.report("RS1106", "static fields are not supported", staticKeyword->span);
                result.fields.push_back(parseFieldDeclaration(std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
''')
new_block = '''        auto memberAttributes = parseAttributeLists();
        auto memberModifiers = parseModifiers();
        if (current().kind == SyntaxKind::EventKeyword) {
            result.events.push_back(parseEventDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
            continue;
        }
        const auto isStatic = findModifier(
            memberModifiers, SyntaxKind::StaticKeyword).has_value();
        if (!isStatic && current().kind == SyntaxKind::SequenceKeyword) {
            result.sequences.push_back(parseSequenceDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
        } else if (!isStatic && current().kind == SyntaxKind::IdentifierToken &&
            current().text == typeName && peek(1).kind == SyntaxKind::OpenParenToken) {
            result.constructors.push_back(parseConstructorDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
        } else {
            auto type = parseType();
            auto identifier = match(SyntaxKind::IdentifierToken);
            if (current().kind == SyntaxKind::OpenParenToken ||
                current().kind == SyntaxKind::SemicolonToken) {
                result.methods.push_back(parseFunctionDeclaration(
                    std::move(memberModifiers), std::move(type),
                    std::move(identifier), std::move(memberAttributes)));
            } else if (current().kind == SyntaxKind::OpenBraceToken) {
                result.properties.push_back(parsePropertyDeclaration(
                    std::move(memberModifiers), std::move(type),
                    std::move(identifier), std::move(memberAttributes)));
            } else {
                if (isStatic) diagnostics_.report(
                    "RS1106", "static fields are not supported",
                    identifier.span);
                result.fields.push_back(parseFieldDeclaration(
                    std::move(type), std::move(identifier),
                    std::move(memberAttributes), std::move(memberModifiers)));
            }
        }
'''
if old_block not in text or old_struct not in text:
    raise RuntimeError("class/struct member loop anchors missing")
text = text.replace(old_block, new_block, 1)
text = text.replace(old_struct, new_block, 1)
save(p, text)

# Interface members.
once(p, """        result.methods.push_back(parseInterfaceMethodDeclaration(
            parseAttributeLists()));
""", """        auto memberAttributes = parseAttributeLists();
        auto memberModifiers = parseModifiers();
        result.methods.push_back(parseInterfaceMethodDeclaration(
            std::move(memberAttributes), std::move(memberModifiers)));
""")

# Base primary expression.
once(p, """    case SyntaxKind::ThisKeyword: {
        auto result = std::make_unique<ThisExpressionSyntax>();
        result->thisKeyword = nextToken();
        return result;
    }
    case SyntaxKind::OpenParenToken: {
""", """    case SyntaxKind::ThisKeyword: {
        auto result = std::make_unique<ThisExpressionSyntax>();
        result->thisKeyword = nextToken();
        return result;
    }
    case SyntaxKind::BaseKeyword: {
        auto result = std::make_unique<BaseExpressionSyntax>();
        result->baseKeyword = nextToken();
        return result;
    }
    case SyntaxKind::OpenParenToken: {
""")

# Test.
(ROOT / "tests/phase19_runtime_polymorphism_tests.cpp").write_text(r'''#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool has(const std::vector<realscript::syntax::SyntaxToken>& values,
         realscript::syntax::SyntaxKind kind) {
    return std::any_of(values.begin(), values.end(),
        [&](const auto& value) { return value.kind == kind; });
}
void testSyntax() {
    const char* source = R"(
module Phase19.Syntax;
public interface IEntity { public int Kind(); }
public abstract class Unit : IEntity
{
    protected int health;
    protected Unit(int initial) : base() { health = initial; }
    public virtual int Power() { return health; }
    public abstract int Kind();
}
internal sealed class Marine : Unit, IEntity
{
    public Marine(int initial) : base(initial) { }
    public sealed override int Power() { return base.Power() + 2; }
    public override int Kind() { return 1; }
}
)";
    realscript::text::SourceText text(source, "phase19.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(text, diagnostics);
    auto unit = parser.parseCompilationUnit();
    require(!diagnostics.hasErrors(), "Phase 19 parser produced diagnostics");
    require(unit.classes.size() == 2 && unit.interfaces.size() == 1,
        "Phase 19 declarations were not parsed");
    const auto& base = unit.classes.front();
    require(has(base.modifiers, realscript::syntax::SyntaxKind::PublicKeyword) &&
            has(base.modifiers, realscript::syntax::SyntaxKind::AbstractKeyword),
        "class modifiers were lost");
    require(base.interfaces.size() == 1 &&
            base.fields.size() == 1 &&
            has(base.fields.front().modifiers,
                realscript::syntax::SyntaxKind::ProtectedKeyword),
        "base list or field modifiers were lost");
    require(base.constructors.front().baseKeyword.has_value() &&
            base.methods.back().semicolonToken.has_value() &&
            has(base.methods.back().modifiers,
                realscript::syntax::SyntaxKind::AbstractKeyword),
        "base initializer or abstract declaration was lost");
    const auto& derived = unit.classes.back();
    require(derived.interfaces.size() == 2 &&
            derived.constructors.front().baseArguments.size() == 1 &&
            has(derived.methods.front().modifiers,
                realscript::syntax::SyntaxKind::OverrideKeyword),
        "derived syntax was not retained");
    const auto& returned = static_cast<const realscript::syntax::ReturnStatementSyntax&>(
        *derived.methods.front().body.statements.front());
    const auto& binary = static_cast<const realscript::syntax::BinaryExpressionSyntax&>(
        *returned.expression);
    const auto& call = static_cast<const realscript::syntax::MemberCallExpressionSyntax&>(
        *binary.left);
    require(call.receiver->kind() == realscript::syntax::SyntaxKind::BaseExpression,
        "base expression did not survive parsing");
}
void testDuplicateModifier() {
    realscript::text::SourceText text(
        "public public class Bad {}", "bad.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(text, diagnostics);
    (void)parser.parseCompilationUnit();
    require(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
        [](const auto& value) { return value.code == "RS1116"; }),
        "duplicate modifier did not report RS1116");
}
}
int main() {
    try {
        testSyntax();
        testDuplicateModifier();
        std::cout << "Phase 19 frontend tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
''', encoding="utf-8")

p = "CMakeLists.txt"
once(p, """    realscript_add_cpp_test(
        realscript_phase18_native_control_flow_tests
        realscript.phase18.native-control-flow
        tests/phase18_native_control_flow_tests.cpp)
""", """    realscript_add_cpp_test(
        realscript_phase18_native_control_flow_tests
        realscript.phase18.native-control-flow
        tests/phase18_native_control_flow_tests.cpp)
    realscript_add_cpp_test(
        realscript_phase19_runtime_polymorphism_tests
        realscript.phase19.runtime-polymorphism
        tests/phase19_runtime_polymorphism_tests.cpp)
""")
