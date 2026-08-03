#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]

def read(path): return (ROOT / path).read_text(encoding="utf-8")
def write(path, text): (ROOT / path).write_text(text, encoding="utf-8")
def once(path, old, new):
    text = read(path)
    if new in text: return
    if old not in text: raise RuntimeError(f"anchor not found in {path}: {old[:120]!r}")
    write(path, text.replace(old, new, 1))

# AST fields.
once("include/realscript/syntax/Syntax.h",
'''struct TypeSyntax final : SyntaxNode {
    SyntaxToken name;
    std::optional<SyntaxToken> openBracketToken;''',
'''struct TypeSyntax final : SyntaxNode {
    SyntaxToken name;
    std::optional<SyntaxToken> lessToken;
    std::vector<TypeSyntax> typeArguments;
    std::vector<SyntaxToken> typeArgumentCommaTokens;
    std::optional<SyntaxToken> greaterToken;
    std::optional<SyntaxToken> openBracketToken;''')
once("include/realscript/syntax/Syntax.h",
'''    [[nodiscard]] bool isArray() const noexcept {
        return openBracketToken.has_value() && closeBracketToken.has_value();
    }''',
'''    [[nodiscard]] bool isGeneric() const noexcept {
        return lessToken.has_value() && greaterToken.has_value();
    }
    [[nodiscard]] bool isArray() const noexcept {
        return openBracketToken.has_value() && closeBracketToken.has_value();
    }''')
once("include/realscript/syntax/Syntax.h",
'''struct CallExpressionSyntax final : ExpressionSyntax {
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;''',
'''struct CallExpressionSyntax final : ExpressionSyntax {
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> lessToken;
    std::vector<TypeSyntax> typeArguments;
    std::vector<SyntaxToken> typeArgumentCommaTokens;
    std::optional<SyntaxToken> greaterToken;
    SyntaxToken openParenToken;''')
once("include/realscript/syntax/Syntax.h",
'''struct MemberCallExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> receiver;
    SyntaxToken dotToken;
    SyntaxToken nameToken;
    SyntaxToken openParenToken;''',
'''struct MemberCallExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> receiver;
    SyntaxToken dotToken;
    SyntaxToken nameToken;
    std::optional<SyntaxToken> lessToken;
    std::vector<TypeSyntax> typeArguments;
    std::vector<SyntaxToken> typeArgumentCommaTokens;
    std::optional<SyntaxToken> greaterToken;
    SyntaxToken openParenToken;''')
# Class/struct/function type params.
for marker in ["struct ClassDeclarationSyntax final : SyntaxNode {", "struct StructDeclarationSyntax final : SyntaxNode {"]:
    old = marker + '''
    std::vector<AttributeListSyntax> attributes;
    SyntaxToken classKeyword;''' if "Class" in marker else marker + '''
    std::vector<AttributeListSyntax> attributes;
    SyntaxToken structKeyword;'''
    keyword = "classKeyword" if "Class" in marker else "structKeyword"
    new = marker + f'''
    std::vector<AttributeListSyntax> attributes;
    SyntaxToken {keyword};'''
    # Insert after identifier rather than marker.
    id_old = f'''    SyntaxToken {keyword};
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> colonToken;'''
    id_new = f'''    SyntaxToken {keyword};
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> typeParameterLessToken;
    std::vector<SyntaxToken> typeParameters;
    std::vector<SyntaxToken> typeParameterCommaTokens;
    std::optional<SyntaxToken> typeParameterGreaterToken;
    std::optional<SyntaxToken> colonToken;'''
    once("include/realscript/syntax/Syntax.h", id_old, id_new)
once("include/realscript/syntax/Syntax.h",
'''struct FunctionDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::optional<SyntaxToken> staticKeyword;
    TypeSyntax returnType;
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;''',
'''struct FunctionDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::optional<SyntaxToken> staticKeyword;
    TypeSyntax returnType;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> typeParameterLessToken;
    std::vector<SyntaxToken> typeParameters;
    std::vector<SyntaxToken> typeParameterCommaTokens;
    std::optional<SyntaxToken> typeParameterGreaterToken;
    SyntaxToken openParenToken;''')
# Parser helpers.
once("include/realscript/syntax/Syntax.h",
'''    [[nodiscard]] TypeSyntax parseType();
    [[nodiscard]] ParameterSyntax parseParameter();''',
'''    [[nodiscard]] TypeSyntax parseType();
    void parseTypeArgumentList(
        std::optional<SyntaxToken>& lessToken,
        std::vector<TypeSyntax>& arguments,
        std::vector<SyntaxToken>& commaTokens,
        std::optional<SyntaxToken>& greaterToken);
    void parseTypeParameterList(
        std::optional<SyntaxToken>& lessToken,
        std::vector<SyntaxToken>& parameters,
        std::vector<SyntaxToken>& commaTokens,
        std::optional<SyntaxToken>& greaterToken);
    [[nodiscard]] bool isGenericCallStart() const noexcept;
    [[nodiscard]] ParameterSyntax parseParameter();''')

# Spans.
once("src/syntax/SyntaxNodes.cpp",
'''text::TextSpan TypeSyntax::span() const noexcept {
    return closeBracketToken
        ? combine(name.span, closeBracketToken->span)
        : name.span;
}''',
'''text::TextSpan TypeSyntax::span() const noexcept {
    if (closeBracketToken) {
        return combine(name.span, closeBracketToken->span);
    }
    if (greaterToken) {
        return combine(name.span, greaterToken->span);
    }
    return name.span;
}''')

# Parser type lists.
once("src/syntax/Parser.cpp",
'''TypeSyntax Parser::parseType() {
    TypeSyntax result;
    if (isIdentifierLike(current().kind)) {
        result.name = nextToken();
    } else {
        result.name = match(SyntaxKind::IdentifierToken);
    }
    if (current().kind == SyntaxKind::OpenBracketToken &&
        peek(1).kind == SyntaxKind::CloseBracketToken) {
        result.openBracketToken = nextToken();
        result.closeBracketToken = nextToken();
    }
    return result;
}''',
'''void Parser::parseTypeArgumentList(
    std::optional<SyntaxToken>& lessToken,
    std::vector<TypeSyntax>& arguments,
    std::vector<SyntaxToken>& commaTokens,
    std::optional<SyntaxToken>& greaterToken) {
    if (current().kind != SyntaxKind::LessToken) return;
    lessToken = nextToken();
    if (current().kind != SyntaxKind::GreaterToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        arguments.push_back(parseType());
        while (current().kind == SyntaxKind::CommaToken) {
            commaTokens.push_back(nextToken());
            arguments.push_back(parseType());
        }
    }
    greaterToken = match(SyntaxKind::GreaterToken);
}

void Parser::parseTypeParameterList(
    std::optional<SyntaxToken>& lessToken,
    std::vector<SyntaxToken>& parameters,
    std::vector<SyntaxToken>& commaTokens,
    std::optional<SyntaxToken>& greaterToken) {
    if (current().kind != SyntaxKind::LessToken) return;
    lessToken = nextToken();
    if (current().kind != SyntaxKind::GreaterToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        parameters.push_back(match(SyntaxKind::IdentifierToken));
        while (current().kind == SyntaxKind::CommaToken) {
            commaTokens.push_back(nextToken());
            parameters.push_back(match(SyntaxKind::IdentifierToken));
        }
    }
    greaterToken = match(SyntaxKind::GreaterToken);
}

TypeSyntax Parser::parseType() {
    TypeSyntax result;
    if (isIdentifierLike(current().kind)) {
        result.name = nextToken();
    } else {
        result.name = match(SyntaxKind::IdentifierToken);
    }
    parseTypeArgumentList(
        result.lessToken,
        result.typeArguments,
        result.typeArgumentCommaTokens,
        result.greaterToken);
    if (current().kind == SyntaxKind::OpenBracketToken &&
        peek(1).kind == SyntaxKind::CloseBracketToken) {
        result.openBracketToken = nextToken();
        result.closeBracketToken = nextToken();
    }
    return result;
}''')
# Declaration parameter lists.
once("src/syntax/Parser.cpp",
'''    result.identifierToken = identifier ? std::move(*identifier) :
        match(SyntaxKind::IdentifierToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);''',
'''    result.identifierToken = identifier ? std::move(*identifier) :
        match(SyntaxKind::IdentifierToken);
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);''')
once("src/syntax/Parser.cpp",
'''    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    parseInterfaceList(
        result.colonToken,''',
'''    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    parseInterfaceList(
        result.colonToken,''')
# Same anchor occurs twice; first replacement handled class. Replace remaining struct.
text = read("src/syntax/Parser.cpp")
old = '''    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    parseInterfaceList(
        result.colonToken,'''
new = '''    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    parseInterfaceList(
        result.colonToken,'''
if old in text:
    text = text.replace(old, new, 1)
write("src/syntax/Parser.cpp", text)
# New expression uses full parseType.
once("src/syntax/Parser.cpp",
'''    TypeSyntax type;
    if (isIdentifierLike(current().kind)) type.name = nextToken();
    else type.name = match(SyntaxKind::IdentifierToken);

    if (current().kind == SyntaxKind::OpenParenToken) {''',
'''    auto type = parseType();

    if (current().kind == SyntaxKind::OpenParenToken) {''')
# Generic calls and member calls.
once("src/syntax/Parser.cpp",
'''            const auto dot = nextToken();
            const auto name = match(SyntaxKind::IdentifierToken);
            if (current().kind == SyntaxKind::OpenParenToken) {
                auto call = std::make_unique<MemberCallExpressionSyntax>();
                call->receiver = std::move(expression);
                call->dotToken = dot;
                call->nameToken = name;
                call->openParenToken = nextToken();''',
'''            const auto dot = nextToken();
            const auto name = match(SyntaxKind::IdentifierToken);
            std::optional<SyntaxToken> lessToken;
            std::vector<TypeSyntax> typeArguments;
            std::vector<SyntaxToken> typeArgumentCommas;
            std::optional<SyntaxToken> greaterToken;
            parseTypeArgumentList(
                lessToken, typeArguments, typeArgumentCommas,
                greaterToken);
            if (current().kind == SyntaxKind::OpenParenToken) {
                auto call = std::make_unique<MemberCallExpressionSyntax>();
                call->receiver = std::move(expression);
                call->dotToken = dot;
                call->nameToken = name;
                call->lessToken = std::move(lessToken);
                call->typeArguments = std::move(typeArguments);
                call->typeArgumentCommaTokens =
                    std::move(typeArgumentCommas);
                call->greaterToken = std::move(greaterToken);
                call->openParenToken = nextToken();''')
# Reject type args on non-call member by preserving parse diagnostic.
once("src/syntax/Parser.cpp",
'''            } else {
                auto member = std::make_unique<MemberAccessExpressionSyntax>();''',
'''            } else {
                if (lessToken) {
                    diagnostics_.report(
                        "RS1115",
                        "generic member type arguments require a call",
                        lessToken->span);
                }
                auto member = std::make_unique<MemberAccessExpressionSyntax>();''')
# Generic call lookahead/helper and parser.
once("src/syntax/Parser.cpp",
'''    case SyntaxKind::IdentifierToken:
        if (peek(1).kind == SyntaxKind::OpenParenToken) {
            return parseCallExpression();''',
'''    case SyntaxKind::IdentifierToken:
        if (peek(1).kind == SyntaxKind::OpenParenToken ||
            isGenericCallStart()) {
            return parseCallExpression();''')
once("src/syntax/Parser.cpp",
'''std::unique_ptr<ExpressionSyntax> Parser::parseCallExpression() {
    auto result = std::make_unique<CallExpressionSyntax>();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    result->openParenToken = match(SyntaxKind::OpenParenToken);''',
'''bool Parser::isGenericCallStart() const noexcept {
    if (current().kind != SyntaxKind::IdentifierToken ||
        peek(1).kind != SyntaxKind::LessToken) {
        return false;
    }
    int depth = 0;
    std::size_t offset = 1;
    while (position_ + offset < tokens_.size()) {
        const auto kind = peek(offset).kind;
        if (kind == SyntaxKind::LessToken) ++depth;
        else if (kind == SyntaxKind::GreaterToken) {
            --depth;
            if (depth == 0) {
                return peek(offset + 1).kind ==
                    SyntaxKind::OpenParenToken;
            }
        } else if (kind == SyntaxKind::EndOfFileToken ||
                   kind == SyntaxKind::SemicolonToken) {
            return false;
        }
        ++offset;
    }
    return false;
}

std::unique_ptr<ExpressionSyntax> Parser::parseCallExpression() {
    auto result = std::make_unique<CallExpressionSyntax>();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    parseTypeArgumentList(
        result->lessToken,
        result->typeArguments,
        result->typeArgumentCommaTokens,
        result->greaterToken);
    result->openParenToken = match(SyntaxKind::OpenParenToken);''')
# Variable declaration lookahead.
once("src/syntax/Parser.cpp",
'''bool Parser::isVariableDeclarationStart() const noexcept {
    if (!isIdentifierLike(current().kind)) {
        return false;
    }
    std::size_t offset = 1;
    if (peek(offset).kind == SyntaxKind::OpenBracketToken &&
        peek(offset + 1).kind == SyntaxKind::CloseBracketToken) {
        offset += 2;
    }
    return peek(offset).kind == SyntaxKind::IdentifierToken &&
        (peek(offset + 1).kind == SyntaxKind::EqualsToken ||
         peek(offset + 1).kind == SyntaxKind::SemicolonToken);
}''',
'''bool Parser::isVariableDeclarationStart() const noexcept {
    if (!isIdentifierLike(current().kind)) {
        return false;
    }
    std::size_t offset = 1;
    if (peek(offset).kind == SyntaxKind::LessToken) {
        int depth = 0;
        do {
            if (peek(offset).kind == SyntaxKind::LessToken) ++depth;
            else if (peek(offset).kind == SyntaxKind::GreaterToken) --depth;
            ++offset;
            if (peek(offset).kind == SyntaxKind::EndOfFileToken) {
                return false;
            }
        } while (depth > 0);
    }
    if (peek(offset).kind == SyntaxKind::OpenBracketToken &&
        peek(offset + 1).kind == SyntaxKind::CloseBracketToken) {
        offset += 2;
    }
    return peek(offset).kind == SyntaxKind::IdentifierToken &&
        (peek(offset + 1).kind == SyntaxKind::EqualsToken ||
         peek(offset + 1).kind == SyntaxKind::SemicolonToken);
}''')

# Parser regression.
test_path = ROOT / "tests/phase18_native_control_flow_tests.cpp"
tests = test_path.read_text(encoding="utf-8")
anchor = "void testNativeSequenceDiagnostics() {"
insert = '''void testNativeGenericSyntax() {
    realscript::text::SourceText source(R"(
module Phase18.Generics;
class Pair<TLeft, TRight>
{
    TLeft left;
    TRight right;
}
T Identity<T>(T value) { return value; }
int main()
{
    Pair<int, List<string>> value =
        new Pair<int, List<string>>();
    return Identity<int>(1);
}
)", "generic-syntax.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();
    require(!diagnostics.hasErrors(),
        "native generic syntax failed to parse:\n" +
            diagnosticsText(diagnostics));
    require(unit.classes.size() == 1 &&
            unit.classes.front().typeParameters.size() == 2 &&
            unit.functions.size() == 2 &&
            unit.functions.front().typeParameters.size() == 1,
        "native generic declarations were not retained");
    const auto& mainBody = unit.functions.back().body.statements;
    const auto& declaration = static_cast<const
        realscript::syntax::VariableDeclarationStatementSyntax&>(
            *mainBody.front());
    require(declaration.type.typeArguments.size() == 2 &&
            declaration.type.typeArguments[1].typeArguments.size() == 1,
        "nested native generic type arguments were not retained");
    const auto& returned = static_cast<const
        realscript::syntax::ReturnStatementSyntax&>(
            *mainBody.back());
    const auto& call = static_cast<const
        realscript::syntax::CallExpressionSyntax&>(
            *returned.expression);
    require(call.typeArguments.size() == 1,
        "native generic call arguments were not retained");
}

'''
if insert not in tests:
    if anchor not in tests: raise RuntimeError("generic syntax test anchor missing")
    tests = tests.replace(anchor, insert + anchor, 1)
run_anchor = '    run("native sequence diagnostics", testNativeSequenceDiagnostics);'
run_insert = '    run("native generic syntax", testNativeGenericSyntax);\n'
if run_insert not in tests:
    if run_anchor not in tests: raise RuntimeError("generic syntax registration anchor missing")
    tests = tests.replace(run_anchor, run_insert + run_anchor, 1)
test_path.write_text(tests, encoding="utf-8")

print("native Phase 18 generic syntax applied")
