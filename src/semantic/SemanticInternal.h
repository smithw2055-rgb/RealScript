#pragma once

#include "realscript/semantic/Semantic.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace realscript::semantic::detail {

struct FlexibleCallPlan {
    const FunctionSymbol* function = nullptr;
    std::vector<std::vector<std::size_t>> sources;
    bool expandedParams = false;
    int score = std::numeric_limits<int>::max();
};

[[nodiscard]] bool sameExactType(
    const BoundExpression& expression,
    PrimitiveType targetType,
    const std::string& targetTypeName,
    const TypeSymbolMap& visibleTypes);

[[nodiscard]] const FieldSymbol* findField(
    const TypeSymbol& type,
    const std::string& name);

[[nodiscard]] const PropertySymbol* findProperty(
    const TypeSymbol& type,
    const std::string& name);

[[nodiscard]] const EnumMemberSymbol* findEnumMember(
    const TypeSymbol& type,
    const std::string& name);

[[nodiscard]] std::vector<const FunctionSymbol*> findMethods(
    const TypeSymbol& type,
    const std::string& name,
    bool staticMethod);

[[nodiscard]] PrimitiveType commonNumericType(
    PrimitiveType left,
    PrimitiveType right) noexcept;

[[nodiscard]] bool isImplicitNumericConstant(
    const BoundExpression& expression,
    PrimitiveType target) noexcept;

[[nodiscard]] ParameterModifier syntaxModifier(
    const std::optional<syntax::SyntaxToken>& token) noexcept;

[[nodiscard]] std::string unaryOperatorMethodName(syntax::SyntaxKind kind);
[[nodiscard]] std::string binaryOperatorMethodName(syntax::SyntaxKind kind);

[[nodiscard]] std::optional<syntax::SyntaxToken> compoundBinaryOperatorToken(
    const syntax::SyntaxToken& assignmentToken);

[[nodiscard]] std::unique_ptr<BoundVariableExpression> variableExpression(
    const VariableSymbol& variable,
    text::TextSpan span = {});

[[nodiscard]] std::unique_ptr<BoundVariableExpression> storageVariableExpression(
    const VariableSymbol& variable,
    text::TextSpan span = {});

[[nodiscard]] std::size_t visibleParameterOffset(
    const FunctionSymbol& function) noexcept;

void recordSymbolReference(
    std::vector<SymbolOccurrence>& occurrences,
    const std::string& sourceName,
    SymbolId id,
    SymbolKind kind,
    const std::string& name,
    const std::string& detail,
    text::TextSpan span);

void recordSymbolReference(
    std::vector<SymbolOccurrence>& occurrences,
    const std::string& sourceName,
    SymbolId id,
    SymbolKind kind,
    const std::string& name,
    text::TextSpan span);

[[nodiscard]] FunctionSymbol makeIntrinsicFunction(
    std::string intrinsicName,
    PrimitiveType returnType,
    const std::string& returnTypeName,
    const std::vector<std::pair<PrimitiveType, std::string>>& params);

[[nodiscard]] std::string formatLiteralTokenToString(
    const syntax::TokenValue& val);

[[nodiscard]] std::vector<const syntax::ExpressionSyntax*> syntaxPointers(
    const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& values);

[[nodiscard]] const FunctionSymbol* findUserDefinedConversion(
    PrimitiveType sourceType,
    const std::string& sourceTypeName,
    PrimitiveType targetType,
    const std::string& targetTypeName,
    bool allowExplicit,
    const TypeSymbolMap& visibleTypes,
    bool& ambiguous);

[[nodiscard]] bool exactParameterMatch(
    const BoundExpression& expression,
    const VariableSymbol& parameter,
    const TypeSymbolMap& visibleTypes);

[[nodiscard]] const FunctionSymbol* selectBest(
    const std::vector<const FunctionSymbol*>& candidates,
    const std::vector<std::unique_ptr<BoundExpression>>& arguments,
    const std::vector<std::optional<syntax::SyntaxToken>>* modifiers,
    const TypeSymbolMap& visibleTypes,
    bool& ambiguous);

[[nodiscard]] FlexibleCallPlan selectFlexibleCall(
    const std::vector<const FunctionSymbol*>& candidates,
    const std::vector<std::unique_ptr<BoundExpression>>& arguments,
    const std::vector<std::optional<syntax::SyntaxToken>>* names,
    const std::vector<std::optional<syntax::SyntaxToken>>* modifiers,
    const TypeSymbolMap& visibleTypes,
    bool& ambiguous);

[[nodiscard]] const FunctionSymbol* delegateInvokeMethod(
    const TypeSymbol& delegateType) noexcept;

[[nodiscard]] bool delegateMethodMatches(
    const FunctionSymbol& candidate,
    const FunctionSymbol& invoke);

void collectLambdaNames(
    const syntax::ExpressionSyntax& expression,
    std::vector<syntax::SyntaxToken>& names);

[[nodiscard]] bool isReadonlyFieldWritable(
    const FieldSymbol& field,
    bool currentConstructor,
    bool currentStaticMethod,
    const std::optional<TypeSymbol>& currentOwnerType) noexcept;

[[nodiscard]] std::optional<FunctionSymbol> findStaticConstructorForFunction(
    const TypeSymbolMap& visibleTypes,
    const FunctionSymbol& function);

} // namespace realscript::semantic::detail
