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
#include <unordered_set>
#include <utility>
#include <vector>

namespace realscript::semantic {

using SymbolId = std::uint64_t;

enum class PrimitiveType {
    Error,
    Void,
    Bool,
    Byte,
    SByte,
    Short,
    UShort,
    Int,
    UInt,
    Long,
    ULong,
    Float,
    Double,
    Char,
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

enum class Accessibility {
    Public,
    Internal,
    Protected,
    Private,
};

[[nodiscard]] const char* primitiveTypeName(PrimitiveType type) noexcept;
[[nodiscard]] PrimitiveType resolvePrimitiveType(const std::string& name) noexcept;
[[nodiscard]] bool isNumericType(PrimitiveType type) noexcept;
[[nodiscard]] bool isIntegralType(PrimitiveType type) noexcept;
[[nodiscard]] bool isUnsignedIntegralType(PrimitiveType type) noexcept;
[[nodiscard]] bool isFloatingPointType(PrimitiveType type) noexcept;
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
    Numeric,
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
    bool paramsArray = false;
    bool hasDefaultValue = false;
    syntax::TokenValue defaultValue;
    PrimitiveType defaultValueType = PrimitiveType::Error;
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
    Accessibility accessibility = Accessibility::Public;
    SymbolId declaringTypeId = 0;
    std::string declaringTypeName;
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
    Accessibility accessibility = Accessibility::Public;
    SymbolId declaringTypeId = 0;
    std::string declaringTypeName;
    bool virtualMethod = false;
    bool overrideMethod = false;
    bool abstractMethod = false;
    bool sealedMethod = false;
    std::uint32_t virtualSlot = std::numeric_limits<std::uint32_t>::max();
    bool interfaceMethod = false;
    std::uint32_t interfaceSlot = std::numeric_limits<std::uint32_t>::max();
    std::string moduleName;
    std::string name;
    std::string ownerTypeName;
    SymbolId ownerTypeId = 0;
    PrimitiveType returnType = PrimitiveType::Error;
    std::string returnTypeName;
    ParameterModifier returnModifier = ParameterModifier::None;
    PrimitiveType storageReturnType = PrimitiveType::Error;
    std::string storageReturnTypeName;
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

[[nodiscard]] inline PrimitiveType storageReturnTypeOf(
    const FunctionSymbol& function) noexcept {
    return function.storageReturnType == PrimitiveType::Error
        ? function.returnType
        : function.storageReturnType;
}

[[nodiscard]] inline const std::string& storageReturnTypeNameOf(
    const FunctionSymbol& function) noexcept {
    return function.storageReturnType == PrimitiveType::Error
        ? function.returnTypeName
        : function.storageReturnTypeName;
}

// Retained as an in-memory compatibility shape for pre-Phase-20 compiler
// inputs. New event declarations leave these collections empty and use the
// delegate-valued storageField below.
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
    Accessibility accessibility = Accessibility::Public;
    SymbolId declaringTypeId = 0;
    std::string declaringTypeName;
    std::string name;
    std::string delegateName;
    FieldSymbol storageField;
    std::vector<VariableSymbol> parameters;
    std::vector<EventHandlerSymbol> handlers;
    std::vector<EventSubscriptionSymbol> subscriptions;
    std::string sourceName;
    text::TextSpan declarationSpan;
};

struct PropertySymbol {
    std::string name;
    Accessibility accessibility = Accessibility::Public;
    SymbolId declaringTypeId = 0;
    std::string declaringTypeName;
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

struct InterfaceDispatchMap {
    SymbolId interfaceTypeId = 0;
    std::vector<SymbolId> slots;
};

[[nodiscard]] inline bool operator==(
    const InterfaceDispatchMap& left,
    const InterfaceDispatchMap& right) noexcept {
    return left.interfaceTypeId == right.interfaceTypeId &&
        left.slots == right.slots;
}

[[nodiscard]] inline bool operator!=(
    const InterfaceDispatchMap& left,
    const InterfaceDispatchMap& right) noexcept {
    return !(left == right);
}

struct TypeSymbol {
    SymbolId id = 0;
    TypeKind kind = TypeKind::Class;
    Accessibility accessibility = Accessibility::Public;
    bool synthetic = false;
    bool delegateType = false;
    bool interfaceType = false;
    bool abstractType = false;
    bool sealedType = false;
    SymbolId baseTypeId = 0;
    std::string baseTypeName;
    std::string moduleName;
    std::string name;
    std::vector<FieldSymbol> fields;
    std::vector<FunctionSymbol> methods;
    std::vector<SymbolId> virtualDispatchTable;
    std::vector<InterfaceDispatchMap> interfaceDispatchMaps;
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

[[nodiscard]] bool hasModifier(
    const std::vector<syntax::SyntaxToken>& modifiers,
    syntax::SyntaxKind kind) noexcept;
[[nodiscard]] Accessibility accessibilityFromModifiers(
    const std::vector<syntax::SyntaxToken>& modifiers,
    Accessibility fallback = Accessibility::Public) noexcept;
[[nodiscard]] bool isAssignable(
    const TypeSymbolMap& visibleTypes,
    const std::string& sourceTypeName,
    const std::string& targetTypeName) noexcept;
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
[[nodiscard]] FunctionSymbol declareFunctionSymbol(
    const std::string& moduleName,
    const syntax::InterfaceMethodDeclarationSyntax& syntax,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    const TypeSymbol* owner);
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
    const syntax::ConstructorDeclarationSyntax* constructorSyntax = nullptr;
    std::vector<std::string> parameterNames;
    std::vector<text::TextSpan> parameterSpans;
    bool syntheticAutoGetter = false;
    bool syntheticAutoSetter = false;
    FieldSymbol syntheticField;
    const syntax::SequenceDeclarationSyntax* sequence = nullptr;
    std::size_t sequenceSegment = 0;
    FieldSymbol sequenceTargetField;
    FieldSymbol sequenceTimerField;
    FieldSymbol sequenceStateField;
    FieldSymbol sequenceCompletedField;
    FieldSymbol sequenceResultField;
    std::vector<std::pair<std::string, FieldSymbol>> sequenceParameterFields;
    std::vector<std::pair<std::string, FieldSymbol>> sequenceLocalFields;
    struct SequenceForeachFields {
        const syntax::ForeachStatementSyntax* syntax = nullptr;
        FieldSymbol collection;
        FieldSymbol index;
        FieldSymbol iteration;
        bool usesEnumerator = false;
    };
    std::vector<SequenceForeachFields> sequenceForeachFields;
    bool sequenceSingleYieldLoop = false;
    bool sequenceSingleYieldBranch = false;
    bool sequenceStateMachine = false;
    bool sequenceCancellation = false;
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
    TypeBinaryExpression,
    TypeOfExpression,
    SwitchExpression,
    NullCoalescingExpression,
    ConditionalExpression,
    AssignmentExpression,
    ConversionExpression,
    DelegateCreationExpression,
    DelegateInvocationExpression,
    DelegateCombinationExpression,
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
    ThrowStatement,
    TryStatement,
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
    bool checkedArithmetic = true;
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
    bool checkedArithmetic = true;
    std::unique_ptr<BoundExpression> left;
    std::unique_ptr<BoundExpression> right;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::BinaryExpression;
    }
};

struct BoundTypeBinaryExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> expression;
    PrimitiveType targetType = PrimitiveType::Error;
    std::string targetTypeName;
    SymbolId targetTypeId = 0;
    bool safeCast = false;
    std::optional<VariableSymbol> patternVariable;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::TypeBinaryExpression;
    }
};

struct BoundTypeOfExpression final : BoundExpression {
    PrimitiveType queriedType = PrimitiveType::Error;
    std::string queriedTypeName;
    SymbolId queriedTypeId = 0;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::TypeOfExpression;
    }
};

struct BoundSwitchExpressionArm {
    std::unique_ptr<BoundExpression> label;
    PrimitiveType patternType = PrimitiveType::Error;
    std::string patternTypeName;
    SymbolId patternTypeId = 0;
    std::optional<VariableSymbol> patternVariable;
    std::unique_ptr<BoundExpression> guard;
    std::unique_ptr<BoundExpression> value;
    text::TextSpan span;
    bool discard = false;
};

struct BoundSwitchExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> expression;
    std::vector<BoundSwitchExpressionArm> arms;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::SwitchExpression;
    }
};

struct BoundNullCoalescingExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> left;
    std::unique_ptr<BoundExpression> right;
    bool nullableValue = false;
    TypeSymbol nullableType;
    FieldSymbol hasValueField;
    FieldSymbol valueField;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::NullCoalescingExpression;
    }
};

struct BoundConditionalExpression final : BoundExpression {
    std::unique_ptr<BoundExpression> condition;
    std::unique_ptr<BoundExpression> whenTrue;
    std::unique_ptr<BoundExpression> whenFalse;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ConditionalExpression;
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
    bool checkedArithmetic = true;
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ConversionExpression;
    }
};

struct BoundDelegateCreationExpression final : BoundExpression {
    TypeSymbol delegateType;
    FunctionSymbol function;
    std::unique_ptr<BoundExpression> receiver;
    std::optional<TypeSymbol> closureType;
    std::vector<FieldSymbol> captureFields;
    std::vector<std::unique_ptr<BoundExpression>> captures;
    bool virtualDispatch = false;
    std::uint32_t virtualSlot = std::numeric_limits<std::uint32_t>::max();
    bool interfaceDispatch = false;
    SymbolId interfaceTypeId = 0;
    std::uint32_t interfaceSlot = std::numeric_limits<std::uint32_t>::max();
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::DelegateCreationExpression;
    }
};

struct BoundDelegateInvocationExpression final : BoundExpression {
    struct Argument {
        ParameterModifier modifier = ParameterModifier::None;
        std::unique_ptr<BoundExpression> value;
        VariableSymbol variable;
        TypeSymbol wrapperType;
        FieldSymbol valueField;
        bool forwarded = false;
    };
    TypeSymbol delegateType;
    std::unique_ptr<BoundExpression> delegate;
    std::vector<Argument> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::DelegateInvocationExpression;
    }
};

struct BoundDelegateCombinationExpression final : BoundExpression {
    TypeSymbol delegateType;
    bool remove = false;
    std::unique_ptr<BoundExpression> left;
    std::unique_ptr<BoundExpression> right;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::DelegateCombinationExpression;
    }
};

struct BoundCallExpression final : BoundExpression {
    FunctionSymbol function;
    bool virtualDispatch = false;
    std::uint32_t virtualSlot = std::numeric_limits<std::uint32_t>::max();
    bool interfaceDispatch = false;
    SymbolId interfaceTypeId = 0;
    std::uint32_t interfaceSlot = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    std::vector<std::size_t> argumentEvaluationOrder;
    bool nullConditional = false;
    PrimitiveType nullConditionalValueType = PrimitiveType::Error;
    std::string nullConditionalValueTypeName;
    TypeSymbol nullConditionalNullableType;
    FieldSymbol nullConditionalHasValueField;
    FieldSymbol nullConditionalValueField;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::CallExpression;
    }
};

struct PreparedCallArguments {
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    std::vector<const syntax::ExpressionSyntax*> syntaxArguments;
    std::vector<std::optional<syntax::SyntaxToken>> modifiers;
    std::vector<std::size_t> evaluationOrder;
};


enum class ReferenceTargetKind {
    None,
    Variable,
    ObjectField,
    ArrayElement,
    StructField
};

struct BoundReferenceCallArgument {
    ParameterModifier modifier = ParameterModifier::None;
    std::unique_ptr<BoundExpression> value;
    VariableSymbol variable;
    TypeSymbol wrapperType;
    FieldSymbol valueField;
    bool forwarded = false;
    bool defensiveCopy = false;
    ReferenceTargetKind targetKind = ReferenceTargetKind::None;
    std::unique_ptr<BoundExpression> targetReceiver;
    std::unique_ptr<BoundExpression> targetIndex;
    TypeSymbol targetOwnerType;
    FieldSymbol targetField;
    PrimitiveType targetElementType = PrimitiveType::Error;
    std::string targetElementTypeName;
};

struct BoundReferenceCallExpression final : BoundExpression {
    FunctionSymbol function;
    bool virtualDispatch = false;
    std::uint32_t virtualSlot = std::numeric_limits<std::uint32_t>::max();
    bool interfaceDispatch = false;
    SymbolId interfaceTypeId = 0;
    std::uint32_t interfaceSlot = std::numeric_limits<std::uint32_t>::max();
    std::vector<BoundReferenceCallArgument> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ReferenceCallExpression;
    }
};

struct BoundEventInvocationExpression final : BoundExpression {
    TypeSymbol ownerType;
    EventSymbol event;
    TypeSymbol delegateType;
    std::unique_ptr<BoundExpression> receiver;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::EventInvocationExpression;
    }
};

struct BoundNewObjectExpression final : BoundExpression {
    struct Initializer {
        enum class Kind { Field, Property, Collection } kind = Kind::Field;
        FieldSymbol field;
        FunctionSymbol function;
        std::vector<std::unique_ptr<BoundExpression>> arguments;
    };
    TypeSymbol objectType;
    std::optional<FunctionSymbol> constructor;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    std::vector<std::size_t> argumentEvaluationOrder;
    std::vector<Initializer> initializers;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::NewObjectExpression;
    }
};

struct BoundNewStructExpression final : BoundExpression {
    using Initializer = BoundNewObjectExpression::Initializer;
    TypeSymbol structType;
    std::optional<FunctionSymbol> constructor;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    std::vector<std::size_t> argumentEvaluationOrder;
    std::vector<Initializer> initializers;
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
    bool wrappedVariable = false;
    TypeSymbol wrapperType;
    FieldSymbol wrapperValueField;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::StructFieldAssignmentExpression;
    }
};


struct BoundNewArrayExpression final : BoundExpression {
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    std::unique_ptr<BoundExpression> length;
    std::vector<std::unique_ptr<BoundExpression>> initialValues;
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
    bool nullConditional = false;
    PrimitiveType nullConditionalValueType = PrimitiveType::Error;
    std::string nullConditionalValueTypeName;
    TypeSymbol nullConditionalNullableType;
    FieldSymbol nullConditionalHasValueField;
    FieldSymbol nullConditionalValueField;
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
    bool usesEnumerator = false;
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
    PrimitiveType patternType = PrimitiveType::Error;
    std::string patternTypeName;
    SymbolId patternTypeId = 0;
    std::optional<VariableSymbol> patternVariable;
    std::unique_ptr<BoundExpression> guard;
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

struct BoundThrowStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ThrowStatement;
    }
};

struct BoundCatchClause {
    PrimitiveType type = PrimitiveType::Object;
    std::string typeName;
    SymbolId typeId = 0;
    VariableSymbol exceptionVariable;
    std::unique_ptr<BoundStatement> body;
    text::TextSpan span;
};

struct BoundTryStatement final : BoundStatement {
    std::unique_ptr<BoundStatement> body;
    std::vector<BoundCatchClause> catches;
    std::unique_ptr<BoundStatement> finallyBody;
    std::optional<VariableSymbol> finallyExceptionVariable;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::TryStatement;
    }
};

struct BoundEventSubscriptionStatement final : BoundStatement {
    TypeSymbol ownerType;
    std::unique_ptr<BoundExpression> receiver;
    EventSymbol event;
    TypeSymbol delegateType;
    std::unique_ptr<BoundExpression> handler;
    bool adding = true;
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
    [[nodiscard]] std::unique_ptr<BoundBlockStatement>
        bindSingleYieldLoopSequence(const FunctionBindingInput& input);
    [[nodiscard]] std::unique_ptr<BoundBlockStatement>
        bindSingleYieldBranchSequence(const FunctionBindingInput& input);
    [[nodiscard]] std::unique_ptr<BoundBlockStatement>
        bindStateMachineSequence(const FunctionBindingInput& input);
    [[nodiscard]] std::unique_ptr<BoundBlockStatement>
        bindSequenceCancellation(const FunctionBindingInput& input);
    [[nodiscard]] std::unique_ptr<BoundExpression> makeSequenceFieldAccess(
        const FieldSymbol& field, text::TextSpan span);
    [[nodiscard]] std::unique_ptr<BoundExpression> makeVariableAccess(
        const VariableSymbol& variable, text::TextSpan span);
    [[nodiscard]] const FunctionSymbol* findScheduleFunction() const noexcept;
    void appendSequenceRestartCancellation(
        BoundBlockStatement& result,
        const FunctionBindingInput& input);
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
    [[nodiscard]] std::unique_ptr<BoundStatement> bindThrowStatement(
        const syntax::ThrowStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindTryStatement(
        const syntax::TryStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindEventSubscriptionStatement(
        const syntax::EventSubscriptionStatementSyntax& syntax);
    [[nodiscard]] TypeSymbol ensureCaptureStorage(
        const VariableSymbol& variable);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindVariableDeclaration(
        const syntax::VariableDeclarationStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindExpressionStatement(
        const syntax::ExpressionStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindExpression(
        const syntax::ExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindTargetExpression(
        const syntax::ExpressionSyntax& syntax,
        PrimitiveType target,
        const std::string& targetTypeName,
        const std::string& context);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindDelegateCreation(
        const syntax::ExpressionSyntax& syntax,
        const TypeSymbol& delegateType,
        const std::string& context);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindLiteralExpression(
        const syntax::LiteralExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindNameExpression(
        const syntax::NameExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindThisExpression(
        const syntax::ThisExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindBaseExpression(
        const syntax::BaseExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindUnaryExpression(
        const syntax::UnaryExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindBinaryExpression(
        const syntax::BinaryExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindTypeBinaryExpression(
        const syntax::TypeBinaryExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindTypeOfExpression(
        const syntax::TypeOfExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindSwitchExpression(
        const syntax::SwitchExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindConditionalExpression(
        const syntax::ConditionalExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindAssignmentExpression(
        const syntax::AssignmentExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindCallExpression(
        const syntax::CallExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindCastExpression(
        const syntax::CastExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindSelectedCall(
        const FunctionSymbol& function,
        std::vector<std::unique_ptr<BoundExpression>> arguments,
        const std::vector<const syntax::ExpressionSyntax*>& syntaxArguments,
        const std::vector<std::optional<syntax::SyntaxToken>>& argumentModifiers,
        std::vector<std::size_t> argumentEvaluationOrder,
        std::unique_ptr<BoundExpression> receiver,
        text::TextSpan span,
        const std::string& context,
        bool forceStaticDispatch = false);
    [[nodiscard]] PreparedCallArguments prepareCallArguments(
        const FunctionSymbol& function,
        const std::vector<std::vector<std::size_t>>& sources,
        std::vector<std::unique_ptr<BoundExpression>> arguments,
        const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& syntaxArguments,
        const std::vector<std::optional<syntax::SyntaxToken>>& modifiers,
        text::TextSpan callSpan,
        const std::string& context);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindMemberCallExpression(
        const syntax::MemberCallExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindNewObjectExpression(
        const syntax::NewObjectExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement>
        bindBaseConstructorInitializer(const FunctionBindingInput& input);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindNewArrayExpression(
        const syntax::NewArrayExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindElementAccessExpression(
        const syntax::ElementAccessExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindMemberAccessExpression(
        const syntax::MemberAccessExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> applyNullConditional(
        std::unique_ptr<BoundExpression> expression,
        const syntax::SyntaxToken& accessToken,
        text::TextSpan span);
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
    [[nodiscard]] const TypeSymbol* findVisibleType(
        SymbolId id) const noexcept;
    [[nodiscard]] bool isTypeAccessible(
        const TypeSymbol& type) const noexcept;
    [[nodiscard]] bool isMemberAccessible(
        Accessibility accessibility,
        SymbolId declaringTypeId,
        const std::string& declaringModule = {}) const noexcept;
    bool declareVariable(VariableSymbol variable, text::TextSpan span);
    void pushScope(text::TextSpan span = {});
    void popScope();
    [[nodiscard]] std::unique_ptr<BoundErrorExpression> makeError(
        text::TextSpan span) const;

    diagnostics::DiagnosticBag& diagnostics_;
    std::vector<std::unordered_map<std::string, VariableSymbol>> scopes_;
    std::vector<std::unordered_map<std::string, VariableSymbol>>
        referenceAliasScopes_;
    std::vector<text::TextSpan> scopeSpans_;
    PrimitiveType currentReturnType_ = PrimitiveType::Error;
    std::string currentReturnTypeName_;
    ParameterModifier currentReturnModifier_ = ParameterModifier::None;
    PrimitiveType currentStorageReturnType_ = PrimitiveType::Error;
    std::string currentStorageReturnTypeName_;
    std::size_t nextVariableIndex_ = 0;
    std::optional<VariableSymbol> currentExceptionVariable_;
    bool bindingFinally_ = false;
    std::size_t finallyBreakableDepth_ = 0;
    std::size_t finallyLoopDepth_ = 0;
    FunctionOverloadMap visibleFunctions_;
    TypeSymbolMap visibleTypes_;
    std::optional<TypeSymbol> currentOwnerType_;
    bool currentStaticMethod_ = false;
    bool currentConstructor_ = false;
    std::string currentModuleName_;
    std::string currentSourceName_;
    SymbolId currentFunctionId_ = 0;
    std::vector<VariableSymbol> allVariables_;
    std::vector<SymbolOccurrence> occurrences_;
    std::vector<FunctionBindingInput> pendingFunctionBindings_;
    std::vector<TypeSymbol> pendingTypes_;
    std::size_t nextLambdaOrdinal_ = 0;
    std::unordered_set<std::string> capturedVariableNames_;
    std::unordered_map<std::string, FieldSymbol> sequenceLocalFields_;
    std::size_t loopDepth_ = 0;
    std::size_t breakableDepth_ = 0;
    bool checkedArithmetic_ = true;
};

} // namespace realscript::semantic
