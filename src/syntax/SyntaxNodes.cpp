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
    return attributes.empty()
        ? fallback
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

text::TextSpan TypeSyntax::span() const noexcept {
    if (closeBracketToken) {
        return combine(name.span, closeBracketToken->span);
    }
    if (nullableToken) {
        return combine(name.span, nullableToken->span);
    }
    if (greaterToken) {
        return combine(name.span, greaterToken->span);
    }
    return name.span;
}

text::TextSpan ConstantPatternSyntax::span() const noexcept {
    return expression ? expression->span() : text::TextSpan{};
}

text::TextSpan VarPatternSyntax::span() const noexcept {
    return combine(varKeyword.span, designationToken.span);
}

text::TextSpan TypePatternSyntax::span() const noexcept {
    return designationToken ? combine(type.span(), designationToken->span) : type.span();
}

text::TextSpan RelationalPatternSyntax::span() const noexcept {
    return operand ? combine(operatorToken.span, operand->span()) : operatorToken.span;
}

text::TextSpan UnaryPatternSyntax::span() const noexcept {
    return pattern ? combine(operatorToken.span, pattern->span()) : operatorToken.span;
}

text::TextSpan BinaryPatternSyntax::span() const noexcept {
    if (left && right) return combine(left->span(), right->span());
    if (left) return combine(left->span(), operatorToken.span);
    return operatorToken.span;
}

text::TextSpan PropertySubpatternSyntax::span() const noexcept {
    return pattern ? combine(memberToken.span, pattern->span())
                   : combine(memberToken.span, colonToken.span);
}

text::TextSpan PropertyPatternSyntax::span() const noexcept {
    const auto start = type ? type->span() : openBraceToken.span;
    const auto end = designationToken ? designationToken->span : closeBraceToken.span;
    return combine(start, end);
}

text::TextSpan ParenthesizedPatternSyntax::span() const noexcept {
    return combine(openParenToken.span, closeParenToken.span);
}

text::TextSpan LambdaExpressionSyntax::span() const noexcept {
    const auto start = openParenToken
        ? openParenToken->span
        : parameterTokens.empty()
            ? arrowToken.span
            : parameterTokens.front().span;
    return combine(start, body->span());
}

text::TextSpan UnaryExpressionSyntax::span() const noexcept {
    return combine(operatorToken.span, operand->span());
}

text::TextSpan BinaryExpressionSyntax::span() const noexcept {
    return combine(left->span(), right->span());
}

text::TextSpan TypeBinaryExpressionSyntax::span() const noexcept {
    if (pattern) {
        return combine(expression->span(), pattern->span());
    }
    return combine(expression->span(),
        designationToken ? designationToken->span : type.span());
}

text::TextSpan TypeOfExpressionSyntax::span() const noexcept {
    return combine(typeofKeyword.span, closeParenToken.span);
}

text::TextSpan SwitchExpressionArmSyntax::span() const noexcept {
    const auto start = pattern ? pattern->span() :
        (patternType ? patternType->span() :
         (discardToken ? discardToken->span : (label ? label->span() : arrowToken.span)));
    return combine(start, value ? value->span() : arrowToken.span);
}

text::TextSpan SwitchExpressionSyntax::span() const noexcept {
    return combine(expression->span(), closeBraceToken.span);
}

text::TextSpan ConditionalExpressionSyntax::span() const noexcept {
    return combine(condition->span(), whenFalse->span());
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
    return combine(newKeyword.span,
        initializerCloseBraceToken
            ? initializerCloseBraceToken->span
            : closeParenToken.span);
}

text::TextSpan InitializerElementSyntax::span() const noexcept {
    if (nameToken) {
        return combine(nameToken->span,
            expressions.empty() ? equalsToken->span : expressions.back()->span());
    }
    if (openBraceToken) {
        return combine(openBraceToken->span,
            closeBraceToken ? closeBraceToken->span : openBraceToken->span);
    }
    return expressions.empty() ? text::TextSpan{} : expressions.front()->span();
}

text::TextSpan NewArrayExpressionSyntax::span() const noexcept {
    return combine(
        newKeyword.span,
        initializerCloseBraceToken ? initializerCloseBraceToken->span : closeBracketToken.span);
}

text::TextSpan ParenthesizedExpressionSyntax::span() const noexcept {
    return combine(openParenToken.span, closeParenToken.span);
}

text::TextSpan CastExpressionSyntax::span() const noexcept {
    return combine(openParenToken.span, expression->span());
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

text::TextSpan ThrowStatementSyntax::span() const noexcept {
    return combine(throwKeyword.span, semicolonToken.span);
}

text::TextSpan CatchClauseSyntax::span() const noexcept {
    return combine(catchKeyword.span, body.span());
}

text::TextSpan TryStatementSyntax::span() const noexcept {
    return combine(tryKeyword.span,
        finallyBody ? finallyBody->span() :
        (!catches.empty() ? catches.back().span() : body.span()));
}

text::TextSpan YieldWaitStatementSyntax::span() const noexcept {
    return combine(yieldKeyword.span, semicolonToken.span);
}

text::TextSpan YieldBreakStatementSyntax::span() const noexcept {
    return combine(yieldKeyword.span, semicolonToken.span);
}

text::TextSpan EventSubscriptionStatementSyntax::span() const noexcept {
    return combine(
        receiver ? receiver->span() : eventNameToken.span,
        semicolonToken.span);
}

text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {
    return combine(
        constKeyword ? constKeyword->span : (refKeyword ? refKeyword->span : type.span()),
        semicolonToken.span);
}

text::TextSpan ExpressionStatementSyntax::span() const noexcept {
    return combine(expression->span(), semicolonToken.span);
}

text::TextSpan BlockStatementSyntax::span() const noexcept {
    return combine(openBraceToken.span, closeBraceToken.span);
}

text::TextSpan ParameterSyntax::span() const noexcept {
    text::TextSpan first = type.span();
    if (modifierToken && modifierToken->span.start < first.start) first = modifierToken->span;
    if (thisKeyword && thisKeyword->span.start < first.start) first = thisKeyword->span;
    if (paramsKeyword) first = paramsKeyword->span;
    if (!attributes.empty()) first = attributes.front().span();
    return combine(
        first,
        defaultValue ? defaultValue->span() : identifierToken.span);
}

text::TextSpan FieldDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, type.span()),
        semicolonToken.span);
}

text::TextSpan EventDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, eventKeyword.span),
        semicolonToken.span);
}

text::TextSpan ConstructorDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, identifierToken.span),
        body.span());
}

text::TextSpan AccessorDeclarationSyntax::span() const noexcept {
    if (body) return combine(keyword.span, body->span());
    return semicolonToken ? combine(keyword.span, semicolonToken->span) : keyword.span;
}

text::TextSpan PropertyDeclarationSyntax::span() const noexcept {
    const auto endSpan = semicolonToken
        ? semicolonToken->span
        : (initializer ? initializer->span() : closeBraceToken.span);
    return combine(
        declarationStart(attributes, type.span()),
        endSpan);
}

text::TextSpan ClassDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, classKeyword.span),
        closeBraceToken.span);
}

text::TextSpan StructDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, structKeyword.span),
        closeBraceToken.span);
}

text::TextSpan InterfaceMethodDeclarationSyntax::span() const noexcept {
    const auto end = body ? body->span()
                          : (semicolonToken ? semicolonToken->span : closeParenToken.span);
    return combine(
        declarationStart(attributes, returnType.span()),
        end);
}

text::TextSpan InterfaceDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, interfaceKeyword.span),
        closeBraceToken.span);
}

text::TextSpan GenericConstraintClauseSyntax::span() const noexcept {
    return combine(
        whereKeyword.span,
        constraints.empty() ? colonToken.span : constraints.back().span);
}

text::TextSpan EnumMemberDeclarationSyntax::span() const noexcept {
    text::TextSpan end = identifierToken.span;
    if (commaToken) end = commaToken->span;
    else if (valueToken) end = valueToken->span;
    return combine(
        declarationStart(attributes, identifierToken.span),
        end);
}

text::TextSpan EnumDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, enumKeyword.span),
        closeBraceToken.span);
}

text::TextSpan FunctionDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, returnType.span()),
        semicolonToken ? semicolonToken->span : body.span());
}

std::string ModuleDeclarationSyntax::fullName() const {
    return joinQualifiedName(nameParts);
}

text::TextSpan DelegateDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, delegateKeyword.span),
        semicolonToken.span);
}

text::TextSpan SequenceDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, sequenceKeyword.span),
        body.span());
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
    if (!delegates.empty()) {
        return combine(delegates.front().span(), endOfFileToken.span);
    }
    if (!functions.empty()) {
        return combine(functions.front().span(), endOfFileToken.span);
    }
    return endOfFileToken.span;
}


TypeSyntax cloneTypeSyntax(const TypeSyntax& value) {
    return value;
}

std::unique_ptr<PatternSyntax> clonePatternSyntax(const PatternSyntax& value) {
    switch (value.kind()) {
    case SyntaxKind::ConstantPattern: {
        const auto& src = static_cast<const ConstantPatternSyntax&>(value);
        auto out = std::make_unique<ConstantPatternSyntax>();
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        return out;
    }
    case SyntaxKind::DiscardPattern: {
        const auto& src = static_cast<const DiscardPatternSyntax&>(value);
        auto out = std::make_unique<DiscardPatternSyntax>();
        out->underscoreToken = src.underscoreToken;
        return out;
    }
    case SyntaxKind::VarPattern: {
        const auto& src = static_cast<const VarPatternSyntax&>(value);
        auto out = std::make_unique<VarPatternSyntax>();
        out->varKeyword = src.varKeyword;
        out->designationToken = src.designationToken;
        return out;
    }
    case SyntaxKind::TypePattern: {
        const auto& src = static_cast<const TypePatternSyntax&>(value);
        auto out = std::make_unique<TypePatternSyntax>();
        out->type = src.type;
        out->designationToken = src.designationToken;
        return out;
    }
    case SyntaxKind::RelationalPattern: {
        const auto& src = static_cast<const RelationalPatternSyntax&>(value);
        auto out = std::make_unique<RelationalPatternSyntax>();
        out->operatorToken = src.operatorToken;
        if (src.operand) out->operand = cloneExpressionSyntax(*src.operand);
        return out;
    }
    case SyntaxKind::UnaryPattern: {
        const auto& src = static_cast<const UnaryPatternSyntax&>(value);
        auto out = std::make_unique<UnaryPatternSyntax>();
        out->operatorToken = src.operatorToken;
        if (src.pattern) out->pattern = clonePatternSyntax(*src.pattern);
        return out;
    }
    case SyntaxKind::BinaryPattern: {
        const auto& src = static_cast<const BinaryPatternSyntax&>(value);
        auto out = std::make_unique<BinaryPatternSyntax>();
        if (src.left) out->left = clonePatternSyntax(*src.left);
        out->operatorToken = src.operatorToken;
        if (src.right) out->right = clonePatternSyntax(*src.right);
        return out;
    }
    case SyntaxKind::PropertyPattern: {
        const auto& src = static_cast<const PropertyPatternSyntax&>(value);
        auto out = std::make_unique<PropertyPatternSyntax>();
        out->type = src.type;
        out->openBraceToken = src.openBraceToken;
        for (const auto& prop : src.properties) {
            PropertySubpatternSyntax sub;
            sub.memberToken = prop.memberToken;
            sub.colonToken = prop.colonToken;
            if (prop.pattern) sub.pattern = clonePatternSyntax(*prop.pattern);
            out->properties.push_back(std::move(sub));
        }
        out->commaTokens = src.commaTokens;
        out->closeBraceToken = src.closeBraceToken;
        out->designationToken = src.designationToken;
        return out;
    }
    case SyntaxKind::ParenthesizedPattern: {
        const auto& src = static_cast<const ParenthesizedPatternSyntax&>(value);
        auto out = std::make_unique<ParenthesizedPatternSyntax>();
        out->openParenToken = src.openParenToken;
        if (src.pattern) out->pattern = clonePatternSyntax(*src.pattern);
        out->closeParenToken = src.closeParenToken;
        return out;
    }
    default:
        return nullptr;
    }
}

namespace {

InitializerElementSyntax cloneInitializerElement(const InitializerElementSyntax& value) {
    InitializerElementSyntax out;
    out.nameToken = value.nameToken;
    out.equalsToken = value.equalsToken;
    out.openBraceToken = value.openBraceToken;
    for (const auto& expr : value.expressions) {
        out.expressions.push_back(expr ? cloneExpressionSyntax(*expr) : nullptr);
    }
    out.commaTokens = value.commaTokens;
    out.closeBraceToken = value.closeBraceToken;
    return out;
}

SwitchExpressionArmSyntax cloneSwitchExpressionArm(const SwitchExpressionArmSyntax& value) {
    SwitchExpressionArmSyntax out;
    if (value.pattern) out.pattern = clonePatternSyntax(*value.pattern);
    if (value.label) out.label = cloneExpressionSyntax(*value.label);
    out.patternType = value.patternType;
    out.patternDesignation = value.patternDesignation;
    out.discardToken = value.discardToken;
    out.whenKeyword = value.whenKeyword;
    if (value.guard) out.guard = cloneExpressionSyntax(*value.guard);
    out.arrowToken = value.arrowToken;
    if (value.value) out.value = cloneExpressionSyntax(*value.value);
    return out;
}

CatchClauseSyntax cloneCatchClause(const CatchClauseSyntax& value) {
    CatchClauseSyntax out;
    out.catchKeyword = value.catchKeyword;
    out.openParenToken = value.openParenToken;
    out.type = value.type;
    out.identifierToken = value.identifierToken;
    out.closeParenToken = value.closeParenToken;
    out.body = cloneBlockStatementSyntax(value.body);
    return out;
}

SwitchSectionSyntax cloneSwitchSection(const SwitchSectionSyntax& value) {
    SwitchSectionSyntax out;
    out.caseKeyword = value.caseKeyword;
    out.defaultKeyword = value.defaultKeyword;
    if (value.pattern) out.pattern = clonePatternSyntax(*value.pattern);
    if (value.label) out.label = cloneExpressionSyntax(*value.label);
    out.patternType = value.patternType;
    out.patternDesignation = value.patternDesignation;
    out.whenKeyword = value.whenKeyword;
    if (value.guard) out.guard = cloneExpressionSyntax(*value.guard);
    out.colonToken = value.colonToken;
    for (const auto& stmt : value.statements) {
        out.statements.push_back(stmt ? cloneStatementSyntax(*stmt) : nullptr);
    }
    return out;
}

std::unique_ptr<AccessorDeclarationSyntax> cloneAccessorDeclaration(
    const AccessorDeclarationSyntax& value) {
    auto out = std::make_unique<AccessorDeclarationSyntax>();
    out->keyword = value.keyword;
    out->semicolonToken = value.semicolonToken;
    if (value.body) {
        out->body = std::make_unique<BlockStatementSyntax>(
            cloneBlockStatementSyntax(*value.body));
    }
    return out;
}

} // namespace

std::unique_ptr<ExpressionSyntax> cloneExpressionSyntax(const ExpressionSyntax& value) {
    switch (value.kind()) {
    case SyntaxKind::LiteralExpression: {
        auto out = std::make_unique<LiteralExpressionSyntax>();
        out->literalToken = static_cast<const LiteralExpressionSyntax&>(value).literalToken;
        return out;
    }
    case SyntaxKind::NameExpression: {
        const auto& src = static_cast<const NameExpressionSyntax&>(value);
        auto out = std::make_unique<NameExpressionSyntax>();
        out->identifierToken = src.identifierToken;
        out->lessToken = src.lessToken;
        out->typeArguments = src.typeArguments;
        out->typeArgumentCommaTokens = src.typeArgumentCommaTokens;
        out->greaterToken = src.greaterToken;
        return out;
    }
    case SyntaxKind::LambdaExpression: {
        const auto& src = static_cast<const LambdaExpressionSyntax&>(value);
        auto out = std::make_unique<LambdaExpressionSyntax>();
        out->openParenToken = src.openParenToken;
        out->parameterTokens = src.parameterTokens;
        out->commaTokens = src.commaTokens;
        out->closeParenToken = src.closeParenToken;
        out->arrowToken = src.arrowToken;
        if (src.body) out->body = cloneExpressionSyntax(*src.body);
        return out;
    }
    case SyntaxKind::UnaryExpression: {
        const auto& src = static_cast<const UnaryExpressionSyntax&>(value);
        auto out = std::make_unique<UnaryExpressionSyntax>();
        out->operatorToken = src.operatorToken;
        if (src.operand) out->operand = cloneExpressionSyntax(*src.operand);
        return out;
    }
    case SyntaxKind::BinaryExpression: {
        const auto& src = static_cast<const BinaryExpressionSyntax&>(value);
        auto out = std::make_unique<BinaryExpressionSyntax>();
        if (src.left) out->left = cloneExpressionSyntax(*src.left);
        out->operatorToken = src.operatorToken;
        if (src.right) out->right = cloneExpressionSyntax(*src.right);
        return out;
    }
    case SyntaxKind::TypeBinaryExpression: {
        const auto& src = static_cast<const TypeBinaryExpressionSyntax&>(value);
        auto out = std::make_unique<TypeBinaryExpressionSyntax>();
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        out->operatorToken = src.operatorToken;
        out->type = src.type;
        out->designationToken = src.designationToken;
        if (src.pattern) out->pattern = clonePatternSyntax(*src.pattern);
        return out;
    }
    case SyntaxKind::TypeOfExpression: {
        const auto& src = static_cast<const TypeOfExpressionSyntax&>(value);
        auto out = std::make_unique<TypeOfExpressionSyntax>();
        out->typeofKeyword = src.typeofKeyword;
        out->openParenToken = src.openParenToken;
        out->type = src.type;
        out->closeParenToken = src.closeParenToken;
        return out;
    }
    case SyntaxKind::SwitchExpression: {
        const auto& src = static_cast<const SwitchExpressionSyntax&>(value);
        auto out = std::make_unique<SwitchExpressionSyntax>();
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        out->switchKeyword = src.switchKeyword;
        out->openBraceToken = src.openBraceToken;
        for (const auto& arm : src.arms) {
            out->arms.push_back(cloneSwitchExpressionArm(arm));
        }
        out->commaTokens = src.commaTokens;
        out->closeBraceToken = src.closeBraceToken;
        return out;
    }
    case SyntaxKind::ConditionalExpression: {
        const auto& src = static_cast<const ConditionalExpressionSyntax&>(value);
        auto out = std::make_unique<ConditionalExpressionSyntax>();
        if (src.condition) out->condition = cloneExpressionSyntax(*src.condition);
        out->questionToken = src.questionToken;
        if (src.whenTrue) out->whenTrue = cloneExpressionSyntax(*src.whenTrue);
        out->colonToken = src.colonToken;
        if (src.whenFalse) out->whenFalse = cloneExpressionSyntax(*src.whenFalse);
        return out;
    }
    case SyntaxKind::AssignmentExpression: {
        const auto& src = static_cast<const AssignmentExpressionSyntax&>(value);
        auto out = std::make_unique<AssignmentExpressionSyntax>();
        out->identifierToken = src.identifierToken;
        out->equalsToken = src.equalsToken;
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        return out;
    }
    case SyntaxKind::ParenthesizedExpression: {
        const auto& src = static_cast<const ParenthesizedExpressionSyntax&>(value);
        auto out = std::make_unique<ParenthesizedExpressionSyntax>();
        out->openParenToken = src.openParenToken;
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        out->closeParenToken = src.closeParenToken;
        return out;
    }
    case SyntaxKind::CastExpression: {
        const auto& src = static_cast<const CastExpressionSyntax&>(value);
        auto out = std::make_unique<CastExpressionSyntax>();
        out->openParenToken = src.openParenToken;
        out->type = src.type;
        out->closeParenToken = src.closeParenToken;
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        return out;
    }
    case SyntaxKind::CallExpression: {
        const auto& src = static_cast<const CallExpressionSyntax&>(value);
        auto out = std::make_unique<CallExpressionSyntax>();
        out->identifierToken = src.identifierToken;
        out->lessToken = src.lessToken;
        out->typeArguments = src.typeArguments;
        out->typeArgumentCommaTokens = src.typeArgumentCommaTokens;
        out->greaterToken = src.greaterToken;
        out->openParenToken = src.openParenToken;
        out->argumentModifiers = src.argumentModifiers;
        out->argumentNames = src.argumentNames;
        out->argumentNameColonTokens = src.argumentNameColonTokens;
        for (const auto& arg : src.arguments) {
            out->arguments.push_back(arg ? cloneExpressionSyntax(*arg) : nullptr);
        }
        out->commaTokens = src.commaTokens;
        out->closeParenToken = src.closeParenToken;
        return out;
    }
    case SyntaxKind::ThisExpression: {
        auto out = std::make_unique<ThisExpressionSyntax>();
        out->thisKeyword = static_cast<const ThisExpressionSyntax&>(value).thisKeyword;
        return out;
    }
    case SyntaxKind::BaseExpression: {
        auto out = std::make_unique<BaseExpressionSyntax>();
        out->baseKeyword = static_cast<const BaseExpressionSyntax&>(value).baseKeyword;
        return out;
    }
    case SyntaxKind::MemberCallExpression: {
        const auto& src = static_cast<const MemberCallExpressionSyntax&>(value);
        auto out = std::make_unique<MemberCallExpressionSyntax>();
        if (src.receiver) out->receiver = cloneExpressionSyntax(*src.receiver);
        out->dotToken = src.dotToken;
        out->nameToken = src.nameToken;
        out->lessToken = src.lessToken;
        out->typeArguments = src.typeArguments;
        out->typeArgumentCommaTokens = src.typeArgumentCommaTokens;
        out->greaterToken = src.greaterToken;
        out->openParenToken = src.openParenToken;
        out->argumentModifiers = src.argumentModifiers;
        out->argumentNames = src.argumentNames;
        out->argumentNameColonTokens = src.argumentNameColonTokens;
        for (const auto& arg : src.arguments) {
            out->arguments.push_back(arg ? cloneExpressionSyntax(*arg) : nullptr);
        }
        out->commaTokens = src.commaTokens;
        out->closeParenToken = src.closeParenToken;
        return out;
    }
    case SyntaxKind::MemberAccessExpression: {
        const auto& src = static_cast<const MemberAccessExpressionSyntax&>(value);
        auto out = std::make_unique<MemberAccessExpressionSyntax>();
        if (src.receiver) out->receiver = cloneExpressionSyntax(*src.receiver);
        out->dotToken = src.dotToken;
        out->nameToken = src.nameToken;
        return out;
    }
    case SyntaxKind::ElementAccessExpression: {
        const auto& src = static_cast<const ElementAccessExpressionSyntax&>(value);
        auto out = std::make_unique<ElementAccessExpressionSyntax>();
        if (src.receiver) out->receiver = cloneExpressionSyntax(*src.receiver);
        out->openBracketToken = src.openBracketToken;
        if (src.index) out->index = cloneExpressionSyntax(*src.index);
        out->closeBracketToken = src.closeBracketToken;
        return out;
    }
    case SyntaxKind::ElementAssignmentExpression: {
        const auto& src = static_cast<const ElementAssignmentExpressionSyntax&>(value);
        auto out = std::make_unique<ElementAssignmentExpressionSyntax>();
        if (src.receiver) out->receiver = cloneExpressionSyntax(*src.receiver);
        out->openBracketToken = src.openBracketToken;
        if (src.index) out->index = cloneExpressionSyntax(*src.index);
        out->closeBracketToken = src.closeBracketToken;
        out->equalsToken = src.equalsToken;
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        return out;
    }
    case SyntaxKind::MemberAssignmentExpression: {
        const auto& src = static_cast<const MemberAssignmentExpressionSyntax&>(value);
        auto out = std::make_unique<MemberAssignmentExpressionSyntax>();
        if (src.receiver) out->receiver = cloneExpressionSyntax(*src.receiver);
        out->dotToken = src.dotToken;
        out->nameToken = src.nameToken;
        out->equalsToken = src.equalsToken;
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        return out;
    }
    case SyntaxKind::NewObjectExpression: {
        const auto& src = static_cast<const NewObjectExpressionSyntax&>(value);
        auto out = std::make_unique<NewObjectExpressionSyntax>();
        out->newKeyword = src.newKeyword;
        out->type = src.type;
        out->openParenToken = src.openParenToken;
        for (const auto& arg : src.arguments) {
            out->arguments.push_back(arg ? cloneExpressionSyntax(*arg) : nullptr);
        }
        out->argumentNames = src.argumentNames;
        out->argumentNameColonTokens = src.argumentNameColonTokens;
        out->commaTokens = src.commaTokens;
        out->closeParenToken = src.closeParenToken;
        out->initializerOpenBraceToken = src.initializerOpenBraceToken;
        for (const auto& init : src.initializers) {
            out->initializers.push_back(cloneInitializerElement(init));
        }
        out->initializerCommaTokens = src.initializerCommaTokens;
        out->initializerCloseBraceToken = src.initializerCloseBraceToken;
        return out;
    }
    case SyntaxKind::NewArrayExpression: {
        const auto& src = static_cast<const NewArrayExpressionSyntax&>(value);
        auto out = std::make_unique<NewArrayExpressionSyntax>();
        out->newKeyword = src.newKeyword;
        out->elementType = src.elementType;
        out->openBracketToken = src.openBracketToken;
        if (src.length) out->length = cloneExpressionSyntax(*src.length);
        out->explicitLength = src.explicitLength;
        out->closeBracketToken = src.closeBracketToken;
        out->initializerOpenBraceToken = src.initializerOpenBraceToken;
        for (const auto& item : src.initialValues) {
            out->initialValues.push_back(item ? cloneExpressionSyntax(*item) : nullptr);
        }
        out->initializerCommaTokens = src.initializerCommaTokens;
        out->initializerCloseBraceToken = src.initializerCloseBraceToken;
        return out;
    }
    default:
        return nullptr;
    }
}

BlockStatementSyntax cloneBlockStatementSyntax(const BlockStatementSyntax& value) {
    BlockStatementSyntax out;
    out.openBraceToken = value.openBraceToken;
    for (const auto& stmt : value.statements) {
        out.statements.push_back(stmt ? cloneStatementSyntax(*stmt) : nullptr);
    }
    out.closeBraceToken = value.closeBraceToken;
    return out;
}

std::unique_ptr<StatementSyntax> cloneStatementSyntax(const StatementSyntax& value) {
    switch (value.kind()) {
    case SyntaxKind::BlockStatement:
        return std::make_unique<BlockStatementSyntax>(
            cloneBlockStatementSyntax(static_cast<const BlockStatementSyntax&>(value)));
    case SyntaxKind::ReturnStatement: {
        const auto& src = static_cast<const ReturnStatementSyntax&>(value);
        auto out = std::make_unique<ReturnStatementSyntax>();
        out->returnKeyword = src.returnKeyword;
        out->refKeyword = src.refKeyword;
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    case SyntaxKind::IfStatement: {
        const auto& src = static_cast<const IfStatementSyntax&>(value);
        auto out = std::make_unique<IfStatementSyntax>();
        out->ifKeyword = src.ifKeyword;
        out->openParenToken = src.openParenToken;
        if (src.condition) out->condition = cloneExpressionSyntax(*src.condition);
        out->closeParenToken = src.closeParenToken;
        if (src.thenStatement) out->thenStatement = cloneStatementSyntax(*src.thenStatement);
        out->elseKeyword = src.elseKeyword;
        if (src.elseStatement) out->elseStatement = cloneStatementSyntax(*src.elseStatement);
        return out;
    }
    case SyntaxKind::WhileStatement: {
        const auto& src = static_cast<const WhileStatementSyntax&>(value);
        auto out = std::make_unique<WhileStatementSyntax>();
        out->whileKeyword = src.whileKeyword;
        out->openParenToken = src.openParenToken;
        if (src.condition) out->condition = cloneExpressionSyntax(*src.condition);
        out->closeParenToken = src.closeParenToken;
        if (src.body) out->body = cloneStatementSyntax(*src.body);
        return out;
    }
    case SyntaxKind::ForStatement: {
        const auto& src = static_cast<const ForStatementSyntax&>(value);
        auto out = std::make_unique<ForStatementSyntax>();
        out->forKeyword = src.forKeyword;
        out->openParenToken = src.openParenToken;
        if (src.initializer) out->initializer = cloneStatementSyntax(*src.initializer);
        out->firstSemicolonToken = src.firstSemicolonToken;
        if (src.condition) out->condition = cloneExpressionSyntax(*src.condition);
        out->secondSemicolonToken = src.secondSemicolonToken;
        if (src.increment) out->increment = cloneExpressionSyntax(*src.increment);
        out->closeParenToken = src.closeParenToken;
        if (src.body) out->body = cloneStatementSyntax(*src.body);
        return out;
    }
    case SyntaxKind::ForeachStatement: {
        const auto& src = static_cast<const ForeachStatementSyntax&>(value);
        auto out = std::make_unique<ForeachStatementSyntax>();
        out->foreachKeyword = src.foreachKeyword;
        out->openParenToken = src.openParenToken;
        out->modifierToken = src.modifierToken;
        out->type = src.type;
        out->identifierToken = src.identifierToken;
        out->bindingCommaTokens = src.bindingCommaTokens;
        out->additionalBindings = src.additionalBindings;
        out->inKeyword = src.inKeyword;
        if (src.collection) out->collection = cloneExpressionSyntax(*src.collection);
        out->closeParenToken = src.closeParenToken;
        if (src.body) out->body = cloneStatementSyntax(*src.body);
        return out;
    }
    case SyntaxKind::DoWhileStatement: {
        const auto& src = static_cast<const DoWhileStatementSyntax&>(value);
        auto out = std::make_unique<DoWhileStatementSyntax>();
        out->doKeyword = src.doKeyword;
        if (src.body) out->body = cloneStatementSyntax(*src.body);
        out->whileKeyword = src.whileKeyword;
        out->openParenToken = src.openParenToken;
        if (src.condition) out->condition = cloneExpressionSyntax(*src.condition);
        out->closeParenToken = src.closeParenToken;
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    case SyntaxKind::BreakStatement: {
        const auto& src = static_cast<const BreakStatementSyntax&>(value);
        auto out = std::make_unique<BreakStatementSyntax>();
        out->breakKeyword = src.breakKeyword;
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    case SyntaxKind::ContinueStatement: {
        const auto& src = static_cast<const ContinueStatementSyntax&>(value);
        auto out = std::make_unique<ContinueStatementSyntax>();
        out->continueKeyword = src.continueKeyword;
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    case SyntaxKind::SwitchStatement: {
        const auto& src = static_cast<const SwitchStatementSyntax&>(value);
        auto out = std::make_unique<SwitchStatementSyntax>();
        out->switchKeyword = src.switchKeyword;
        out->openParenToken = src.openParenToken;
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        out->closeParenToken = src.closeParenToken;
        out->openBraceToken = src.openBraceToken;
        for (const auto& sec : src.sections) {
            out->sections.push_back(cloneSwitchSection(sec));
        }
        out->closeBraceToken = src.closeBraceToken;
        return out;
    }
    case SyntaxKind::ThrowStatement: {
        const auto& src = static_cast<const ThrowStatementSyntax&>(value);
        auto out = std::make_unique<ThrowStatementSyntax>();
        out->throwKeyword = src.throwKeyword;
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    case SyntaxKind::TryStatement: {
        const auto& src = static_cast<const TryStatementSyntax&>(value);
        auto out = std::make_unique<TryStatementSyntax>();
        out->tryKeyword = src.tryKeyword;
        out->body = cloneBlockStatementSyntax(src.body);
        for (const auto& c : src.catches) {
            out->catches.push_back(cloneCatchClause(c));
        }
        out->finallyKeyword = src.finallyKeyword;
        if (src.finallyBody) {
            out->finallyBody = cloneBlockStatementSyntax(*src.finallyBody);
        }
        return out;
    }
    case SyntaxKind::YieldWaitStatement: {
        const auto& src = static_cast<const YieldWaitStatementSyntax&>(value);
        auto out = std::make_unique<YieldWaitStatementSyntax>();
        out->yieldKeyword = src.yieldKeyword;
        out->waitTicksToken = src.waitTicksToken;
        out->openParenToken = src.openParenToken;
        if (src.delay) out->delay = cloneExpressionSyntax(*src.delay);
        out->commaToken = src.commaToken;
        if (src.secondArgument) out->secondArgument = cloneExpressionSyntax(*src.secondArgument);
        out->closeParenToken = src.closeParenToken;
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    case SyntaxKind::YieldBreakStatement: {
        const auto& src = static_cast<const YieldBreakStatementSyntax&>(value);
        auto out = std::make_unique<YieldBreakStatementSyntax>();
        out->yieldKeyword = src.yieldKeyword;
        out->breakKeyword = src.breakKeyword;
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    case SyntaxKind::EventSubscriptionStatement: {
        const auto& src = static_cast<const EventSubscriptionStatementSyntax&>(value);
        auto out = std::make_unique<EventSubscriptionStatementSyntax>();
        if (src.receiver) out->receiver = cloneExpressionSyntax(*src.receiver);
        out->dotToken = src.dotToken;
        out->eventNameToken = src.eventNameToken;
        out->operatorToken = src.operatorToken;
        if (src.handler) out->handler = cloneExpressionSyntax(*src.handler);
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    case SyntaxKind::VariableDeclarationStatement: {
        const auto& src = static_cast<const VariableDeclarationStatementSyntax&>(value);
        auto out = std::make_unique<VariableDeclarationStatementSyntax>();
        out->constKeyword = src.constKeyword;
        out->refKeyword = src.refKeyword;
        out->type = src.type;
        out->identifierToken = src.identifierToken;
        out->equalsToken = src.equalsToken;
        out->initializerRefKeyword = src.initializerRefKeyword;
        if (src.initializer) out->initializer = cloneExpressionSyntax(*src.initializer);
        out->semicolonToken = src.semicolonToken;
        out->isUsingDeclaration = src.isUsingDeclaration;
        return out;
    }
    case SyntaxKind::ExpressionStatement: {
        const auto& src = static_cast<const ExpressionStatementSyntax&>(value);
        auto out = std::make_unique<ExpressionStatementSyntax>();
        if (src.expression) out->expression = cloneExpressionSyntax(*src.expression);
        out->semicolonToken = src.semicolonToken;
        return out;
    }
    default:
        return nullptr;
    }
}

ParameterSyntax cloneParameterSyntax(const ParameterSyntax& value) {
    ParameterSyntax out;
    out.attributes = value.attributes;
    out.paramsKeyword = value.paramsKeyword;
    out.thisKeyword = value.thisKeyword;
    out.modifierToken = value.modifierToken;
    out.type = value.type;
    out.identifierToken = value.identifierToken;
    out.equalsToken = value.equalsToken;
    if (value.defaultValue) out.defaultValue = cloneExpressionSyntax(*value.defaultValue);
    return out;
}

FieldDeclarationSyntax cloneFieldDeclarationSyntax(const FieldDeclarationSyntax& value) {
    FieldDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.type = value.type;
    out.identifierToken = value.identifierToken;
    out.equalsToken = value.equalsToken;
    if (value.initializer) out.initializer = cloneExpressionSyntax(*value.initializer);
    out.semicolonToken = value.semicolonToken;
    return out;
}

ConstructorDeclarationSyntax cloneConstructorDeclarationSyntax(
    const ConstructorDeclarationSyntax& value) {
    ConstructorDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.identifierToken = value.identifierToken;
    out.openParenToken = value.openParenToken;
    for (const auto& param : value.parameters) {
        out.parameters.push_back(cloneParameterSyntax(param));
    }
    out.commaTokens = value.commaTokens;
    out.closeParenToken = value.closeParenToken;
    out.initializerColonToken = value.initializerColonToken;
    out.baseKeyword = value.baseKeyword;
    out.initializerOpenParenToken = value.initializerOpenParenToken;
    for (const auto& arg : value.baseArguments) {
        out.baseArguments.push_back(arg ? cloneExpressionSyntax(*arg) : nullptr);
    }
    out.baseArgumentCommaTokens = value.baseArgumentCommaTokens;
    out.initializerCloseParenToken = value.initializerCloseParenToken;
    out.body = cloneBlockStatementSyntax(value.body);
    return out;
}

PropertyDeclarationSyntax clonePropertyDeclarationSyntax(
    const PropertyDeclarationSyntax& value) {
    PropertyDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.staticKeyword = value.staticKeyword;
    out.type = value.type;
    out.identifierToken = value.identifierToken;
    out.openBraceToken = value.openBraceToken;
    if (value.getter) out.getter = cloneAccessorDeclaration(*value.getter);
    if (value.setter) out.setter = cloneAccessorDeclaration(*value.setter);
    out.closeBraceToken = value.closeBraceToken;
    out.equalsToken = value.equalsToken;
    if (value.initializer) out.initializer = cloneExpressionSyntax(*value.initializer);
    out.semicolonToken = value.semicolonToken;
    return out;
}

SequenceDeclarationSyntax cloneSequenceDeclarationSyntax(
    const SequenceDeclarationSyntax& value) {
    SequenceDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.sequenceKeyword = value.sequenceKeyword;
    out.resultLessToken = value.resultLessToken;
    if (value.resultType) {
        out.resultType = std::make_unique<TypeSyntax>(*value.resultType);
    }
    out.resultGreaterToken = value.resultGreaterToken;
    out.identifierToken = value.identifierToken;
    out.openParenToken = value.openParenToken;
    for (const auto& param : value.parameters) {
        out.parameters.push_back(cloneParameterSyntax(param));
    }
    out.commaTokens = value.commaTokens;
    out.closeParenToken = value.closeParenToken;
    out.body = cloneBlockStatementSyntax(value.body);
    return out;
}

FunctionDeclarationSyntax cloneFunctionDeclarationSyntax(
    const FunctionDeclarationSyntax& value) {
    FunctionDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.staticKeyword = value.staticKeyword;
    out.refReturnKeyword = value.refReturnKeyword;
    out.returnType = value.returnType;
    out.identifierToken = value.identifierToken;
    out.typeParameterLessToken = value.typeParameterLessToken;
    out.typeParameters = value.typeParameters;
    out.typeParameterCommaTokens = value.typeParameterCommaTokens;
    out.typeParameterGreaterToken = value.typeParameterGreaterToken;
    out.constraints = value.constraints;
    out.openParenToken = value.openParenToken;
    for (const auto& param : value.parameters) {
        out.parameters.push_back(cloneParameterSyntax(param));
    }
    out.commaTokens = value.commaTokens;
    out.closeParenToken = value.closeParenToken;
    out.semicolonToken = value.semicolonToken;
    out.body = cloneBlockStatementSyntax(value.body);
    out.hygienicModules = value.hygienicModules;
    return out;
}

ClassDeclarationSyntax cloneClassDeclarationSyntax(const ClassDeclarationSyntax& value) {
    ClassDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.classKeyword = value.classKeyword;
    out.identifierToken = value.identifierToken;
    out.typeParameterLessToken = value.typeParameterLessToken;
    out.typeParameters = value.typeParameters;
    out.typeParameterCommaTokens = value.typeParameterCommaTokens;
    out.typeParameterGreaterToken = value.typeParameterGreaterToken;
    out.constraints = value.constraints;
    out.colonToken = value.colonToken;
    out.interfaces = value.interfaces;
    out.interfaceCommaTokens = value.interfaceCommaTokens;
    out.openBraceToken = value.openBraceToken;
    for (const auto& f : value.fields) out.fields.push_back(cloneFieldDeclarationSyntax(f));
    out.events = value.events;
    for (const auto& m : value.methods) out.methods.push_back(cloneFunctionDeclarationSyntax(m));
    for (const auto& s : value.sequences) out.sequences.push_back(cloneSequenceDeclarationSyntax(s));
    for (const auto& c : value.constructors) out.constructors.push_back(cloneConstructorDeclarationSyntax(c));
    for (const auto& p : value.properties) out.properties.push_back(clonePropertyDeclarationSyntax(p));
    out.closeBraceToken = value.closeBraceToken;
    return out;
}

StructDeclarationSyntax cloneStructDeclarationSyntax(const StructDeclarationSyntax& value) {
    StructDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.structKeyword = value.structKeyword;
    out.identifierToken = value.identifierToken;
    out.typeParameterLessToken = value.typeParameterLessToken;
    out.typeParameters = value.typeParameters;
    out.typeParameterCommaTokens = value.typeParameterCommaTokens;
    out.typeParameterGreaterToken = value.typeParameterGreaterToken;
    out.constraints = value.constraints;
    out.colonToken = value.colonToken;
    out.interfaces = value.interfaces;
    out.interfaceCommaTokens = value.interfaceCommaTokens;
    out.openBraceToken = value.openBraceToken;
    for (const auto& f : value.fields) out.fields.push_back(cloneFieldDeclarationSyntax(f));
    out.events = value.events;
    for (const auto& m : value.methods) out.methods.push_back(cloneFunctionDeclarationSyntax(m));
    for (const auto& s : value.sequences) out.sequences.push_back(cloneSequenceDeclarationSyntax(s));
    for (const auto& c : value.constructors) out.constructors.push_back(cloneConstructorDeclarationSyntax(c));
    for (const auto& p : value.properties) out.properties.push_back(clonePropertyDeclarationSyntax(p));
    out.closeBraceToken = value.closeBraceToken;
    return out;
}

InterfaceDeclarationSyntax cloneInterfaceDeclarationSyntax(const InterfaceDeclarationSyntax& value) {
    InterfaceDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.interfaceKeyword = value.interfaceKeyword;
    out.identifierToken = value.identifierToken;
    out.typeParameterLessToken = value.typeParameterLessToken;
    out.typeParameters = value.typeParameters;
    out.typeParameterCommaTokens = value.typeParameterCommaTokens;
    out.typeParameterGreaterToken = value.typeParameterGreaterToken;
    out.constraints = value.constraints;
    out.openBraceToken = value.openBraceToken;
    for (const auto& m : value.methods) {
        InterfaceMethodDeclarationSyntax method;
        method.attributes = m.attributes;
        method.modifiers = m.modifiers;
        method.returnType = m.returnType;
        method.identifierToken = m.identifierToken;
        method.typeParameterLessToken = m.typeParameterLessToken;
        method.typeParameters = m.typeParameters;
        method.typeParameterCommaTokens = m.typeParameterCommaTokens;
        method.typeParameterGreaterToken = m.typeParameterGreaterToken;
        method.constraints = m.constraints;
        method.openParenToken = m.openParenToken;
        for (const auto& param : m.parameters) {
            method.parameters.push_back(cloneParameterSyntax(param));
        }
        method.commaTokens = m.commaTokens;
        method.closeParenToken = m.closeParenToken;
        method.semicolonToken = m.semicolonToken;
        if (m.body) method.body = cloneBlockStatementSyntax(*m.body);
        out.methods.push_back(std::move(method));
    }
    out.closeBraceToken = value.closeBraceToken;
    return out;
}

DelegateDeclarationSyntax cloneDelegateDeclarationSyntax(const DelegateDeclarationSyntax& value) {
    DelegateDeclarationSyntax out;
    out.attributes = value.attributes;
    out.modifiers = value.modifiers;
    out.delegateKeyword = value.delegateKeyword;
    out.returnType = value.returnType;
    out.identifierToken = value.identifierToken;
    out.typeParameterLessToken = value.typeParameterLessToken;
    out.typeParameters = value.typeParameters;
    out.typeParameterCommaTokens = value.typeParameterCommaTokens;
    out.typeParameterGreaterToken = value.typeParameterGreaterToken;
    out.constraints = value.constraints;
    out.openParenToken = value.openParenToken;
    for (const auto& param : value.parameters) {
        out.parameters.push_back(cloneParameterSyntax(param));
    }
    out.commaTokens = value.commaTokens;
    out.closeParenToken = value.closeParenToken;
    out.semicolonToken = value.semicolonToken;
    return out;
}

} // namespace realscript::syntax
