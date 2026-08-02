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


# Public metadata --------------------------------------------------------------
path = "include/realscript/compiler/LanguageExpansion.h"
text = read(path)
if "sourceName;" not in text[text.index("struct LanguageAttributeRecord"):text.index("struct LanguageInterfaceImplementation")]:
    text = text.replace(
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
        1)
text = text.replace("    bool sourceAttributes = true;", "    bool sourceAttributes = false;")
write(path, text)

# Syntax model ----------------------------------------------------------------
path = "include/realscript/syntax/Syntax.h"
text = read(path)
if "AttributeList" not in text[text.index("CompilationUnit"):text.index("ModuleKeyword")]:
    text = text.replace(
        "    CompilationUnit,\n    ModuleDeclaration,",
        "    CompilationUnit,\n"
        "    AttributeList,\n"
        "    Attribute,\n"
        "    AttributeArgument,\n"
        "    ModuleDeclaration,",
        1)

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
    text = text.replace(
        "struct ExpressionSyntax : SyntaxNode {};\nstruct StatementSyntax : SyntaxNode {};\n",
        "struct ExpressionSyntax : SyntaxNode {};\n"
        "struct StatementSyntax : SyntaxNode {};\n\n" + attribute_nodes,
        1)

    # Declaration attribute fields.
    declarations = [
        ("struct FieldDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct ConstructorDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct PropertyDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct ClassDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct StructDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct InterfaceMethodDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct InterfaceDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct EnumMemberDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct EnumDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
        ("struct FunctionDeclarationSyntax final : SyntaxNode {\n", "    std::vector<AttributeListSyntax> attributes;\n"),
    ]
    for anchor, field in declarations:
        if anchor not in text:
            raise RuntimeError(f"syntax declaration anchor missing: {anchor}")
        text = text.replace(anchor, anchor + field, 1)

    # Parser declarations.
    text = text.replace(
        "    [[nodiscard]] ClassDeclarationSyntax parseClassDeclaration();\n",
        "    [[nodiscard]] std::vector<AttributeListSyntax> parseAttributeLists();\n"
        "    [[nodiscard]] AttributeListSyntax parseAttributeList();\n"
        "    [[nodiscard]] AttributeSyntax parseAttribute();\n"
        "    [[nodiscard]] AttributeArgumentSyntax parseAttributeArgument();\n"
        "    [[nodiscard]] ClassDeclarationSyntax parseClassDeclaration(\n"
        "        std::vector<AttributeListSyntax> attributes);\n",
        1)
    text = text.replace(
        "    [[nodiscard]] StructDeclarationSyntax parseStructDeclaration();\n",
        "    [[nodiscard]] StructDeclarationSyntax parseStructDeclaration(\n"
        "        std::vector<AttributeListSyntax> attributes);\n",
        1)
    text = text.replace(
        "    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration();\n",
        "    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration(\n"
        "        std::vector<AttributeListSyntax> attributes);\n",
        1)
    text = text.replace(
        "    [[nodiscard]] InterfaceDeclarationSyntax parseInterfaceDeclaration();\n",
        "    [[nodiscard]] InterfaceDeclarationSyntax parseInterfaceDeclaration(\n"
        "        std::vector<AttributeListSyntax> attributes);\n",
        1)
    text = text.replace(
        "    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration();\n",
        "    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration(\n"
        "        std::vector<AttributeListSyntax> attributes);\n",
        1)
    text = text.replace(
        "    [[nodiscard]] FieldDeclarationSyntax parseFieldDeclaration(TypeSyntax type, SyntaxToken identifier);\n",
        "    [[nodiscard]] FieldDeclarationSyntax parseFieldDeclaration(\n"
        "        TypeSyntax type, SyntaxToken identifier,\n"
        "        std::vector<AttributeListSyntax> attributes);\n",
        1)
    text = text.replace(
        "        std::optional<SyntaxToken> identifier = std::nullopt);\n",
        "        std::optional<SyntaxToken> identifier = std::nullopt,\n"
        "        std::vector<AttributeListSyntax> attributes = {});\n",
        1)
    text = text.replace(
        "    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration();\n",
        "    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration(\n"
        "        std::vector<AttributeListSyntax> attributes);\n",
        1)
    text = text.replace(
        "        TypeSyntax type,\n        SyntaxToken identifier);\n",
        "        TypeSyntax type,\n"
        "        SyntaxToken identifier,\n"
        "        std::vector<AttributeListSyntax> attributes);\n",
        1)
    write(path, text)

# Syntax facts ----------------------------------------------------------------
path = "src/syntax/SyntaxFacts.cpp"
text = read(path)
if "RS_KIND(AttributeList)" not in text:
    text = text.replace(
        "        RS_KIND(CompilationUnit);\n        RS_KIND(ModuleDeclaration);",
        "        RS_KIND(CompilationUnit);\n"
        "        RS_KIND(AttributeList);\n"
        "        RS_KIND(Attribute);\n"
        "        RS_KIND(AttributeArgument);\n"
        "        RS_KIND(ModuleDeclaration);",
        1)
    write(path, text)

# Syntax spans ----------------------------------------------------------------
path = "src/syntax/SyntaxNodes.cpp"
text = read(path)
if "AttributeArgumentSyntax::valueSpan" not in text:
    helper = '''text::TextSpan declarationStart(
    const std::vector<AttributeListSyntax>& attributes,
    text::TextSpan fallback) noexcept {
    return attributes.empty() ? fallback
        : text::TextSpan::fromBounds(
            attributes.front().span().start,
            fallback.end());
}

'''
    text = text.replace(
        "std::string joinQualifiedName(const std::vector<SyntaxToken>& parts) {",
        helper + "std::string joinQualifiedName(const std::vector<SyntaxToken>& parts) {",
        1)
    nodes = '''text::TextSpan AttributeArgumentSyntax::valueSpan() const noexcept {
    if (valueTokens.empty()) return {};
    return combine(valueTokens.front().span, valueTokens.back().span);
}

text::TextSpan AttributeArgumentSyntax::span() const noexcept {
    const auto value = valueSpan();
    if (nameToken) return combine(nameToken->span, value.empty() ? nameToken->span : value);
    return value;
}

text::TextSpan AttributeSyntax::span() const noexcept {
    return closeParenToken ? combine(nameToken.span, closeParenToken->span)
                           : nameToken.span;
}

text::TextSpan AttributeListSyntax::span() const noexcept {
    return combine(openBracketToken.span, closeBracketToken.span);
}

'''
    text = text.replace(
        "text::TextSpan TypeSyntax::span() const noexcept {",
        nodes + "text::TextSpan TypeSyntax::span() const noexcept {",
        1)

    replacements = {
        "return combine(type.span(), semicolonToken.span);\n}": "return combine(declarationStart(attributes, type.span()), semicolonToken.span);\n}",
        "return combine(identifierToken.span, body.span());\n}": "return combine(declarationStart(attributes, identifierToken.span), body.span());\n}",
        "return combine(type.span(), closeBraceToken.span);\n}": "return combine(declarationStart(attributes, type.span()), closeBraceToken.span);\n}",
        "return combine(classKeyword.span, closeBraceToken.span);\n}": "return combine(declarationStart(attributes, classKeyword.span), closeBraceToken.span);\n}",
        "return combine(structKeyword.span, closeBraceToken.span);\n}": "return combine(declarationStart(attributes, structKeyword.span), closeBraceToken.span);\n}",
        "return combine(returnType.span(), semicolonToken.span);\n}": "return combine(declarationStart(attributes, returnType.span()), semicolonToken.span);\n}",
        "return combine(interfaceKeyword.span, closeBraceToken.span);\n}": "return combine(declarationStart(attributes, interfaceKeyword.span), closeBraceToken.span);\n}",
        "return combine(enumKeyword.span, closeBraceToken.span);\n}": "return combine(declarationStart(attributes, enumKeyword.span), closeBraceToken.span);\n}",
        "return combine(returnType.span(), body.span());\n}": "return combine(declarationStart(attributes, returnType.span()), body.span());\n}",
    }
    for old, new in replacements.items():
        if old in text:
            text = text.replace(old, new, 1)
    # Enum member has conditional return; prepend attributes at the end.
    old_enum = '''text::TextSpan EnumMemberDeclarationSyntax::span() const noexcept {
    if (commaToken) return combine(identifierToken.span, commaToken->span);
    if (valueToken) return combine(identifierToken.span, valueToken->span);
    return identifierToken.span;
}
'''
    new_enum = '''text::TextSpan EnumMemberDeclarationSyntax::span() const noexcept {
    text::TextSpan end = identifierToken.span;
    if (commaToken) end = commaToken->span;
    else if (valueToken) end = valueToken->span;
    return combine(declarationStart(attributes, identifierToken.span), end);
}
'''
    if old_enum in text:
        text = text.replace(old_enum, new_enum, 1)
    write(path, text)

# Parser implementation -------------------------------------------------------
path = "src/syntax/Parser.cpp"
text = read(path)
if "Parser::parseAttributeLists" not in text:
    # Top-level declaration dispatch.
    old_loop = '''    while (current().kind != SyntaxKind::EndOfFileToken) {
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
'''
    new_loop = '''    while (current().kind != SyntaxKind::EndOfFileToken) {
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
'''
    if old_loop not in text:
        raise RuntimeError("top-level parser loop anchor missing")
    text = text.replace(old_loop, new_loop, 1)

    # Attribute parsing before Type parser.
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
        else if (current().kind == SyntaxKind::CloseParenToken && parens > 0) --parens;
        else if (current().kind == SyntaxKind::OpenBracketToken) ++brackets;
        else if (current().kind == SyntaxKind::CloseBracketToken && brackets > 0) --brackets;
        else if (current().kind == SyntaxKind::OpenBraceToken) ++braces;
        else if (current().kind == SyntaxKind::CloseBraceToken && braces > 0) --braces;
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
    text = text.replace(
        "TypeSyntax Parser::parseType() {",
        attribute_parser + "TypeSyntax Parser::parseType() {",
        1)

    # Function signatures and assignments.
    text = text.replace(
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
        1)
    text = text.replace(
        '''ConstructorDeclarationSyntax Parser::parseConstructorDeclaration() {
    ConstructorDeclarationSyntax result;
''',
        '''ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    ConstructorDeclarationSyntax result;
    result.attributes = std::move(attributes);
''',
        1)
    text = text.replace(
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
        1)
    text = text.replace(
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
        1)
    text = text.replace(
        "ClassDeclarationSyntax Parser::parseClassDeclaration() {\n    ClassDeclarationSyntax result;\n",
        "ClassDeclarationSyntax Parser::parseClassDeclaration(\n"
        "    std::vector<AttributeListSyntax> attributes) {\n"
        "    ClassDeclarationSyntax result;\n"
        "    result.attributes = std::move(attributes);\n",
        1)
    text = text.replace(
        "StructDeclarationSyntax Parser::parseStructDeclaration() {\n    StructDeclarationSyntax result;\n",
        "StructDeclarationSyntax Parser::parseStructDeclaration(\n"
        "    std::vector<AttributeListSyntax> attributes) {\n"
        "    StructDeclarationSyntax result;\n"
        "    result.attributes = std::move(attributes);\n",
        1)
    text = text.replace(
        "InterfaceMethodDeclarationSyntax Parser::parseInterfaceMethodDeclaration() {\n    InterfaceMethodDeclarationSyntax result;\n",
        "InterfaceMethodDeclarationSyntax Parser::parseInterfaceMethodDeclaration(\n"
        "    std::vector<AttributeListSyntax> attributes) {\n"
        "    InterfaceMethodDeclarationSyntax result;\n"
        "    result.attributes = std::move(attributes);\n",
        1)
    text = text.replace(
        "InterfaceDeclarationSyntax Parser::parseInterfaceDeclaration() {\n    InterfaceDeclarationSyntax result;\n",
        "InterfaceDeclarationSyntax Parser::parseInterfaceDeclaration(\n"
        "    std::vector<AttributeListSyntax> attributes) {\n"
        "    InterfaceDeclarationSyntax result;\n"
        "    result.attributes = std::move(attributes);\n",
        1)
    text = text.replace(
        "EnumDeclarationSyntax Parser::parseEnumDeclaration() {\n    EnumDeclarationSyntax result;\n",
        "EnumDeclarationSyntax Parser::parseEnumDeclaration(\n"
        "    std::vector<AttributeListSyntax> attributes) {\n"
        "    EnumDeclarationSyntax result;\n"
        "    result.attributes = std::move(attributes);\n",
        1)

    # Class member attributes.
    text = text.replace(
        '''        const auto before = position_;
        std::optional<SyntaxToken> staticKeyword;
''',
        '''        const auto before = position_;
        auto memberAttributes = parseAttributeLists();
        std::optional<SyntaxToken> staticKeyword;
''',
        1)
    text = text.replace(
        "            result.constructors.push_back(parseConstructorDeclaration());\n",
        "            result.constructors.push_back(parseConstructorDeclaration(\n"
        "                std::move(memberAttributes)));\n",
        1)
    text = text.replace(
        '''                result.methods.push_back(parseFunctionDeclaration(
                    std::move(staticKeyword), std::move(type), std::move(identifier)));
''',
        '''                result.methods.push_back(parseFunctionDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
''',
        1)
    text = text.replace(
        '''                result.properties.push_back(parsePropertyDeclaration(
                    std::move(staticKeyword), std::move(type), std::move(identifier)));
''',
        '''                result.properties.push_back(parsePropertyDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
''',
        1)
    text = text.replace(
        '''                result.fields.push_back(parseFieldDeclaration(
                    std::move(type), std::move(identifier)));
''',
        '''                result.fields.push_back(parseFieldDeclaration(
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
''',
        1)

    # Struct member attributes (second loop occurrence).
    struct_anchor = '''        const auto before = position_;
        std::optional<SyntaxToken> staticKeyword;
'''
    if struct_anchor not in text:
        raise RuntimeError("struct member attribute anchor missing")
    text = text.replace(
        struct_anchor,
        '''        const auto before = position_;
        auto memberAttributes = parseAttributeLists();
        std::optional<SyntaxToken> staticKeyword;
''',
        1)
    text = text.replace(
        "            result.constructors.push_back(parseConstructorDeclaration());\n",
        "            result.constructors.push_back(parseConstructorDeclaration(\n"
        "                std::move(memberAttributes)));\n",
        1)
    text = text.replace(
        '''                result.methods.push_back(parseFunctionDeclaration(
                    std::move(staticKeyword), std::move(type), std::move(identifier)));
''',
        '''                result.methods.push_back(parseFunctionDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
''',
        1)
    text = text.replace(
        '''                result.properties.push_back(parsePropertyDeclaration(
                    std::move(staticKeyword), std::move(type), std::move(identifier)));
''',
        '''                result.properties.push_back(parsePropertyDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
''',
        1)
    text = text.replace(
        "                result.fields.push_back(parseFieldDeclaration(std::move(type), std::move(identifier)));\n",
        "                result.fields.push_back(parseFieldDeclaration(\n"
        "                    std::move(type),\n"
        "                    std::move(identifier),\n"
        "                    std::move(memberAttributes)));\n",
        1)

    # Interface method attributes.
    text = text.replace(
        "        result.methods.push_back(parseInterfaceMethodDeclaration());\n",
        "        result.methods.push_back(parseInterfaceMethodDeclaration(\n"
        "            parseAttributeLists()));\n",
        1)

    # Enum member attributes.
    text = text.replace(
        '''        EnumMemberDeclarationSyntax member;
        member.identifierToken = match(SyntaxKind::IdentifierToken);
''',
        '''        EnumMemberDeclarationSyntax member;
        member.attributes = parseAttributeLists();
        member.identifierToken = match(SyntaxKind::IdentifierToken);
''',
        1)
    write(path, text)

# Preserve attributes on generic declarations --------------------------------
path = "src/compiler/LanguageExpansion.cpp"
text = read(path)
if "genericDeclarationStart" not in text:
    helper = '''std::size_t genericDeclarationStart(
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

'''
    text = text.replace(
        "void collectGenericDeclarationsWithInterfaces(\n",
        helper + "void collectGenericDeclarationsWithInterfaces(\n",
        1)
    old = '''            GenericDecl declaration;
            declaration.kind = GenericDecl::Kind::Type;
            declaration.name = tokens[index + 1].text;
            declaration.parameters = parseTypeParameterNames(
                tokens, index + 2, angleClose);
            declaration.tokens.assign(
                tokens.begin() + static_cast<std::ptrdiff_t>(index),
                tokens.begin() + static_cast<std::ptrdiff_t>(bodyClose + 1));
            context.generics[declaration.name] = std::move(declaration);
            remove.push_back({index, bodyClose + 1});
'''
    new = '''            const auto declarationStart =
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
'''
    if old not in text:
        raise RuntimeError("generic attribute preservation anchor missing")
    text = text.replace(old, new, 1)
    write(path, text)

# Build metadata ---------------------------------------------------------------
path = "include/realscript/compiler/Compilation.h"
text = read(path)
if "nativeAttributes" not in text:
    text = text.replace(
        "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n",
        "    std::vector<LanguageAttributeRecord> nativeAttributes;\n"
        "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n",
        1)
    write(path, text)

path = "src/compiler/Compilation.cpp"
text = read(path)
if "appendNativeAttributes" not in text:
    helper = '''std::string attributeValueText(
    const syntax::AttributeArgumentSyntax& argument,
    const text::SourceText& source) {
    const auto span = argument.valueSpan();
    return span.empty() ? std::string{} : std::string(source.view(span));
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
    if (kind == "method" || kind == "ctor") out << '#' << arity;
    return out.str();
}

'''
    text = text.replace(
        "std::string canonicalInterfaceName(\n",
        helper + "std::string canonicalInterfaceName(\n",
        1)

    collect_anchor = '''        const auto validateInterfaces = [&](auto const& declarations) {
'''
    collector = '''        // Collect native source attributes from original syntax declarations.
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
                                ownerName, "field", field.identifierToken.text),
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

        const auto validateInterfaces = [&](auto const& declarations) {
'''
    if collect_anchor not in text:
        raise RuntimeError("native attribute collection anchor missing")
    text = text.replace(collect_anchor, collector, 1)

    # Include attrs in fingerprints and sort at build end.
    fingerprint_anchor = '''        for (const auto& implementation : result.nativeInterfaces) {
'''
    attrs_fingerprint = '''        for (const auto& attribute : result.nativeAttributes) {
            if (attribute.target.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream attributeSignature;
            attributeSignature << "attribute:" << attribute.target
                << ':' << attribute.name << '(';
            for (const auto& argument : attribute.arguments) {
                attributeSignature << argument.name << '='
                    << argument.value << ',';
            }
            attributeSignature << ')';
            signatures.push_back(attributeSignature.str());
        }
        for (const auto& implementation : result.nativeInterfaces) {
'''
    text = text.replace(fingerprint_anchor, attrs_fingerprint, 1)
    sort_anchor = '''    std::stable_sort(
        result.nativeInterfaces.begin(),
'''
    sort_attrs = '''    std::stable_sort(
        result.nativeAttributes.begin(),
        result.nativeAttributes.end(),
        [](const auto& left, const auto& right) {
            if (left.target != right.target) return left.target < right.target;
            if (left.name != right.name) return left.name < right.name;
            if (left.sourceName != right.sourceName) {
                return left.sourceName < right.sourceName;
            }
            return left.offset < right.offset;
        });
    std::stable_sort(
        result.nativeInterfaces.begin(),
'''
    if sort_anchor not in text:
        raise RuntimeError("native attribute sort anchor missing")
    text = text.replace(sort_anchor, sort_attrs, 1)
    write(path, text)

# Game SDK metadata ------------------------------------------------------------
path = "src/game/GameApi.cpp"
text = read(path)
if "build.nativeAttributes" not in text:
    text = text.replace(
        '''    auto build = compilation.build();
    result.languageMetadata.interfaces.insert(
''',
        '''    auto build = compilation.build();
    result.languageMetadata.attributes.insert(
        result.languageMetadata.attributes.end(),
        build.nativeAttributes.begin(),
        build.nativeAttributes.end());
    std::stable_sort(
        result.languageMetadata.attributes.begin(),
        result.languageMetadata.attributes.end(),
        [](const auto& left, const auto& right) {
            if (left.target != right.target) return left.target < right.target;
            if (left.name != right.name) return left.name < right.name;
            if (left.sourceName != right.sourceName) {
                return left.sourceName < right.sourceName;
            }
            return left.offset < right.offset;
        });
    result.languageMetadata.interfaces.insert(
''',
        1)
    write(path, text)

# Existing tests use native metadata ------------------------------------------
path = "tests/phase11_17_language_expansion_tests.cpp"
text = read(path)
old = '''void testExpansionMetadata() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "metadata.rs",
        "module Meta; [Replicated(channel = \\"state\\")] class Unit { int health; }");
    require(expansion.succeeded(), "metadata expansion failed");
    require(expansion.attributes.size() == 1, "source attribute was not captured");
    require(expansion.attributes.front().target == "Meta::Unit",
        "source attribute target was not module-qualified");
    require(expansion.attributes.front().name == "Replicated",
        "source attribute name changed");
    require(expansion.attributes.front().arguments.size() == 1,
        "source attribute arguments were not captured");
}
'''
# The source in the repository uses escaped quotes only in C++ string; handle exact current text separately.
if old not in text:
    start = text.index("void testExpansionMetadata() {")
    end = text.index("\n}\n\nvoid testExpansionOptionsRefreshExistingSources", start) + 3
    old = text[start:end]
new = '''void testExpansionMetadata() {
    realscript::compiler::Compilation compilation({{
        "metadata.rs",
        "module Meta; [Replicated(channel = \\\"state\\\")] class Unit { int health; }"}});
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
            build.nativeAttributes.front().arguments.front().name == "channel" &&
            build.nativeAttributes.front().arguments.front().value == "\\\"state\\\"",
        "source attribute arguments were not captured");
}
'''
text = text.replace(old, new, 1)
write(path, text)

# Native attribute tests -------------------------------------------------------
path = "tests/phase18_native_control_flow_tests.cpp"
text = read(path)
if "testNativeAttributes" not in text:
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
    require(!build.diagnostics.hasErrors(),
        "native attributes failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    require(build.nativeAttributes.size() == 4,
        "native declaration attributes were not retained");
    require(build.nativeAttributes.front().target ==
            "Phase18.Attributes::Unit",
        "native type attribute target was not canonical");
    bool replicated = false;
    bool command = false;
    for (const auto& attribute : build.nativeAttributes) {
        replicated = replicated ||
            (attribute.name == "Replicated" &&
             attribute.target.find("field:health") != std::string::npos);
        command = command ||
            (attribute.name == "Command" &&
             attribute.target.find("method:Damage#1") != std::string::npos);
    }
    require(replicated && command,
        "native member attribute targets were not retained");
}

void testAttributesBypassExpansion() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "attributes.rs",
        "module Native; [Serializable] class Unit { int health; }");
    require(!expansion.changed,
        "native attributes still used source expansion");
}
'''
    text = text.replace("\n} // namespace\n\nint main() {", insert + "\n} // namespace\n\nint main() {", 1)
    text = text.replace(
        '''    run("interfaces bypass expansion", testInterfaceBypassesExpansion);
''',
        '''    run("interfaces bypass expansion", testInterfaceBypassesExpansion);
    run("native attributes", testNativeAttributes);
    run("attributes bypass expansion", testAttributesBypassExpansion);
''',
        1)
    write(path, text)

print("Native Phase 18 attribute migration applied")
