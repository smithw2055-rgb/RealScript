#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/syntax/Syntax.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace realscript::semantic {

using SymbolId = std::uint64_t;

enum class PrimitiveType {
    Error,
    Void,
    Bool,
    Int,
    Long,
    Double,
    String,
    Object,
    Struct,
    Enum,
    Array,
    Handle,
    Null,
};

enum class TypeKind {
    Class,
    Struct,
    Enum,
};

[[nodiscard]] const char* primitiveTypeName(PrimitiveType type) noexcept;
[[nodiscard]] PrimitiveType resolvePrimitiveType(const std::string& name) noexcept;
[[nodiscard]] bool isNumericType(PrimitiveType type) noexcept;
[[nodiscard]] bool isIntegralType(PrimitiveType type) noexcept;
[[nodiscard]] bool isReferenceType(PrimitiveType type) noexcept;
[[nodiscard]] bool isExactType(PrimitiveType type) noexcept;
[[nodiscard]] std::string arrayTypeName(
    PrimitiveType elementType,
    const std::string& elementTypeName = {});
[[nodiscard]] bool decodeArrayTypeName(
    const std::string& name,
    PrimitiveType& elementType,
    std::string& elementTypeName);

enum class ParameterModifier {
    None,
    Ref,
    Out,
    In,
};

enum class ConversionKind {
    None,
    Identity,
    NullToString,
    NullToObject,
    NullToArray,
    IntToLong,
    IntToDouble,
    LongToDouble,
};

[[nodiscard]] ConversionKind classifyConversion(
    PrimitiveType from,
    PrimitiveType to) noexcept;
[[nodiscard]] int conversionRank(PrimitiveType from, PrimitiveType to) noexcept;

struct VariableSymbol {
    std::string name;
    PrimitiveType type = PrimitiveType::Error;
    std::string typeName;
    PrimitiveType storageType = PrimitiveType::Error;
    std::string storageTypeName;
    ParameterModifier modifier = ParameterModifier::None;
    std::size_t index = 0;
    bool parameter = false;
    text::TextSpan declarationSpan;
    text::TextSpan scopeSpan;
    SymbolId id = 0;
};

[[nodiscard]] inline PrimitiveType storageTypeOf(
    const VariableSymbol& variable) noexcept {
    return variable.storageType == PrimitiveType::Error
        ? variable.type
        : variable.storageType;
}

[[nodiscard]] inline const std::string& storageTypeNameOf(
    const VariableSymbol& variable) noexcept {
    return variable.storageType == PrimitiveType::Error
        ? variable.typeName
        : variable.storageTypeName;
}

struct FieldSymbol {
    std::string name;
    PrimitiveType type = PrimitiveType::Error;
    std::string typeName;
    std::size_t index = 0;
    bool synthetic = false;
    std::string sourceName;
    text::TextSpan declarationSpan;
    SymbolId id = 0;
};

struct FunctionSymbol {
    SymbolId id = 0;
    std::string moduleName;
    std::string name;
    std::string ownerTypeName;
    SymbolId ownerTypeId = 0;
    PrimitiveType returnType = PrimitiveType::Error;
    std::string returnTypeName;
    std::vector<VariableSymbol> parameters;
    bool method = false;
    bool staticMethod = false;
    bool constructor = false;
    bool propertyGetter = false;
    bool propertySetter = false;
    bool synthetic = false;
    std::string sourceName;
    text::TextSpan declarationSpan;
    text::TextSpan bodySpan;
};

struct EventHandlerSymbol {
    FunctionSymbol function;
    FieldSymbol enabledField;
};

struct EventSubscriptionSymbol {
    text::TextSpan span;
    FieldSymbol enabledField;
    bool enabled = true;
};

struct EventSymbol {
    SymbolId id = 0;
    std::string name;
    std::string delegateName;
    std::vector<VariableSymbol> parameters;
    std::vector<EventHandlerSymbol> handlers;
    std::vector<EventSubscriptionSymbol> subscriptions;
    std::string sourceName;
    text::TextSpan declarationSpan;
};

struct PropertySymbol {
    std::string name;
    PrimitiveType type = PrimitiveType::Error;
    std::string typeName;
    bool staticProperty = false;
    std::optional<FunctionSymbol> getter;
    std::optional<FunctionSymbol> setter;
    std::size_t backingFieldIndex = std::numeric_limits<std::size_t>::max();
    std::string sourceName;
    text::TextSpan declarationSpan;
    SymbolId id = 0;
};

struct EnumMemberSymbol {
    std::string name;
    std::int64_t value = 0;
    std::string sourceName;
    text::TextSpan declarationSpan;
    SymbolId id = 0;
};

struct TypeSymbol {
    SymbolId id = 0;
    TypeKind kind = TypeKind::Class;
    bool synthetic = false;
    std::string moduleName;
    std::string name;
    std::vector<FieldSymbol> fields;
    std::vector<FunctionSymbol> methods;
    std::vector<FunctionSymbol> constructors;
    std::vector<PropertySymbol> properties;
    std::vector<EventSymbol> events;
    std::vector<EnumMemberSymbol> enumMembers;
    std::string sourceName;
    text::TextSpan declarationSpan;
};

enum class SymbolKind {
    Module,
    Type,
    Field,
    EnumMember,
    Function,
    Property,
    Parameter,
    Local,
};

struct SymbolOccurrence {
    SymbolId id = 0;
    SymbolKind kind = SymbolKind::Local;
    std::string name;
    std::string detail;
    std::string sourceName;
    text::TextSpan span;
    bool definition = false;
};

using TypeSymbolMap = std::unordered_map<std::string, TypeSymbol>;
using FunctionOverloadMap =
    std::unordered_map<std::string, std::vector<FunctionSymbol>>;

[[nodiscard]] std::string canonicalTypeName(const TypeSymbol& type);
[[nodiscard]] SymbolId stableTypeId(const TypeSymbol& type);
[[nodiscard]] SymbolId stableTypeId(const std::string& canonicalName);
[[nodiscard]] std::string referenceWrapperTypeName(
    const std::string& moduleName,
    PrimitiveType sourceType,
    const std::string& sourceTypeName = {});
[[nodiscard]] TypeSymbol declareTypeShell(
    const std::string& moduleName,
    const syntax::ClassDeclarationSyntax& syntax);
[[nodiscard]] TypeSymbol declareTypeShell(
    const std::string& moduleName,
    const syntax::StructDeclarationSyntax& syntax);
[[nodiscard]] TypeSymbol declareTypeShell(
    const std::string& moduleName,
    const syntax::EnumDeclarationSyntax& syntax);
[[nodiscard]] bool populateTypeFields(
    TypeSymbol& type,
    const syntax::ClassDeclarationSyntax& syntax,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] bool populateTypeFields(
    TypeSymbol& type,
    const syntax::StructDeclarationSyntax& syntax,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] bool populateEnumMembers(
    TypeSymbol& type,
    const syntax::EnumDeclarationSyntax& syntax,
    diagnostics::DiagnosticBag& diagnostics);

[[nodiscard]] std::string canonicalFunctionKey(const FunctionSymbol& function);
[[nodiscard]] std::string canonicalFunctionSignature(const FunctionSymbol& function);
[[nodiscard]] SymbolId stableFunctionId(const FunctionSymbol& function);
[[nodiscard]] FunctionSymbol declareFunctionSymbol(
    const std::string& moduleName,
    const syntax::FunctionDeclarationSyntax& syntax,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    const TypeSymbol* owner = nullptr);
[[nodiscard]] FunctionSymbol declareConstructorSymbol(
    const std::string& moduleName,
    const syntax::ConstructorDeclarationSyntax& syntax,
    const TypeSymbol& owner,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] PropertySymbol declarePropertySymbol(
    const std::string& moduleName,
    const syntax::PropertyDeclarationSyntax& syntax,
    const TypeSymbol& owner,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics);

struct FunctionBindingInput {
    FunctionSymbol symbol;
    std::string sourceName;
    const syntax::BlockStatementSyntax* body = nullptr;
    std::vector<std::string> parameterNames;
    std::vector<text::TextSpan> parameterSpans;
    bool syntheticAutoGetter = false;
    bool syntheticAutoSetter = false;
    FieldSymbol syntheticField;
    const syntax::SequenceDeclarationSyntax* sequence = nullptr;
    std::size_t sequenceSegment = 0;
    FieldSymbol sequenceTargetField;
    std::optional<FunctionSymbol> sequenceNextCallback;
    const syntax::LambdaExpressionSyntax* eventLambda = nullptr;
};

struct ModuleBindingInput {
    std::string moduleName;
    std::vector<const syntax::CompilationUnitSyntax*> units;
    std::vector<FunctionSymbol> declarations;
    std::vector<FunctionBindingInput> functionBindings;
    std::vector<TypeSymbol> types;
    FunctionOverloadMap visibleFunctions;
    TypeSymbolMap visibleTypes;
};

enum class BoundNodeKind {
    ErrorExpression,
    LiteralExpression,
    VariableExpression,
    UnaryExpression,
    BinaryExpression,
    AssignmentExpression,
    ConversionExpression,
    CallExpression,
    ReferenceCallExpression,
    EventInvocationExpression,
    NewObjectExpression,
    NewStructExpression,
    StructFieldAccessExpression,
    StructFieldAssignmentExpression,
    NewArrayExpression,
    ArrayLengthExpression,
    ElementAccessExpression,
    MemberAccessExpression,
    MemberAssignmentExpression,
    PropertyAssignmentExpression,
    ElementAssignmentExpression,
    BlockStatement,
    ReturnStatement,
    IfStatement,
    WhileStatement,
    ForStatement,
    ForeachStatement,
    DoWhileStatement,
    BreakStatement,
    ContinueStatement,
    SwitchStatement,
    EventSubscriptionStatement,
    VariableDeclarationStatement,
    ExpressionStatement,
};

struct BoundNode {
    virtual ~BoundNode() = default;
    [[nodiscard]] virtual BoundNodeKind kind() const noexcept = 0;
};

struct BoundExpression : BoundNode {
    PrimitiveType type = PrimitiveType::Error;
    std::string typeName;
    text::TextSpan span;
};

struct BoundStatement : BoundNode {
    text::TextSpan span;
};

struct BoundErrorExpression final : BoundExpression {
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ErrorExpression;
    }
};

struct BoundLiteralExpression final : BoundExpression {
    syntax::TokenValue value;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::LiteralExpression;
    }
};

struct BoundVariableExpression final : BoundExpression {
    VariableSymbol variable;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::VariableExpression;
    }
};

enum class BoundUnaryOperatorKind {
    Identity,
    Negation,
    LogicalNegation,
};

struct BoundUnaryExpression final : BoundExpression {
    BoundUnaryOperatorKind operatorKind = BoundUnaryOperatorKind::Identity;
    std::unique_ptr<BoundExpression> operand;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::UnaryExpression;
    }
};

enum class BoundBinaryOperatorKind {
    Addition,
    Subtraction,
    Multiplication,
    Division,
    Remainder,
    Equals,
    NotEquals,
    Less,
    LessOrEquals,
    Greater,
    GreaterOrEquals,
    LogicalAnd,
    LogicalOr,
};

struct BoundBinaryExpression final : BoundExpression {
    BoundBinaryOperatorKind operatorKind = BoundBinaryOperatorKind::Addition;
    std::unique_ptr<BoundExpression> left;
    std::unique_ptr<BoundExpression> right;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::BinaryExpression;
    }
};

struct BoundAssignmentExpression final : BoundExpression {
    VariableSymbol variable;
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::AssignmentExpression;
    }
};

struct BoundConversionExpression final : BoundExpression {
    ConversionKind conversion = ConversionKind::None;
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ConversionExpression;
    }
};

struct BoundCallExpression final : BoundExpression {
    FunctionSymbol function;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::CallExpression;
    }
};


struct BoundReferenceCallArgument {
    ParameterModifier modifier = ParameterModifier::None;
    std::unique_ptr<BoundExpression> value;
    VariableSymbol variable;
    TypeSymbol wrapperType;
    FieldSymbol valueField;
    bool forwarded = false;
};

struct BoundReferenceCallExpression final : BoundExpression {
    FunctionSymbol function;
    std::vector<BoundReferenceCallArgument> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ReferenceCallExpression;
    }
};

struct BoundEventInvocationExpression final : BoundExpression {
    TypeSymbol ownerType;
    EventSymbol event;
    std::unique_ptr<BoundExpression> receiver;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::EventInvocationExpression;
    }
};

struct BoundNewObjectExpression final : BoundExpression {
    TypeSymbol objectType;
    std::optional<FunctionSymbol> constructor;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::NewObjectExpression;
    }
};

struct BoundNewStructExpression final : BoundExpression {
    TypeSymbol structType;
    std::optional<FunctionSymbol> constructor;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::NewStructExpression;
    }
};

struct BoundStructFieldAccessExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> receiver;
    TypeSymbol ownerType;
    FieldSymbol field;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::StructFieldAccessExpression;
    }
};

struct BoundStructFieldAssignmentExpression final : BoundExpression {
    VariableSymbol variable;
    TypeSymbol ownerType;
    FieldSymbol field;
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::StructFieldAssignmentExpression;
    }
};


struct BoundNewArrayExpression final : BoundExpression {
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    std::unique_ptr<BoundExpression> length;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::NewArrayExpression;
    }
};

struct BoundArrayLengthExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> receiver;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ArrayLengthExpression;
    }
};

struct BoundElementAccessExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> receiver;
    std::unique_ptr<BoundExpression> index;
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ElementAccessExpression;
    }
};

struct BoundMemberAccessExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> receiver;
    TypeSymbol ownerType;
    FieldSymbol field;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::MemberAccessExpression;
    }
};

struct BoundMemberAssignmentExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> receiver;
    TypeSymbol ownerType;
    FieldSymbol field;
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::MemberAssignmentExpression;
    }
};

struct BoundPropertyAssignmentExpression final : BoundExpression {
    FunctionSymbol setter;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    std::unique_ptr<BoundExpression> assignedValue;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::PropertyAssignmentExpression;
    }
};


struct BoundElementAssignmentExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> receiver;
    std::unique_ptr<BoundExpression> index;
    std::unique_ptr<BoundExpression> expression;
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ElementAssignmentExpression;
    }
};

struct BoundBlockStatement final : BoundStatement {
    std::vector<std::unique_ptr<BoundStatement>> statements;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::BlockStatement;
    }
};

struct BoundReturnStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ReturnStatement;
    }
};

struct BoundIfStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> condition;
    std::unique_ptr<BoundStatement> thenStatement;
    std::unique_ptr<BoundStatement> elseStatement;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::IfStatement;
    }
};

struct BoundWhileStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> condition;
    std::unique_ptr<BoundStatement> body;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::WhileStatement;
    }
};

struct BoundForStatement final : BoundStatement {
    std::unique_ptr<BoundStatement> initializer;
    std::unique_ptr<BoundExpression> condition;
    std::unique_ptr<BoundExpression> increment;
    std::unique_ptr<BoundStatement> body;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ForStatement;
    }
};

struct BoundForeachStatement final : BoundStatement {
    VariableSymbol collectionVariable;
    VariableSymbol indexVariable;
    VariableSymbol iterationVariable;
    std::unique_ptr<BoundExpression> collection;
    std::unique_ptr<BoundExpression> count;
    std::unique_ptr<BoundExpression> element;
    std::unique_ptr<BoundStatement> body;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ForeachStatement;
    }
};

struct BoundDoWhileStatement final : BoundStatement {
    std::unique_ptr<BoundStatement> body;
    std::unique_ptr<BoundExpression> condition;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::DoWhileStatement;
    }
};

struct BoundBreakStatement final : BoundStatement {
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::BreakStatement;
    }
};

struct BoundContinueStatement final : BoundStatement {
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ContinueStatement;
    }
};

struct BoundSwitchSection {
    std::unique_ptr<BoundExpression> label;
    std::vector<std::unique_ptr<BoundStatement>> statements;
    text::TextSpan span;
};

struct BoundSwitchStatement final : BoundStatement {
    VariableSymbol valueVariable;
    std::unique_ptr<BoundExpression> expression;
    std::vector<BoundSwitchSection> sections;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::SwitchStatement;
    }
};

struct BoundEventSubscriptionStatement final : BoundStatement {
    TypeSymbol ownerType;
    std::unique_ptr<BoundExpression> receiver;
    FieldSymbol enabledField;
    bool enabled = true;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::EventSubscriptionStatement;
    }
};

struct BoundVariableDeclarationStatement final : BoundStatement {
    VariableSymbol variable;
    std::unique_ptr<BoundExpression> initializer;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::VariableDeclarationStatement;
    }
};

struct BoundExpressionStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ExpressionStatement;
    }
};

struct BoundFunction {
    FunctionSymbol symbol;
    std::unique_ptr<BoundBlockStatement> body;
    std::size_t variableCount = 0;
    std::vector<VariableSymbol> variables;
};

struct SemanticModel {
    std::string moduleName;
    std::vector<TypeSymbol> types;
    std::vector<BoundFunction> functions;
    std::vector<SymbolOccurrence> occurrences;
};

class Binder {
public:
    explicit Binder(diagnostics::DiagnosticBag& diagnostics);

    [[nodiscard]] SemanticModel bind(const syntax::CompilationUnitSyntax& syntax);
    [[nodiscard]] SemanticModel bindModule(const ModuleBindingInput& input);

private:
    [[nodiscard]] BoundFunction bindFunction(
        const FunctionBindingInput& input);
    [[nodiscard]] std::unique_ptr<BoundBlockStatement> bindSequenceSegment(
        const FunctionBindingInput& input);
    [[nodiscard]] std::unique_ptr<BoundExpression> makeSequenceFieldAccess(
        const FieldSymbol& field, text::TextSpan span);
    [[nodiscard]] std::unique_ptr<BoundExpression> makeVariableAccess(
        const VariableSymbol& variable, text::TextSpan span);
    [[nodiscard]] const FunctionSymbol* findScheduleFunction() const noexcept;
    [[nodiscard]] std::unique_ptr<BoundBlockStatement> bindBlockStatement(
        const syntax::BlockStatementSyntax& syntax,
        bool createScope);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindEmbeddedStatement(
        const syntax::StatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindStatement(
        const syntax::StatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindReturnStatement(
        const syntax::ReturnStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindIfStatement(
        const syntax::IfStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindWhileStatement(
        const syntax::WhileStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindForStatement(
        const syntax::ForStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindForeachStatement(
        const syntax::ForeachStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindDoWhileStatement(
        const syntax::DoWhileStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindBreakStatement(
        const syntax::BreakStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindContinueStatement(
        const syntax::ContinueStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindSwitchStatement(
        const syntax::SwitchStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindEventSubscriptionStatement(
        const syntax::EventSubscriptionStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindVariableDeclaration(
        const syntax::VariableDeclarationStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindExpressionStatement(
        const syntax::ExpressionStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindExpression(
        const syntax::ExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindLiteralExpression(
        const syntax::LiteralExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindNameExpression(
        const syntax::NameExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindThisExpression(
        const syntax::ThisExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindUnaryExpression(
        const syntax::UnaryExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindBinaryExpression(
        const syntax::BinaryExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindAssignmentExpression(
        const syntax::AssignmentExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindCallExpression(
        const syntax::CallExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindSelectedCall(
        const FunctionSymbol& function,
        std::vector<std::unique_ptr<BoundExpression>> arguments,
        const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& syntaxArguments,
        const std::vector<std::optional<syntax::SyntaxToken>>& argumentModifiers,
        std::unique_ptr<BoundExpression> receiver,
        text::TextSpan span,
        const std::string& context);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindMemberCallExpression(
        const syntax::MemberCallExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindNewObjectExpression(
        const syntax::NewObjectExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindNewArrayExpression(
        const syntax::NewArrayExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindElementAccessExpression(
        const syntax::ElementAccessExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindMemberAccessExpression(
        const syntax::MemberAccessExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindMemberAssignmentExpression(
        const syntax::MemberAssignmentExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindElementAssignmentExpression(
        const syntax::ElementAssignmentExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> convertExpression(
        std::unique_ptr<BoundExpression> expression,
        PrimitiveType target,
        text::TextSpan span,
        const std::string& context,
        std::string targetTypeName = {});

    [[nodiscard]] PrimitiveType bindType(
        const syntax::TypeSyntax& syntax,
        bool allowVoid,
        std::string* typeName = nullptr);
    [[nodiscard]] const VariableSymbol* lookupVariable(
        const std::string& name) const noexcept;
    bool declareVariable(VariableSymbol variable, text::TextSpan span);
    void pushScope(text::TextSpan span = {});
    void popScope();
    [[nodiscard]] std::unique_ptr<BoundErrorExpression> makeError(
        text::TextSpan span) const;

    diagnostics::DiagnosticBag& diagnostics_;
    std::vector<std::unordered_map<std::string, VariableSymbol>> scopes_;
    std::vector<text::TextSpan> scopeSpans_;
    PrimitiveType currentReturnType_ = PrimitiveType::Error;
    std::string currentReturnTypeName_;
    std::size_t nextVariableIndex_ = 0;
    FunctionOverloadMap visibleFunctions_;
    TypeSymbolMap visibleTypes_;
    std::optional<TypeSymbol> currentOwnerType_;
    bool currentStaticMethod_ = false;
    bool currentConstructor_ = false;
    std::string currentSourceName_;
    SymbolId currentFunctionId_ = 0;
    std::vector<VariableSymbol> allVariables_;
    std::vector<SymbolOccurrence> occurrences_;
    std::size_t loopDepth_ = 0;
    std::size_t breakableDepth_ = 0;
};

} // namespace realscript::semantic
