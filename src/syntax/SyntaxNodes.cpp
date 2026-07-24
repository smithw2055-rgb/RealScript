#include "realscript/syntax/Syntax.h"

#include <sstream>

namespace realscript::syntax {
namespace {

text::TextSpan combine(text::TextSpan first, text::TextSpan last) noexcept {
    return text::TextSpan::fromBounds(first.start, last.end());
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

text::TextSpan UnaryExpressionSyntax::span() const noexcept {
    return combine(operatorToken.span, operand->span());
}

text::TextSpan BinaryExpressionSyntax::span() const noexcept {
    return combine(left->span(), right->span());
}

text::TextSpan AssignmentExpressionSyntax::span() const noexcept {
    return combine(identifierToken.span, expression->span());
}

text::TextSpan ParenthesizedExpressionSyntax::span() const noexcept {
    return combine(openParenToken.span, closeParenToken.span);
}

text::TextSpan CallExpressionSyntax::span() const noexcept {
    return combine(identifierToken.span, closeParenToken.span);
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

text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {
    return combine(type.span(), semicolonToken.span);
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

text::TextSpan FunctionDeclarationSyntax::span() const noexcept {
    return combine(returnType.span(), body.span());
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
    if (!functions.empty()) {
        return combine(functions.front().span(), endOfFileToken.span);
    }
    return endOfFileToken.span;
}

} // namespace realscript::syntax
