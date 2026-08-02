#include "realscript/syntax/Syntax.h"

#include <sstream>

namespace realscript::syntax {
namespace {

text::TextSpan combine(text::TextSpan first, text::TextSpan last) noexcept {
    return text::TextSpan::fromBounds(first.start, last.end());
}

text::TextSpan declarationStart(
    const std::vector<AttributeListSyntax>& attributes,
    text::TextSpan fallback) noexcept {
    return attributes.empty() ? fallback
        : text::TextSpan::fromBounds(
            attributes.front().span().start,
            fallback.end());
}

std::string joinQualifiedName(const std::vector<SyntaxToken>& parts) {
    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out << '.';
        out << parts[i].text;
    }
    return out.str();
}

} // namespace

text::TextSpan AttributeArgumentSyntax::valueSpan() const noexcept {
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

text::TextSpan TypeSyntax::span() const noexcept {
    return closeBracketToken
        ? combine(name.span, closeBracketToken->span)
        : name.span;
}

text::TextSpan UnaryExpressionSyntax::span() const noexcept {
    return combine(operatorToken.span, operand->span());
}

text::TextSpan BinaryExpressionSyntax::span() const noexcept {
    return combine(left->span(), right->span());
}

text::TextSpan AssignmentExpressionSyntax::span() const noexcept {
    return combine(identifierToken.span, expression->span());
}

text::TextSpan MemberAccessExpressionSyntax::span() const noexcept {
    return combine(receiver->span(), nameToken.span);
}

text::TextSpan ElementAccessExpressionSyntax::span() const noexcept {
    return combine(receiver->span(), closeBracketToken.span);
}

text::TextSpan ElementAssignmentExpressionSyntax::span() const noexcept {
    return combine(receiver->span(), expression->span());
}

text::TextSpan MemberAssignmentExpressionSyntax::span() const noexcept {
    return combine(receiver->span(), expression->span());
}

text::TextSpan NewObjectExpressionSyntax::span() const noexcept {
    return combine(newKeyword.span, closeParenToken.span);
}

text::TextSpan NewArrayExpressionSyntax::span() const noexcept {
    return combine(newKeyword.span, closeBracketToken.span);
}

text::TextSpan ParenthesizedExpressionSyntax::span() const noexcept {
    return combine(openParenToken.span, closeParenToken.span);
}

text::TextSpan CallExpressionSyntax::span() const noexcept {
    return combine(identifierToken.span, closeParenToken.span);
}

text::TextSpan MemberCallExpressionSyntax::span() const noexcept {
    return combine(receiver->span(), closeParenToken.span);
}

text::TextSpan ReturnStatementSyntax::span() const noexcept {
    return combine(returnKeyword.span, semicolonToken.span);
}

text::TextSpan IfStatementSyntax::span() const noexcept {
    return combine(ifKeyword.span, elseStatement ? elseStatement->span() : thenStatement->span());
}

text::TextSpan WhileStatementSyntax::span() const noexcept {
    return combine(whileKeyword.span, body->span());
}

text::TextSpan ForStatementSyntax::span() const noexcept {
    return combine(forKeyword.span, body ? body->span() : closeParenToken.span);
}

text::TextSpan ForeachStatementSyntax::span() const noexcept {
    return combine(foreachKeyword.span, body ? body->span() : closeParenToken.span);
}

text::TextSpan DoWhileStatementSyntax::span() const noexcept {
    return combine(doKeyword.span, semicolonToken.span);
}

text::TextSpan BreakStatementSyntax::span() const noexcept {
    return combine(breakKeyword.span, semicolonToken.span);
}

text::TextSpan ContinueStatementSyntax::span() const noexcept {
    return combine(continueKeyword.span, semicolonToken.span);
}

text::TextSpan SwitchSectionSyntax::span() const noexcept {
    const auto start = caseKeyword ? caseKeyword->span : defaultKeyword->span;
    return statements.empty() ? combine(start, colonToken.span)
                              : combine(start, statements.back()->span());
}

text::TextSpan SwitchStatementSyntax::span() const noexcept {
    return combine(switchKeyword.span, closeBraceToken.span);
}

text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {
    return combine(declarationStart(attributes, type.span()), semicolonToken.span);
}

text::TextSpan ExpressionStatementSyntax::span() const noexcept {
    return combine(expression->span(), semicolonToken.span);
}

text::TextSpan BlockStatementSyntax::span() const noexcept {
    return combine(openBraceToken.span, closeBraceToken.span);
}

text::TextSpan ParameterSyntax::span() const noexcept {
    return combine(type.span(), identifierToken.span);
}

text::TextSpan FieldDeclarationSyntax::span() const noexcept {
    return combine(type.span(), semicolonToken.span);
}

text::TextSpan ConstructorDeclarationSyntax::span() const noexcept {
    return combine(declarationStart(attributes, identifierToken.span), body.span());
}

text::TextSpan AccessorDeclarationSyntax::span() const noexcept {
    if (body) return combine(keyword.span, body->span());
    return semicolonToken ? combine(keyword.span, semicolonToken->span) : keyword.span;
}

text::TextSpan PropertyDeclarationSyntax::span() const noexcept {
    return combine(declarationStart(attributes, type.span()), closeBraceToken.span);
}

text::TextSpan ClassDeclarationSyntax::span() const noexcept {
    return combine(declarationStart(attributes, classKeyword.span), closeBraceToken.span);
}

text::TextSpan StructDeclarationSyntax::span() const noexcept {
    return combine(declarationStart(attributes, structKeyword.span), closeBraceToken.span);
}

text::TextSpan InterfaceMethodDeclarationSyntax::span() const noexcept {
    return combine(declarationStart(attributes, returnType.span()), semicolonToken.span);
}

text::TextSpan InterfaceDeclarationSyntax::span() const noexcept {
    return combine(declarationStart(attributes, interfaceKeyword.span), closeBraceToken.span);
}

text::TextSpan EnumMemberDeclarationSyntax::span() const noexcept {
    text::TextSpan end = identifierToken.span;
    if (commaToken) end = commaToken->span;
    else if (valueToken) end = valueToken->span;
    return combine(declarationStart(attributes, identifierToken.span), end);
}

text::TextSpan EnumDeclarationSyntax::span() const noexcept {
    return combine(declarationStart(attributes, enumKeyword.span), closeBraceToken.span);
}

text::TextSpan FunctionDeclarationSyntax::span() const noexcept {
    return combine(declarationStart(attributes, returnType.span()), body.span());
}

std::string ModuleDeclarationSyntax::fullName() const {
    return joinQualifiedName(nameParts);
}

text::TextSpan ModuleDeclarationSyntax::span() const noexcept {
    return combine(moduleKeyword.span, semicolonToken.span);
}

std::string ImportDeclarationSyntax::fullName() const {
    return joinQualifiedName(nameParts);
}

text::TextSpan ImportDeclarationSyntax::span() const noexcept {
    return combine(importKeyword.span, semicolonToken.span);
}

text::TextSpan CompilationUnitSyntax::span() const noexcept {
    if (moduleDeclaration) {
        return combine(moduleDeclaration->span(), endOfFileToken.span);
    }
    if (!imports.empty()) {
        return combine(imports.front().span(), endOfFileToken.span);
    }
    if (!classes.empty()) {
        return combine(classes.front().span(), endOfFileToken.span);
    }
    if (!structs.empty()) {
        return combine(structs.front().span(), endOfFileToken.span);
    }
    if (!enums.empty()) {
        return combine(enums.front().span(), endOfFileToken.span);
    }
    if (!interfaces.empty()) {
        return combine(interfaces.front().span(), endOfFileToken.span);
    }
    if (!functions.empty()) {
        return combine(functions.front().span(), endOfFileToken.span);
    }
    return endOfFileToken.span;
}

} // namespace realscript::syntax
