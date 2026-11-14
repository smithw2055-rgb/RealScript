#include "SemanticInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <unordered_set>
#include <utility>

namespace realscript::semantic {

using namespace detail;

const TypeSymbol* Binder::findVisibleType(SymbolId id) const noexcept {
    for (const auto& [name, type] : visibleTypes_) {
        (void)name;
        if (type.id == id) return &type;
    }
    return nullptr;
}

bool Binder::isTypeAccessible(const TypeSymbol& type) const noexcept {
    if (type.accessibility == Accessibility::Public) return true;
    if (type.accessibility == Accessibility::Internal &&
        hygienicAccessibleModules_.find(type.moduleName) != hygienicAccessibleModules_.end()) {
        return true;
    }
    return type.moduleName == currentModuleName_;
}

bool Binder::isMemberAccessible(
    Accessibility accessibility,
    SymbolId declaringTypeId,
    const std::string& declaringModule) const noexcept {
    if (accessibility == Accessibility::Public) return true;
    const auto* declaringType = declaringTypeId != 0
        ? findVisibleType(declaringTypeId)
        : nullptr;
    const auto& module = declaringType
        ? declaringType->moduleName
        : declaringModule;
    if (accessibility == Accessibility::Internal) {
        return module == currentModuleName_ ||
            hygienicAccessibleModules_.find(module) != hygienicAccessibleModules_.end();
    }
    if (!currentOwnerType_ || declaringTypeId == 0) {
        return false;
    }
    if (currentOwnerType_->id == declaringTypeId) return true;
    if (accessibility == Accessibility::Private) return false;

    auto baseTypeId = currentOwnerType_->baseTypeId;
    while (baseTypeId != 0) {
        if (baseTypeId == declaringTypeId) return true;
        const auto* baseType = findVisibleType(baseTypeId);
        if (!baseType || baseType->baseTypeId == baseTypeId) break;
        baseTypeId = baseType->baseTypeId;
    }
    return false;
}

bool Binder::isManagedResourceHandleType(
    PrimitiveType type,
    const std::string& typeName) const noexcept {
    if (type != PrimitiveType::Struct || typeName.empty()) return false;
    const auto found = visibleTypes_.find(typeName);
    if (found == visibleTypes_.end() || found->second.kind != TypeKind::Struct) {
        return false;
    }
    const auto sep = typeName.rfind("::");
    const std::string_view shortName = sep == std::string::npos
        ? std::string_view(typeName)
        : std::string_view(typeName).substr(sep + 2);
    const bool lifecycleMarked =
        found->second.hasValueLifecycle ||
        shortName == "ResourceHandle" ||
        shortName.rfind("ResourceHandle__", 0) == 0;
    if (!lifecycleMarked) {
        return false;
    }
    for (const auto& m : found->second.methods) {
        if (!m.staticMethod && m.name == "Retain" &&
            m.parameters.size() == 1 && m.returnType == PrimitiveType::Struct) {
            return true;
        }
    }
    return false;
}

bool Binder::isInsideResourceHandleImplementation() const noexcept {
    if (!currentOwnerType_) return false;
    const auto ownerCanon = canonicalTypeName(*currentOwnerType_);
    if (isManagedResourceHandleType(PrimitiveType::Struct, ownerCanon)) {
        return true;
    }
    if (currentModuleName_ == "RealScript.Resource" &&
        (currentOwnerType_->name == "Resource" ||
         currentOwnerType_->name == "ResourceServer")) {
        return true;
    }
    return false;
}

bool Binder::shouldCloneResourceHandleValue(
    const BoundExpression& expr,
    bool isReturnContext) const noexcept {
    if (isInsideResourceHandleImplementation()) return false;
    if (!isManagedResourceHandleType(expr.type, expr.typeName)) return false;
    switch (expr.kind()) {
    case BoundNodeKind::VariableExpression: {
        if (!isReturnContext) return true;
        const auto& v = static_cast<const BoundVariableExpression&>(expr).variable;
        return v.isUsingResource || v.parameter || storageTypeOf(v) != v.type || storageTypeNameOf(v) != v.typeName;
    }
    case BoundNodeKind::MemberAccessExpression:
    case BoundNodeKind::StructFieldAccessExpression:
    case BoundNodeKind::ElementAccessExpression:
    case BoundNodeKind::AssignmentExpression:
    case BoundNodeKind::MemberAssignmentExpression:
    case BoundNodeKind::StructFieldAssignmentExpression:
    case BoundNodeKind::PropertyAssignmentExpression:
    case BoundNodeKind::ElementAssignmentExpression:
        return true;
    default:
        return false;
    }
}

std::unique_ptr<BoundExpression> Binder::cloneResourceHandleIfNeeded(
    std::unique_ptr<BoundExpression> expr,
    text::TextSpan span,
    bool isReturnContext) {
    if (!expr || !shouldCloneResourceHandleValue(*expr, isReturnContext)) {
        return expr;
    }
    const auto ownerIt = visibleTypes_.find(expr->typeName);
    if (ownerIt == visibleTypes_.end()) return expr;
    const FunctionSymbol* retainMethod = nullptr;
    for (const auto& m : ownerIt->second.methods) {
        if (!m.staticMethod && m.name == "Retain" &&
            m.parameters.size() == 1 && m.returnType == PrimitiveType::Struct) {
            retainMethod = &m;
            break;
        }
    }
    if (!retainMethod) return expr;
    std::vector<const syntax::ExpressionSyntax*> emptySyntaxArgs;
    std::vector<std::optional<syntax::SyntaxToken>> emptyMods;
    return bindSelectedCall(
        *retainMethod,
        {},
        emptySyntaxArgs,
        emptyMods,
        {},
        std::move(expr),
        span,
        "resource handle clone",
        true);
}

std::unique_ptr<BoundExpression> Binder::wrapResourceHandleOverwriteRelease(
    std::unique_ptr<BoundExpression> oldValueExpr,
    std::unique_ptr<BoundExpression> newValueExpr,
    text::TextSpan span) {
    if (!oldValueExpr || !newValueExpr ||
        isInsideResourceHandleImplementation() ||
        !isManagedResourceHandleType(newValueExpr->type, newValueExpr->typeName)) {
        return newValueExpr;
    }
    const auto ownerIt = visibleTypes_.find(newValueExpr->typeName);
    if (ownerIt == visibleTypes_.end()) return newValueExpr;
    const FunctionSymbol* assignHelper = nullptr;
    for (const auto& m : ownerIt->second.methods) {
        if (m.staticMethod && m.name == "__AssignHandle" &&
            m.parameters.size() == 2 && m.returnType == PrimitiveType::Struct) {
            assignHelper = &m;
            break;
        }
    }
    if (!assignHelper) return newValueExpr;
    auto markSyntheticRead = [](BoundExpression* expr) {
        while (expr) {
            if (expr->kind() == BoundNodeKind::VariableExpression) {
                static_cast<BoundVariableExpression*>(expr)->syntheticOverwriteRead = true;
                return;
            }
            if (expr->kind() == BoundNodeKind::MemberAccessExpression) {
                expr = static_cast<BoundMemberAccessExpression*>(expr)->receiver.get();
                continue;
            }
            break;
        }
    };
    markSyntheticRead(oldValueExpr.get());
    auto call = std::make_unique<BoundCallExpression>();
    call->span = span;
    call->type = newValueExpr->type;
    call->typeName = newValueExpr->typeName;
    call->function = *assignHelper;
    call->arguments.push_back(std::move(oldValueExpr));
    call->arguments.push_back(std::move(newValueExpr));
    call->argumentEvaluationOrder = {1, 0};
    return call;
}


std::unique_ptr<BoundExpression> Binder::bindNameExpression(
    const syntax::NameExpressionSyntax& syntaxTree) {
    if (const auto* variable = lookupVariable(
            syntaxTree.identifierToken.text)) {
        if (variable->name != "this") {
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                variable->id,
                variable->parameter ? SymbolKind::Parameter : SymbolKind::Local,
                variable->name,
                isExactType(variable->type) && !variable->typeName.empty()
                    ? variable->typeName
                    : primitiveTypeName(variable->type),
                syntaxTree.identifierToken.span);
        }
        if (storageTypeOf(*variable) != variable->type || storageTypeNameOf(*variable) != variable->typeName) {
            const auto wrapper = visibleTypes_.find(
                storageTypeNameOf(*variable));
            if (wrapper == visibleTypes_.end() ||
                wrapper->second.fields.empty()) {
                diagnostics_.report(
                    "RS8705",
                    "reference parameter storage descriptor is unavailable",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            auto result =
                std::make_unique<BoundMemberAccessExpression>();
            result->span = syntaxTree.span();
            result->type = variable->type;
            result->typeName = variable->typeName;
            result->receiver = storageVariableExpression(
                *variable, syntaxTree.span());
            result->ownerType = wrapper->second;
            result->field = wrapper->second.fields.front();
            return result;
        }
        return variableExpression(*variable, syntaxTree.span());
    }

    if (const auto field = sequenceLocalFields_.find(
            syntaxTree.identifierToken.text);
        field != sequenceLocalFields_.end()) {
        return makeSequenceFieldAccess(field->second, syntaxTree.span());
    }

    if (currentOwnerType_) {
        const auto* thisVariable = lookupVariable("this");
        if (const auto* field = findField(*currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    field->accessibility, field->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534", "field '" + field->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            checkObsoleteReference(
                field->id, "field", field->name, syntaxTree.identifierToken.span);
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                field->id,
                SymbolKind::Field,
                field->name,
                isExactType(field->type) && !field->typeName.empty()
                    ? field->typeName
                    : primitiveTypeName(field->type),
                syntaxTree.identifierToken.span);
            if (field->constField && field->hasConstantValue) {
                auto lit = std::make_unique<BoundLiteralExpression>();
                lit->span = syntaxTree.span();
                lit->type = field->type;
                lit->typeName = field->typeName;
                lit->value = field->constantValue;
                return lit;
            }
            if (field->staticField) {
                auto result = std::make_unique<BoundMemberAccessExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                if (!currentOwnerType_->staticConstructor.has_value() &&
                    !field->hasConstantValue && field->initializerSyntax) {
                    result->staticInitializer = bindStaticFieldInitializer(
                        *currentOwnerType_, *field);
                }
                return result;
            }
            if (currentStaticMethod_ || !thisVariable) {
                diagnostics_.report(
                    "RS2484",
                    "instance field '" + field->name + "' requires a value receiver",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            const auto captureStorage = currentOwnerType_->synthetic &&
                    field->type == PrimitiveType::Object
                ? visibleTypes_.find(field->typeName)
                : visibleTypes_.end();
            if (captureStorage != visibleTypes_.end() &&
                captureStorage->second.synthetic &&
                captureStorage->second.fields.size() == 1 &&
                captureStorage->second.fields.front().name == "Value") {
                auto cell =
                    std::make_unique<BoundMemberAccessExpression>();
                cell->span = syntaxTree.span();
                cell->type = field->type;
                cell->typeName = field->typeName;
                cell->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                cell->ownerType = *currentOwnerType_;
                cell->field = *field;
                const auto& valueField =
                    captureStorage->second.fields.front();
                auto result =
                    std::make_unique<BoundMemberAccessExpression>();
                result->span = syntaxTree.span();
                result->type = valueField.type;
                result->typeName = valueField.typeName;
                result->receiver = std::move(cell);
                result->ownerType = captureStorage->second;
                result->field = valueField;
                return result;
            }
            if (currentOwnerType_->kind == TypeKind::Struct) {
                auto result = std::make_unique<BoundStructFieldAccessExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->receiver = makeVariableAccess(
                    *thisVariable, syntaxTree.span());
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                return result;
            }
            auto result = std::make_unique<BoundMemberAccessExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->receiver = variableExpression(*thisVariable, syntaxTree.span());
            result->ownerType = *currentOwnerType_;
            result->field = *field;
            return result;
        }
        if (!currentStaticMethod_ && thisVariable && currentOwnerType_->synthetic) {
            const auto* capturedThis = findField(*currentOwnerType_, "this");
            const auto outer = capturedThis
                ? visibleTypes_.find(capturedThis->typeName)
                : visibleTypes_.end();
            const auto* outerField = outer != visibleTypes_.end()
                ? findField(outer->second, syntaxTree.identifierToken.text)
                : nullptr;
            if (capturedThis && outerField) {
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    outerField->id,
                    SymbolKind::Field,
                    outerField->name,
                    isExactType(outerField->type) && !outerField->typeName.empty()
                        ? outerField->typeName
                        : primitiveTypeName(outerField->type),
                    syntaxTree.identifierToken.span);
                auto capturedReceiver =
                    std::make_unique<BoundMemberAccessExpression>();
                capturedReceiver->span = syntaxTree.span();
                capturedReceiver->type = capturedThis->type;
                capturedReceiver->typeName = capturedThis->typeName;
                capturedReceiver->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                capturedReceiver->ownerType = *currentOwnerType_;
                capturedReceiver->field = *capturedThis;
                auto result =
                    std::make_unique<BoundMemberAccessExpression>();
                result->span = syntaxTree.span();
                result->type = outerField->type;
                result->typeName = outerField->typeName;
                result->receiver = std::move(capturedReceiver);
                result->ownerType = outer->second;
                result->field = *outerField;
                return result;
            }
        }
        if (const auto* property = findProperty(*currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    property->accessibility,
                    property->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534",
                    "property '" + property->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            checkObsoleteReference(
                property->id, "property", property->name, syntaxTree.identifierToken.span);
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                property->id,
                SymbolKind::Property,
                property->name,
                isExactType(property->type) && !property->typeName.empty()
                    ? property->typeName
                    : primitiveTypeName(property->type),
                syntaxTree.identifierToken.span);
            if (property->staticProperty) {
                if (property->getter) {
                    auto result = std::make_unique<BoundCallExpression>();
                    result->span = syntaxTree.span();
                    result->type = property->type;
                    result->typeName = property->typeName;
                    result->function = *property->getter;
                    result->staticConstructor = findStaticConstructorForFunction(
                        visibleTypes_, *property->getter);
                    if (!result->staticConstructor) {
                        result->staticConstructor = currentOwnerType_->staticConstructor;
                    }
                    return result;
                }
            } else if (!currentStaticMethod_ && thisVariable && property->getter) {
                auto result = std::make_unique<BoundCallExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->function = *property->getter;
                result->arguments.push_back(makeVariableAccess(*thisVariable, syntaxTree.span()));
                return result;
            } else if (!property->staticProperty && (currentStaticMethod_ || !thisVariable)) {
                diagnostics_.report(
                    "RS2484",
                    "instance property '" + property->name + "' requires a value receiver",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
        }
    }

    diagnostics_.report(
        "RS2102",
        "undefined name '" + syntaxTree.identifierToken.text + "'",
        syntaxTree.identifierToken.span);
    return makeError(syntaxTree.span());
}


std::unique_ptr<BoundExpression> Binder::bindAssignmentExpression(
    const syntax::AssignmentExpressionSyntax& syntaxTree) {
    const auto compoundOp = compoundBinaryOperatorToken(syntaxTree.equalsToken);
    const auto bindAssignedValue = [&](
        PrimitiveType targetType,
        const std::string& targetTypeName,
        const std::string& context) -> std::unique_ptr<BoundExpression> {
        if (!compoundOp) {
            return bindTargetExpression(
                *syntaxTree.expression, targetType, targetTypeName, context);
        }
        syntax::NameExpressionSyntax lhsSyntax;
        lhsSyntax.identifierToken = syntaxTree.identifierToken;
        auto lhsValue = bindNameExpression(lhsSyntax);
        auto rhsValue = bindExpression(*syntaxTree.expression);
        auto combined = bindBinaryOperation(
            std::move(lhsValue),
            syntaxTree.identifierToken.span,
            *compoundOp,
            std::move(rhsValue),
            syntaxTree.expression->span(),
            syntaxTree.span());
        return convertExpression(
            std::move(combined), targetType, syntaxTree.span(),
            context, targetTypeName);
    };

    if (const auto* foundVariable = lookupVariable(
            syntaxTree.identifierToken.text)) {
        const VariableSymbol variable = *foundVariable;
        if (variable.name != "this" && !compoundOp) {
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                variable.id,
                variable.parameter ? SymbolKind::Parameter : SymbolKind::Local,
                variable.name,
                isExactType(variable.type) && !variable.typeName.empty()
                    ? variable.typeName
                    : primitiveTypeName(variable.type),
                syntaxTree.identifierToken.span);
        }
        if (variable.isConst) {
            diagnostics_.report(
                "RS2483",
                "cannot assign to const local variable '" +
                    variable.name + "'",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        if (variable.modifier == ParameterModifier::In) {
            diagnostics_.report(
                "RS8702",
                "cannot assign to in parameter '" +
                    variable.name + "'",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        if (variable.isUsingResource) {
            diagnostics_.report(
                "RS2651",
                "cannot assign to '" + variable.name +
                    "' because it is a 'using' variable",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        auto value = bindAssignedValue(
            variable.type, variable.typeName, "assignment");
        if (storageTypeOf(variable) != variable.type || storageTypeNameOf(variable) != variable.typeName) {
            const auto wrapper = visibleTypes_.find(
                storageTypeNameOf(variable));
            if (wrapper == visibleTypes_.end() ||
                wrapper->second.fields.empty()) {
                diagnostics_.report(
                    "RS8705",
                    "reference parameter storage descriptor is unavailable",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            auto result =
                std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = variable.type;
            result->typeName = variable.typeName;
            result->receiver = storageVariableExpression(
                variable, syntaxTree.span());
            result->ownerType = wrapper->second;
            result->field = wrapper->second.fields.front();
            result->expression = wrapResourceHandleOverwriteRelease(
                makeVariableAccess(variable, syntaxTree.identifierToken.span),
                std::move(value),
                syntaxTree.span());
            return result;
        }
        auto result = std::make_unique<BoundAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = variable.type;
        result->typeName = variable.typeName;
        result->variable = variable;
        result->expression = wrapResourceHandleOverwriteRelease(
            makeVariableAccess(variable, syntaxTree.identifierToken.span),
            std::move(value),
            syntaxTree.span());
        return result;
    }

    if (const auto field = sequenceLocalFields_.find(
            syntaxTree.identifierToken.text);
        field != sequenceLocalFields_.end()) {
        if (field->second.isUsingResource) {
            diagnostics_.report(
                "RS2651",
                "cannot assign to '" + field->first +
                    "' because it is a 'using' variable",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        const auto* foundReceiver = lookupVariable("this");
        if (!foundReceiver || !currentOwnerType_) return makeError(syntaxTree.span());
        const VariableSymbol receiver = *foundReceiver;
        auto result = std::make_unique<BoundMemberAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = field->second.type;
        result->typeName = field->second.typeName;
        result->receiver = variableExpression(receiver, syntaxTree.span());
        result->ownerType = *currentOwnerType_;
        result->field = field->second;
        auto seqVal = bindAssignedValue(
            field->second.type,
            field->second.typeName,
            "sequence local assignment");
        result->expression = wrapResourceHandleOverwriteRelease(
            makeSequenceFieldAccess(field->second, syntaxTree.identifierToken.span),
            std::move(seqVal),
            syntaxTree.span());
        return result;
    }

    if (currentOwnerType_) {
        std::optional<VariableSymbol> thisVariable;
        if (const auto* foundThis = lookupVariable("this")) {
            thisVariable = *foundThis;
        }
        if (const auto* field = findField(
                *currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    field->accessibility, field->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534", "field '" + field->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            if (!compoundOp) {
                checkObsoleteReference(
                    field->id, "field", field->name, syntaxTree.identifierToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    field->id,
                    SymbolKind::Field,
                    field->name,
                    isExactType(field->type) && !field->typeName.empty()
                        ? field->typeName
                        : primitiveTypeName(field->type),
                    syntaxTree.identifierToken.span);
            }
            if (field->constField) {
                diagnostics_.report(
                    "RS2483",
                    "cannot assign to const field '" + field->name + "'",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (field->readonlyField &&
                !isReadonlyFieldWritable(
                    *field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                diagnostics_.report(
                    "RS2483",
                    "cannot assign to readonly field '" + field->name + "'",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (field->staticField) {
                auto value = bindAssignedValue(
                    field->type, field->typeName, "field assignment");
                auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
                oldAccess->span = syntaxTree.identifierToken.span;
                oldAccess->type = field->type;
                oldAccess->typeName = field->typeName;
                oldAccess->ownerType = *currentOwnerType_;
                oldAccess->field = *field;
                auto result = std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
                return result;
            }
            if (currentStaticMethod_ || !thisVariable) {
                diagnostics_.report(
                    "RS2484",
                    "instance field '" + field->name + "' requires a value receiver",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            const auto captureStorage = currentOwnerType_->synthetic &&
                    field->type == PrimitiveType::Object
                ? visibleTypes_.find(field->typeName)
                : visibleTypes_.end();
            if (captureStorage != visibleTypes_.end() &&
                captureStorage->second.synthetic &&
                captureStorage->second.fields.size() == 1 &&
                captureStorage->second.fields.front().name == "Value") {
                const auto& valueField =
                    captureStorage->second.fields.front();
                auto value = bindAssignedValue(
                    valueField.type,
                    valueField.typeName, "captured variable assignment");
                auto cell =
                    std::make_unique<BoundMemberAccessExpression>();
                cell->span = syntaxTree.span();
                cell->type = field->type;
                cell->typeName = field->typeName;
                cell->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                cell->ownerType = *currentOwnerType_;
                cell->field = *field;
                auto oldCell =
                    std::make_unique<BoundMemberAccessExpression>();
                oldCell->span = syntaxTree.span();
                oldCell->type = field->type;
                oldCell->typeName = field->typeName;
                oldCell->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                oldCell->ownerType = *currentOwnerType_;
                oldCell->field = *field;
                auto oldAccess =
                    std::make_unique<BoundMemberAccessExpression>();
                oldAccess->span = syntaxTree.span();
                oldAccess->type = valueField.type;
                oldAccess->typeName = valueField.typeName;
                oldAccess->receiver = std::move(oldCell);
                oldAccess->ownerType = captureStorage->second;
                oldAccess->field = valueField;
                auto result =
                    std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = valueField.type;
                result->typeName = valueField.typeName;
                result->receiver = std::move(cell);
                result->ownerType = captureStorage->second;
                result->field = valueField;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
                return result;
            }
            auto value = bindAssignedValue(
                field->type,
                field->typeName, "field assignment");
            if (currentOwnerType_->kind == TypeKind::Struct) {
                auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                oldAccess->span = syntaxTree.identifierToken.span;
                oldAccess->type = field->type;
                oldAccess->typeName = field->typeName;
                oldAccess->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                oldAccess->ownerType = *currentOwnerType_;
                oldAccess->field = *field;
                auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->variable = *thisVariable;
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
                if (storageTypeOf(*thisVariable) != thisVariable->type || storageTypeNameOf(*thisVariable) != thisVariable->typeName) {
                    const auto wrapper = visibleTypes_.find(
                        storageTypeNameOf(*thisVariable));
                    if (wrapper == visibleTypes_.end() ||
                        wrapper->second.fields.empty()) {
                        diagnostics_.report(
                            "RS8705",
                            "mutable struct receiver storage descriptor is unavailable",
                            syntaxTree.span());
                        return makeError(syntaxTree.span());
                    }
                    result->wrappedVariable = true;
                    result->wrapperType = wrapper->second;
                    result->wrapperValueField =
                        wrapper->second.fields.front();
                }
                return result;
            }
            auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
            oldAccess->span = syntaxTree.identifierToken.span;
            oldAccess->type = field->type;
            oldAccess->typeName = field->typeName;
            oldAccess->receiver = variableExpression(*thisVariable, syntaxTree.span());
            oldAccess->ownerType = *currentOwnerType_;
            oldAccess->field = *field;
            auto receiver = variableExpression(*thisVariable, syntaxTree.span());
            auto result = std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = *currentOwnerType_;
            result->field = *field;
            result->expression = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(value), syntaxTree.span());
            return result;
        }
        if (!currentStaticMethod_ && thisVariable && currentOwnerType_->synthetic) {
            const auto* capturedThis = findField(*currentOwnerType_, "this");
            const auto outer = capturedThis
                ? visibleTypes_.find(capturedThis->typeName)
                : visibleTypes_.end();
            const auto* outerField = outer != visibleTypes_.end()
                ? findField(outer->second, syntaxTree.identifierToken.text)
                : nullptr;
            if (capturedThis && outerField) {
                if (!compoundOp) {
                    checkObsoleteReference(
                        outerField->id, "field", outerField->name, syntaxTree.identifierToken.span);
                    recordSymbolReference(
                        occurrences_,
                        currentSourceName_,
                        outerField->id,
                        SymbolKind::Field,
                        outerField->name,
                        isExactType(outerField->type) && !outerField->typeName.empty()
                            ? outerField->typeName
                            : primitiveTypeName(outerField->type),
                        syntaxTree.identifierToken.span);
                }
                auto value = bindAssignedValue(
                    outerField->type,
                    outerField->typeName, "captured field assignment");
                auto oldCapRecv =
                    std::make_unique<BoundMemberAccessExpression>();
                oldCapRecv->span = syntaxTree.span();
                oldCapRecv->type = capturedThis->type;
                oldCapRecv->typeName = capturedThis->typeName;
                oldCapRecv->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                oldCapRecv->ownerType = *currentOwnerType_;
                oldCapRecv->field = *capturedThis;
                auto oldAccess =
                    std::make_unique<BoundMemberAccessExpression>();
                oldAccess->span = syntaxTree.span();
                oldAccess->type = outerField->type;
                oldAccess->typeName = outerField->typeName;
                oldAccess->receiver = std::move(oldCapRecv);
                oldAccess->ownerType = outer->second;
                oldAccess->field = *outerField;
                auto capturedReceiver =
                    std::make_unique<BoundMemberAccessExpression>();
                capturedReceiver->span = syntaxTree.span();
                capturedReceiver->type = capturedThis->type;
                capturedReceiver->typeName = capturedThis->typeName;
                capturedReceiver->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                capturedReceiver->ownerType = *currentOwnerType_;
                capturedReceiver->field = *capturedThis;
                auto result =
                    std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = outerField->type;
                result->typeName = outerField->typeName;
                result->receiver = std::move(capturedReceiver);
                result->ownerType = outer->second;
                result->field = *outerField;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
                return result;
            }
        }
        if (const auto* property = findProperty(
                *currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    property->accessibility,
                    property->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534",
                    "property '" + property->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            if (!compoundOp) {
                checkObsoleteReference(
                    property->id, "property", property->name, syntaxTree.identifierToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    property->id,
                    SymbolKind::Property,
                    property->name,
                    isExactType(property->type) && !property->typeName.empty()
                        ? property->typeName
                        : primitiveTypeName(property->type),
                    syntaxTree.identifierToken.span);
            }
            if (!property->staticProperty && (currentStaticMethod_ || !thisVariable)) {
                diagnostics_.report(
                    "RS2484",
                    "instance property '" + property->name + "' requires a value receiver",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (!property->setter) {
                if (property->backingFieldIndex < currentOwnerType_->fields.size()) {
                    const auto& backingField = currentOwnerType_->fields[property->backingFieldIndex];
                    if (isReadonlyFieldWritable(
                            backingField,
                            currentConstructor_,
                            currentStaticMethod_,
                            currentOwnerType_)) {
                        auto assigned = bindAssignedValue(
                            property->type,
                            property->typeName,
                            "property assignment");
                        if (!property->staticProperty &&
                            currentOwnerType_->kind == TypeKind::Struct) {
                            auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                            oldAccess->span = syntaxTree.identifierToken.span;
                            oldAccess->type = backingField.type;
                            oldAccess->typeName = backingField.typeName;
                            oldAccess->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                            oldAccess->ownerType = *currentOwnerType_;
                            oldAccess->field = backingField;
                            auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                            result->span = syntaxTree.span();
                            result->type = property->type;
                            result->typeName = property->typeName;
                            result->variable = *thisVariable;
                            result->ownerType = *currentOwnerType_;
                            result->field = backingField;
                            result->expression = wrapResourceHandleOverwriteRelease(
                                std::move(oldAccess), std::move(assigned), syntaxTree.span());
                            return result;
                        }
                        auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
                        oldAccess->span = syntaxTree.identifierToken.span;
                        oldAccess->type = backingField.type;
                        oldAccess->typeName = backingField.typeName;
                        if (!property->staticProperty && thisVariable) {
                            oldAccess->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                        }
                        oldAccess->ownerType = *currentOwnerType_;
                        oldAccess->field = backingField;
                        auto result = std::make_unique<BoundMemberAssignmentExpression>();
                        result->span = syntaxTree.span();
                        result->type = property->type;
                        result->typeName = property->typeName;
                        if (!property->staticProperty && thisVariable) {
                            result->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                        }
                        result->ownerType = *currentOwnerType_;
                        result->field = backingField;
                        result->expression = wrapResourceHandleOverwriteRelease(
                            std::move(oldAccess), std::move(assigned), syntaxTree.span());
                        return result;
                    }
                }
                diagnostics_.report(
                    "RS2483",
                    "property '" + property->name + "' is read-only",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (!property->staticProperty && currentOwnerType_->kind == TypeKind::Struct) {
                if (!currentConstructor_ ||
                    property->backingFieldIndex >= currentOwnerType_->fields.size()) {
                    diagnostics_.report(
                        "RS2486",
                        "struct properties cannot be assigned through an instance method in the Phase 3E value model",
                        syntaxTree.identifierToken.span);
                    return makeError(syntaxTree.span());
                }
                const auto& backingField = currentOwnerType_->fields[property->backingFieldIndex];
                auto assigned = bindAssignedValue(
                    property->type,
                    property->typeName, "property assignment");
                auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                oldAccess->span = syntaxTree.identifierToken.span;
                oldAccess->type = backingField.type;
                oldAccess->typeName = backingField.typeName;
                oldAccess->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                oldAccess->ownerType = *currentOwnerType_;
                oldAccess->field = backingField;
                auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->variable = *thisVariable;
                result->ownerType = *currentOwnerType_;
                result->field = backingField;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(assigned), syntaxTree.span());
                return result;
            }
            auto assigned = bindAssignedValue(
                property->type,
                property->typeName, "property assignment");
            auto result = std::make_unique<BoundPropertyAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = property->type;
            result->typeName = property->typeName;
            result->setter = *property->setter;
            if (property->staticProperty) {
                result->staticConstructor = findStaticConstructorForFunction(
                    visibleTypes_, *property->setter);
                if (!result->staticConstructor) {
                    result->staticConstructor = currentOwnerType_->staticConstructor;
                }
            }
            if (!property->staticProperty && thisVariable) {
                result->arguments.push_back(
                    makeVariableAccess(*thisVariable, syntaxTree.span()));
            }
            result->assignedValue = std::move(assigned);
            return result;
        }
    }

    diagnostics_.report(
        "RS2102",
        "undefined name '" + syntaxTree.identifierToken.text + "'",
        syntaxTree.identifierToken.span);
    return makeError(syntaxTree.span());
}


std::unique_ptr<BoundExpression> Binder::bindMemberAccessExpression(
    const syntax::MemberAccessExpressionSyntax& syntaxTree) {
    // Static enum member, static/const field, or static property.
    if (syntaxTree.receiver->kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<const syntax::NameExpressionSyntax&>(*syntaxTree.receiver);
        const auto type = visibleTypes_.find(name.identifierToken.text);
        if (!lookupVariable(name.identifierToken.text) &&
            type != visibleTypes_.end()) {
            if (!isTypeAccessible(type->second)) {
                diagnostics_.report(
                    "RS2534",
                    "type '" + canonicalTypeName(type->second) +
                        "' is inaccessible",
                    name.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                type->second.id,
                SymbolKind::Type,
                type->second.name,
                name.identifierToken.span);
            if (syntaxTree.nameToken.missing) {
                return makeError(syntaxTree.span());
            }
            if (type->second.kind == TypeKind::Enum) {
                const auto* member = findEnumMember(type->second, syntaxTree.nameToken.text);
                if (!member) {
                    diagnostics_.report("RS2480", "enum '" + canonicalTypeName(type->second) + "' has no member '" + syntaxTree.nameToken.text + "'", syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    member->id, "enum member", member->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    member->id,
                    SymbolKind::EnumMember,
                    member->name,
                    syntaxTree.nameToken.span);
                auto result = std::make_unique<BoundLiteralExpression>();
                result->span = syntaxTree.span();
                result->type = PrimitiveType::Enum;
                result->typeName = canonicalTypeName(type->second);
                result->value = member->value;
                return result;
            }
            if (const auto* field = findField(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!isMemberAccessible(
                        field->accessibility,
                        field->declaringTypeId)) {
                    diagnostics_.report(
                        "RS2534",
                        "field '" + field->name + "' is inaccessible",
                        syntaxTree.span());
                    return makeError(syntaxTree.span());
                }
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    field->id, "field", field->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    field->id,
                    SymbolKind::Field,
                    field->name,
                    syntaxTree.nameToken.span);
                if (!field->staticField && !field->constField) {
                    diagnostics_.report(
                        "RS2484",
                        "instance field '" + field->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (field->constField && field->hasConstantValue) {
                    auto literal = std::make_unique<BoundLiteralExpression>();
                    literal->span = syntaxTree.span();
                    literal->type = field->type;
                    literal->typeName = field->typeName;
                    literal->value = field->constantValue;
                    return literal;
                }
                auto result = std::make_unique<BoundMemberAccessExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->ownerType = type->second;
                result->field = *field;
                if (field->staticField &&
                    !type->second.staticConstructor.has_value() &&
                    !field->hasConstantValue && field->initializerSyntax) {
                    result->staticInitializer = bindStaticFieldInitializer(
                        type->second, *field);
                }
                return result;
            }
            if (const auto* property = findProperty(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!isMemberAccessible(
                        property->accessibility,
                        property->declaringTypeId)) {
                    diagnostics_.report(
                        "RS2534",
                        "property '" + property->name + "' is inaccessible",
                        syntaxTree.span());
                    return makeError(syntaxTree.span());
                }
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    property->id, "property", property->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    property->id,
                    SymbolKind::Property,
                    property->name,
                    syntaxTree.nameToken.span);
                if (!property->staticProperty) {
                    diagnostics_.report(
                        "RS2484",
                        "instance property '" + property->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (!property->getter) {
                    diagnostics_.report(
                        "RS2481",
                        "property '" + property->name + "' is write-only",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                auto result = std::make_unique<BoundCallExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->function = *property->getter;
                result->staticConstructor = findStaticConstructorForFunction(
                    visibleTypes_, *property->getter);
                if (!result->staticConstructor) {
                    result->staticConstructor = type->second.staticConstructor;
                }
                return result;
            }
        }
    }

    auto receiver = bindExpression(*syntaxTree.receiver);
    if (syntaxTree.nameToken.missing) {
        return makeError(syntaxTree.span());
    }
    if (receiver->type == PrimitiveType::Array &&
        (syntaxTree.nameToken.text == "length" ||
         syntaxTree.nameToken.text == "Length" ||
         syntaxTree.nameToken.text == "Count")) {
        auto result = std::make_unique<BoundArrayLengthExpression>();
        result->span = syntaxTree.span();
        result->type = PrimitiveType::Int;
        result->receiver = std::move(receiver);
        return applyNullConditional(
            std::move(result), syntaxTree.dotToken, syntaxTree.span());
    }
    if (receiver->type == PrimitiveType::String &&
        (syntaxTree.nameToken.text == "length" ||
         syntaxTree.nameToken.text == "Length" ||
         syntaxTree.nameToken.text == "Count")) {
        auto call = std::make_unique<BoundCallExpression>();
        call->span = syntaxTree.span();
        call->type = PrimitiveType::Int;
        call->function = makeIntrinsicFunction(
            "$intrinsic::string_length",
            PrimitiveType::Int,
            "",
            {{PrimitiveType::String, ""}});
        call->function.method = true;
        call->function.staticMethod = false;
        call->arguments.push_back(std::move(receiver));
        return applyNullConditional(
            std::move(call), syntaxTree.dotToken, syntaxTree.span());
    }
    if ((receiver->type != PrimitiveType::Object && receiver->type != PrimitiveType::Struct) || receiver->typeName.empty()) {
        diagnostics_.report("RS2404", "member access requires a class or struct value", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    const auto type = visibleTypes_.find(receiver->typeName);
    if (type == visibleTypes_.end()) {
        diagnostics_.report("RS2405", "type descriptor '" + receiver->typeName + "' is unavailable", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    if (const auto* field = findField(type->second, syntaxTree.nameToken.text)) {
        if (!isMemberAccessible(
                field->accessibility, field->declaringTypeId)) {
            diagnostics_.report(
                "RS2534", "field '" + field->name + "' is inaccessible",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        checkObsoleteReference(
            field->id, "field", field->name, syntaxTree.nameToken.span);
        recordSymbolReference(
            occurrences_,
            currentSourceName_,
            field->id,
            SymbolKind::Field,
            field->name,
            syntaxTree.nameToken.span);
        if (field->constField && field->hasConstantValue) {
            auto literal = std::make_unique<BoundLiteralExpression>();
            literal->span = syntaxTree.span();
            literal->type = field->type;
            literal->typeName = field->typeName;
            literal->value = field->constantValue;
            return literal;
        }
        if (field->staticField) {
            auto result = std::make_unique<BoundMemberAccessExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->ownerType = type->second;
            result->field = *field;
            if (!type->second.staticConstructor.has_value() &&
                !field->hasConstantValue && field->initializerSyntax) {
                result->staticInitializer = bindStaticFieldInitializer(
                    type->second, *field);
            }
            return result;
        }
        if (type->second.kind == TypeKind::Struct) {
            auto result = std::make_unique<BoundStructFieldAccessExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = type->second;
            result->field = *field;
            return result;
        }
        auto result = std::make_unique<BoundMemberAccessExpression>();
        result->span = syntaxTree.span();
        result->type = field->type;
        result->typeName = field->typeName;
        result->receiver = std::move(receiver);
        result->ownerType = type->second;
        result->field = *field;
        return applyNullConditional(
            std::move(result), syntaxTree.dotToken, syntaxTree.span());
    }
    if (const auto* property = findProperty(type->second, syntaxTree.nameToken.text)) {
        if (!isMemberAccessible(
                property->accessibility,
                property->declaringTypeId)) {
            diagnostics_.report(
                "RS2534",
                "property '" + property->name + "' is inaccessible",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        checkObsoleteReference(
            property->id, "property", property->name, syntaxTree.nameToken.span);
        recordSymbolReference(
            occurrences_,
            currentSourceName_,
            property->id,
            SymbolKind::Property,
            property->name,
            syntaxTree.nameToken.span);
        if (property->staticProperty) {
            diagnostics_.report(
                "RS2484",
                "static property '" + property->name +
                    "' must be accessed through its type",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        if (!property->getter) {
            diagnostics_.report("RS2481", "property '" + property->name + "' is write-only", syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        auto result = std::make_unique<BoundCallExpression>();
        result->span = syntaxTree.span();
        result->type = property->type;
        result->typeName = property->typeName;
        result->function = *property->getter;
        result->arguments.push_back(std::move(receiver));
        return applyNullConditional(
            std::move(result), syntaxTree.dotToken, syntaxTree.span());
    }
    if (syntaxTree.nameToken.text == "Count" ||
        syntaxTree.nameToken.text == "Length" ||
        syntaxTree.nameToken.text == "Capacity" ||
        syntaxTree.nameToken.text == "IsEmpty" ||
        syntaxTree.nameToken.text == "UseCount" ||
        syntaxTree.nameToken.text == "IsDisposed" ||
        syntaxTree.nameToken.text == "IsValid" ||
        syntaxTree.nameToken.text == "ResourceId") {
        auto getterMethods = findMethods(type->second, syntaxTree.nameToken.text, false);
        if (getterMethods.empty() && syntaxTree.nameToken.text == "Length") {
            getterMethods = findMethods(type->second, "Count", false);
        }
        for (const auto* method : getterMethods) {
            if (method->parameters.size() == visibleParameterOffset(*method) &&
                isMemberAccessible(method->accessibility, method->declaringTypeId, method->moduleName)) {
                return applyNullConditional(
                    bindSelectedCall(
                        *method,
                        {},
                        {},
                        {},
                        {},
                        std::move(receiver),
                        syntaxTree.span(),
                        "property getter"),
                    syntaxTree.dotToken, syntaxTree.span());
            }
        }
    }
    diagnostics_.report("RS2406", "type '" + receiver->typeName + "' has no member '" + syntaxTree.nameToken.text + "'", syntaxTree.nameToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::applyNullConditional(
    std::unique_ptr<BoundExpression> expression,
    const syntax::SyntaxToken& accessToken,
    text::TextSpan span) {
    if (accessToken.kind != syntax::SyntaxKind::QuestionDotToken) {
        return expression;
    }
    if (!expression || expression->type == PrimitiveType::Void ||
        expression->type == PrimitiveType::Null ||
        expression->type == PrimitiveType::Error) {
        diagnostics_.report(
            "RS8906",
            "null-conditional access requires a value-producing member",
            span);
        return makeError(span);
    }
    const bool referenceResult =
        expression->type == PrimitiveType::String ||
        expression->type == PrimitiveType::Object ||
        expression->type == PrimitiveType::Array;
    PrimitiveType valueType = expression->type;
    std::string valueTypeName = expression->typeName;
    TypeSymbol nullableType;
    FieldSymbol hasValueField;
    FieldSymbol valueField;
    if (!referenceResult) {
        auto typeText = valueTypeName.empty()
            ? std::string(primitiveTypeName(valueType))
            : valueTypeName;
        for (auto& character : typeText) {
            const bool alphaNumeric =
                (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '_';
            if (!alphaNumeric) character = '_';
        }
        const auto nullableName = "Nullable__" + typeText;
        auto found = visibleTypes_.find(nullableName);
        if (found == visibleTypes_.end()) {
            nullableType.moduleName = currentModuleName_;
            nullableType.name = nullableName;
            nullableType.kind = TypeKind::Struct;
            nullableType.synthetic = true;
            nullableType.sourceName = currentSourceName_;
            nullableType.declarationSpan = span;
            nullableType.id = stableTypeId(canonicalTypeName(nullableType));
            hasValueField.name = "hasValue";
            hasValueField.type = PrimitiveType::Bool;
            hasValueField.index = 0;
            hasValueField.synthetic = true;
            hasValueField.sourceName = currentSourceName_;
            hasValueField.declarationSpan = span;
            hasValueField.id = stableTypeId(
                canonicalTypeName(nullableType) + "::field:hasValue");
            valueField.name = "value";
            valueField.type = valueType;
            valueField.typeName = valueTypeName;
            valueField.index = 1;
            valueField.synthetic = true;
            valueField.sourceName = currentSourceName_;
            valueField.declarationSpan = span;
            valueField.id = stableTypeId(
                canonicalTypeName(nullableType) + "::field:value");
            nullableType.fields = {hasValueField, valueField};
            pendingTypes_.push_back(nullableType);
            visibleTypes_[nullableName] = nullableType;
            visibleTypes_[canonicalTypeName(nullableType)] = nullableType;
        } else {
            nullableType = found->second;
            if (nullableType.fields.size() < 2) {
                diagnostics_.report(
                    "RS8908", "nullable descriptor is incomplete", span);
                return makeError(span);
            }
            hasValueField = nullableType.fields[0];
            valueField = nullableType.fields[1];
        }
        expression->type = PrimitiveType::Struct;
        expression->typeName = canonicalTypeName(nullableType);
    }
    if (expression->kind() == BoundNodeKind::MemberAccessExpression) {
        auto& member = static_cast<BoundMemberAccessExpression&>(*expression);
        member.nullConditional = true;
        member.nullConditionalValueType = valueType;
        member.nullConditionalValueTypeName = valueTypeName;
        member.nullConditionalNullableType = nullableType;
        member.nullConditionalHasValueField = hasValueField;
        member.nullConditionalValueField = valueField;
        return expression;
    }
    if (expression->kind() == BoundNodeKind::CallExpression) {
        auto& call = static_cast<BoundCallExpression&>(*expression);
        if (call.arguments.empty() || !call.function.method ||
            call.function.staticMethod) {
            diagnostics_.report(
                "RS8907",
                "null-conditional access requires an instance receiver",
                span);
            return makeError(span);
        }
        call.nullConditional = true;
        call.nullConditionalValueType = valueType;
        call.nullConditionalValueTypeName = valueTypeName;
        call.nullConditionalNullableType = nullableType;
        call.nullConditionalHasValueField = hasValueField;
        call.nullConditionalValueField = valueField;
        return expression;
    }
    diagnostics_.report(
        "RS8907",
        "null-conditional access is not valid for this member",
        span);
    return makeError(span);
}

std::unique_ptr<BoundExpression> Binder::bindElementAccessExpression(
    const syntax::ElementAccessExpressionSyntax& syntaxTree) {
    auto receiver = bindExpression(*syntaxTree.receiver);
    if ((receiver->type == PrimitiveType::Object || receiver->type == PrimitiveType::Struct) &&
        !receiver->typeName.empty()) {
        const auto ownerIt = visibleTypes_.find(receiver->typeName);
        if (ownerIt != visibleTypes_.end()) {
            std::vector<std::unique_ptr<BoundExpression>> args;
            args.push_back(bindExpression(*syntaxTree.index));
            auto methods = findMethods(ownerIt->second, "Get", false);
            bool ambiguous = false;
            const auto plan = selectFlexibleCall(
                methods, args, nullptr, nullptr, visibleTypes_, ambiguous);
            if (plan.function && !ambiguous) {
                std::vector<std::unique_ptr<syntax::ExpressionSyntax>> synthArgs;
                auto dn0 = std::make_unique<syntax::NameExpressionSyntax>();
                dn0->identifierToken.span = syntaxTree.index->span();
                synthArgs.push_back(std::move(dn0));
                std::vector<std::optional<syntax::SyntaxToken>> synthMods;
                auto prepared = prepareCallArguments(
                    *plan.function, plan.sources, std::move(args),
                    synthArgs, synthMods, syntaxTree.span(), "indexer argument");
                return bindSelectedCall(
                    *plan.function,
                    std::move(prepared.arguments),
                    prepared.syntaxArguments,
                    prepared.modifiers,
                    std::move(prepared.evaluationOrder),
                    std::move(receiver),
                    syntaxTree.span(),
                    "indexer argument",
                    false);
            }
        }
    }
    if (receiver->type != PrimitiveType::Array || receiver->typeName.empty()) {
        diagnostics_.report("RS2421", "element access requires an array", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    if (!decodeArrayTypeName(receiver->typeName, elementType, elementTypeName)) {
        diagnostics_.report("RS2422", "array element descriptor is invalid", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    // Resolve exact named element kind when available.
    if (!elementTypeName.empty()) {
        const auto found = visibleTypes_.find(elementTypeName);
        if (found != visibleTypes_.end()) {
            elementType = found->second.kind == TypeKind::Class ? PrimitiveType::Object :
                found->second.kind == TypeKind::Struct ? PrimitiveType::Struct : PrimitiveType::Enum;
        }
    }
    auto index = convertExpression(bindExpression(*syntaxTree.index), PrimitiveType::Int, syntaxTree.index->span(), "array index");
    auto result = std::make_unique<BoundElementAccessExpression>();
    result->span = syntaxTree.span();
    result->type = elementType;
    result->typeName = elementTypeName;
    result->receiver = std::move(receiver);
    result->index = std::move(index);
    result->elementType = elementType;
    result->elementTypeName = std::move(elementTypeName);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindMemberAssignmentExpression(
    const syntax::MemberAssignmentExpressionSyntax& syntaxTree) {
    const auto compoundOp = compoundBinaryOperatorToken(syntaxTree.equalsToken);
    const auto bindReceiverAccess = [&]() -> std::unique_ptr<BoundExpression> {
        if (syntaxTree.receiver->kind() == syntax::SyntaxKind::ThisExpression &&
            currentOwnerType_ && currentOwnerType_->kind == TypeKind::Struct) {
            const auto* self = lookupVariable("this");
            if (self) {
                return variableExpression(*self, syntaxTree.receiver->span());
            }
            return makeError(syntaxTree.receiver->span());
        }
        return bindExpression(*syntaxTree.receiver);
    };

    // Static field or static property assignment.
    if (syntaxTree.receiver->kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<const syntax::NameExpressionSyntax&>(*syntaxTree.receiver);
        const auto type = visibleTypes_.find(name.identifierToken.text);
        if (!lookupVariable(name.identifierToken.text) &&
            type != visibleTypes_.end()) {
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                type->second.id,
                SymbolKind::Type,
                type->second.name,
                name.identifierToken.span);
            if (!isTypeAccessible(type->second)) {
                diagnostics_.report(
                    "RS2534",
                    "type '" + canonicalTypeName(type->second) +
                        "' is inaccessible",
                    name.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (const auto* field = findField(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!isMemberAccessible(
                        field->accessibility,
                        field->declaringTypeId)) {
                    diagnostics_.report(
                        "RS2534",
                        "field '" + field->name + "' is inaccessible",
                        syntaxTree.span());
                    return makeError(syntaxTree.span());
                }
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    field->id, "field", field->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    field->id,
                    SymbolKind::Field,
                    field->name,
                    syntaxTree.nameToken.span);
                if (field->constField) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot assign to const field '" + field->name + "'",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (!field->staticField) {
                    diagnostics_.report(
                        "RS2484",
                        "instance field '" + field->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (field->readonlyField &&
                    !isReadonlyFieldWritable(
                        *field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot assign to readonly field '" + field->name + "'",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                std::unique_ptr<BoundExpression> value;
                if (compoundOp) {
                    auto lhsValue = std::make_unique<BoundMemberAccessExpression>();
                    lhsValue->span = syntaxTree.nameToken.span;
                    lhsValue->type = field->type;
                    lhsValue->typeName = field->typeName;
                    lhsValue->ownerType = type->second;
                    lhsValue->field = *field;
                    if (!type->second.staticConstructor.has_value() &&
                        !field->hasConstantValue && field->initializerSyntax) {
                        lhsValue->staticInitializer = bindStaticFieldInitializer(
                            type->second, *field);
                    }
                    auto rhsValue = bindExpression(*syntaxTree.expression);
                    auto combined = bindBinaryOperation(
                        std::move(lhsValue),
                        syntaxTree.nameToken.span,
                        *compoundOp,
                        std::move(rhsValue),
                        syntaxTree.expression->span(),
                        syntaxTree.span());
                    value = convertExpression(
                        std::move(combined),
                        field->type,
                        syntaxTree.span(),
                        "field assignment",
                        field->typeName);
                } else {
                    value = bindTargetExpression(
                        *syntaxTree.expression,
                        field->type,
                        field->typeName,
                        "field assignment");
                }
                auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
                oldAccess->span = syntaxTree.nameToken.span;
                oldAccess->type = field->type;
                oldAccess->typeName = field->typeName;
                oldAccess->ownerType = type->second;
                oldAccess->field = *field;
                auto result = std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->ownerType = type->second;
                result->field = *field;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
                return result;
            }
            if (const auto* property = findProperty(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!isMemberAccessible(
                        property->accessibility,
                        property->declaringTypeId)) {
                    diagnostics_.report(
                        "RS2534",
                        "property '" + property->name + "' is inaccessible",
                        syntaxTree.span());
                    return makeError(syntaxTree.span());
                }
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    property->id, "property", property->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    property->id,
                    SymbolKind::Property,
                    property->name,
                    syntaxTree.nameToken.span);
                if (!property->staticProperty) {
                    diagnostics_.report(
                        "RS2484",
                        "instance property '" + property->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                const FieldSymbol* writableBackingField = nullptr;
                if (!property->setter) {
                    if (property->backingFieldIndex < type->second.fields.size()) {
                        const auto& candidate = type->second.fields[property->backingFieldIndex];
                        if (isReadonlyFieldWritable(
                                candidate,
                                currentConstructor_,
                                currentStaticMethod_,
                                currentOwnerType_)) {
                            writableBackingField = &candidate;
                        }
                    }
                    if (!writableBackingField) {
                        diagnostics_.report(
                            "RS2483",
                            "property '" + property->name + "' is read-only",
                            syntaxTree.nameToken.span);
                        return makeError(syntaxTree.span());
                    }
                }
                std::unique_ptr<BoundExpression> assigned;
                if (compoundOp) {
                    if (!property->getter) {
                        diagnostics_.report(
                            "RS2481",
                            "property '" + property->name + "' is write-only",
                            syntaxTree.nameToken.span);
                        return makeError(syntaxTree.span());
                    }
                    auto lhsValue = std::make_unique<BoundCallExpression>();
                    lhsValue->span = syntaxTree.nameToken.span;
                    lhsValue->type = property->type;
                    lhsValue->typeName = property->typeName;
                    lhsValue->function = *property->getter;
                    lhsValue->staticConstructor = findStaticConstructorForFunction(
                        visibleTypes_, *property->getter);
                    if (!lhsValue->staticConstructor) {
                        lhsValue->staticConstructor = type->second.staticConstructor;
                    }
                    auto rhsValue = bindExpression(*syntaxTree.expression);
                    auto combined = bindBinaryOperation(
                        std::move(lhsValue),
                        syntaxTree.nameToken.span,
                        *compoundOp,
                        std::move(rhsValue),
                        syntaxTree.expression->span(),
                        syntaxTree.span());
                    assigned = convertExpression(
                        std::move(combined),
                        property->type,
                        syntaxTree.span(),
                        "property assignment",
                        property->typeName);
                } else {
                    assigned = bindTargetExpression(
                        *syntaxTree.expression,
                        property->type,
                        property->typeName,
                        "property assignment");
                }
                if (writableBackingField) {
                    auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
                    oldAccess->span = syntaxTree.nameToken.span;
                    oldAccess->type = writableBackingField->type;
                    oldAccess->typeName = writableBackingField->typeName;
                    oldAccess->ownerType = type->second;
                    oldAccess->field = *writableBackingField;
                    auto result = std::make_unique<BoundMemberAssignmentExpression>();
                    result->span = syntaxTree.span();
                    result->type = property->type;
                    result->typeName = property->typeName;
                    result->ownerType = type->second;
                    result->field = *writableBackingField;
                    result->expression = wrapResourceHandleOverwriteRelease(
                        std::move(oldAccess), std::move(assigned), syntaxTree.span());
                    return result;
                }
                auto result = std::make_unique<BoundPropertyAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->setter = *property->setter;
                result->staticConstructor = findStaticConstructorForFunction(
                    visibleTypes_, *property->setter);
                if (!result->staticConstructor) {
                    result->staticConstructor = type->second.staticConstructor;
                }
                result->assignedValue = std::move(assigned);
                return result;
            }
        }
    }

    std::unique_ptr<BoundExpression> receiver = bindReceiverAccess();
    if ((receiver->type != PrimitiveType::Object && receiver->type != PrimitiveType::Struct) || receiver->typeName.empty()) {
        diagnostics_.report("RS2404", "member assignment requires a class or struct value", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    const auto type = visibleTypes_.find(receiver->typeName);
    if (type == visibleTypes_.end()) {
        diagnostics_.report("RS2405", "type descriptor '" + receiver->typeName + "' is unavailable", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    if (const auto* field = findField(type->second, syntaxTree.nameToken.text)) {
        if (!isMemberAccessible(
                field->accessibility, field->declaringTypeId)) {
            diagnostics_.report(
                "RS2534", "field '" + field->name + "' is inaccessible",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        checkObsoleteReference(
            field->id, "field", field->name, syntaxTree.nameToken.span);
        recordSymbolReference(
            occurrences_,
            currentSourceName_,
            field->id,
            SymbolKind::Field,
            field->name,
            syntaxTree.nameToken.span);
        if (field->constField) {
            diagnostics_.report(
                "RS2483",
                "cannot assign to const field '" + field->name + "'",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        if (field->readonlyField &&
            !isReadonlyFieldWritable(
                *field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
            diagnostics_.report(
                "RS2483",
                "cannot assign to readonly field '" + field->name + "'",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        if (!field->staticField && type->second.kind == TypeKind::Struct) {
            std::vector<StructAssignmentHop> hopsRev;
            std::unique_ptr<BoundExpression> cursor = std::move(receiver);
            while (cursor && cursor->kind() == BoundNodeKind::StructFieldAccessExpression) {
                auto& sHop = static_cast<BoundStructFieldAccessExpression&>(*cursor);
                if (sHop.field.constField ||
                    (sHop.field.readonlyField &&
                     !isReadonlyFieldWritable(
                         sHop.field, currentConstructor_, currentStaticMethod_, currentOwnerType_))) {
                    diagnostics_.report(
                        "RS2483",
                        std::string("cannot assign to field of ") +
                            (sHop.field.constField ? "const" : "readonly") +
                            " struct '" + sHop.field.name + "'",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                hopsRev.push_back(StructAssignmentHop{sHop.ownerType, sHop.field});
                cursor = std::move(sHop.receiver);
            }
            std::vector<StructAssignmentHop> intermediateHops(hopsRev.rbegin(), hopsRev.rend());

            auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->ownerType = type->second;
            result->field = *field;

            const bool needsOldRead =
                compoundOp.has_value() ||
                (!isInsideResourceHandleImplementation() &&
                 isManagedResourceHandleType(field->type, field->typeName));

            const VariableSymbol* rootVar = nullptr;
            if (cursor && cursor->kind() == BoundNodeKind::VariableExpression) {
                rootVar = &static_cast<const BoundVariableExpression&>(*cursor).variable;
            } else if (cursor && cursor->kind() == BoundNodeKind::MemberAccessExpression) {
                const auto& mRecv = static_cast<const BoundMemberAccessExpression&>(*cursor);
                if (mRecv.receiver && mRecv.receiver->kind() == BoundNodeKind::VariableExpression) {
                    const auto& candVar = static_cast<const BoundVariableExpression&>(*mRecv.receiver).variable;
                    if (const auto* origVar = lookupVariable(candVar.name);
                        origVar && (storageTypeOf(*origVar) != origVar->type || storageTypeNameOf(*origVar) != origVar->typeName)) {
                        rootVar = origVar;
                    }
                }
            }

            std::function<std::unique_ptr<BoundExpression>()> makeRootReadExpr;
            if (rootVar) {
                if (rootVar->isConst || rootVar->modifier == ParameterModifier::In) {
                    diagnostics_.report(
                        "RS8702",
                        "cannot assign to field of read-only 'in' or 'Read<T>' variable '" +
                            rootVar->name + "'",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                if (rootVar->isUsingResource) {
                    diagnostics_.report(
                        "RS2651",
                        "cannot modify members of '" + rootVar->name +
                            "' because it is a 'using' variable",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                const VariableSymbol capturedRootVar = *rootVar;
                result->rootKind = StructAssignmentRootKind::Variable;
                result->variable = capturedRootVar;
                if (storageTypeOf(capturedRootVar) != capturedRootVar.type ||
                    storageTypeNameOf(capturedRootVar) != capturedRootVar.typeName) {
                    const auto wrapper = visibleTypes_.find(storageTypeNameOf(capturedRootVar));
                    if (wrapper == visibleTypes_.end() || wrapper->second.fields.empty()) {
                        diagnostics_.report(
                            "RS8705",
                            "mutable struct receiver storage descriptor is unavailable",
                            syntaxTree.receiver->span());
                        return makeError(syntaxTree.span());
                    }
                    result->wrappedVariable = true;
                    result->wrapperType = wrapper->second;
                    result->wrapperValueField = wrapper->second.fields.front();
                }
                makeRootReadExpr = [this, capturedRootVar, &syntaxTree]() {
                    return makeVariableAccess(capturedRootVar, syntaxTree.receiver->span());
                };
            } else if (cursor && cursor->kind() == BoundNodeKind::MemberAccessExpression) {
                auto& memberRecv = static_cast<BoundMemberAccessExpression&>(*cursor);
                if (memberRecv.nullConditional) {
                    diagnostics_.report("RS2482", "struct field assignment requires a variable receiver", syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                if (memberRecv.field.constField) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot assign to field of const struct '" +
                            memberRecv.field.name + "'",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                if (memberRecv.field.readonlyField &&
                    !isReadonlyFieldWritable(
                        memberRecv.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot assign to field of readonly struct '" +
                            memberRecv.field.name + "'",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                if (memberRecv.field.isUsingResource) {
                    diagnostics_.report(
                        "RS2651",
                        "cannot modify members of '" + memberRecv.field.name +
                            "' because it is a 'using' variable",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                const TypeSymbol mOwner = memberRecv.ownerType;
                const FieldSymbol mField = memberRecv.field;
                if (mField.staticField) {
                    result->rootKind = StructAssignmentRootKind::StaticField;
                    result->staticFieldReceiver = true;
                    result->staticReceiverOwnerType = mOwner;
                    result->staticReceiverField = mField;
                    result->staticReceiverInitializer = std::move(memberRecv.staticInitializer);
                    makeRootReadExpr = [mOwner, mField, &syntaxTree]() {
                        auto r = std::make_unique<BoundMemberAccessExpression>();
                        r->span = syntaxTree.receiver->span();
                        r->type = mField.type;
                        r->typeName = mField.typeName;
                        r->ownerType = mOwner;
                        r->field = mField;
                        return r;
                    };
                } else if (memberRecv.receiver && mOwner.kind == TypeKind::Class) {
                    result->rootKind = StructAssignmentRootKind::ObjectField;
                    result->objectOwnerType = mOwner;
                    result->objectField = mField;
                    if (needsOldRead) {
                        if (memberRecv.receiver->kind() == BoundNodeKind::VariableExpression) {
                            const VariableSymbol rv =
                                static_cast<const BoundVariableExpression&>(*memberRecv.receiver).variable;
                            result->objectReceiver = std::move(memberRecv.receiver);
                            makeRootReadExpr = [this, rv, mOwner, mField, &syntaxTree]() {
                                auto r = std::make_unique<BoundMemberAccessExpression>();
                                r->span = syntaxTree.receiver->span();
                                r->type = mField.type;
                                r->typeName = mField.typeName;
                                r->receiver = variableExpression(rv, syntaxTree.receiver->span());
                                r->ownerType = mOwner;
                                r->field = mField;
                                return r;
                            };
                        } else {
                            const VariableSymbol recvTemp = allocateTempVariable(
                                "struct_obj_recv",
                                memberRecv.receiver->type,
                                memberRecv.receiver->typeName,
                                memberRecv.receiver->span);
                            auto recvAssign = std::make_unique<BoundAssignmentExpression>();
                            recvAssign->span = memberRecv.receiver->span;
                            recvAssign->type = recvTemp.type;
                            recvAssign->typeName = recvTemp.typeName;
                            recvAssign->variable = recvTemp;
                            recvAssign->expression = std::move(memberRecv.receiver);
                            result->objectReceiver = std::move(recvAssign);
                            makeRootReadExpr = [this, recvTemp, mOwner, mField, &syntaxTree]() {
                                auto r = std::make_unique<BoundMemberAccessExpression>();
                                r->span = syntaxTree.receiver->span();
                                r->type = mField.type;
                                r->typeName = mField.typeName;
                                r->receiver = makeVariableAccess(recvTemp, syntaxTree.receiver->span());
                                r->ownerType = mOwner;
                                r->field = mField;
                                return r;
                            };
                        }
                    } else {
                        result->objectReceiver = std::move(memberRecv.receiver);
                    }
                } else {
                    diagnostics_.report("RS2482", "struct field assignment requires a variable receiver", syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
            } else if (cursor && cursor->kind() == BoundNodeKind::ElementAccessExpression) {
                auto& elemRecv = static_cast<BoundElementAccessExpression&>(*cursor);
                const PrimitiveType eType = elemRecv.elementType;
                const std::string eTypeName = elemRecv.elementTypeName;
                result->rootKind = StructAssignmentRootKind::ArrayElement;
                result->elementType = eType;
                result->elementTypeName = eTypeName;
                if (needsOldRead) {
                    std::function<std::unique_ptr<BoundExpression>()> makeArrRead;
                    if (elemRecv.receiver->kind() == BoundNodeKind::VariableExpression) {
                        const VariableSymbol av =
                            static_cast<const BoundVariableExpression&>(*elemRecv.receiver).variable;
                        result->elementReceiver = std::move(elemRecv.receiver);
                        makeArrRead = [this, av, &syntaxTree]() {
                            return variableExpression(av, syntaxTree.receiver->span());
                        };
                    } else {
                        const VariableSymbol arrTemp = allocateTempVariable(
                            "struct_arr_recv",
                            elemRecv.receiver->type,
                            elemRecv.receiver->typeName,
                            elemRecv.receiver->span);
                        auto arrAssign = std::make_unique<BoundAssignmentExpression>();
                        arrAssign->span = elemRecv.receiver->span;
                        arrAssign->type = arrTemp.type;
                        arrAssign->typeName = arrTemp.typeName;
                        arrAssign->variable = arrTemp;
                        arrAssign->expression = std::move(elemRecv.receiver);
                        result->elementReceiver = std::move(arrAssign);
                        makeArrRead = [this, arrTemp, &syntaxTree]() {
                            return makeVariableAccess(arrTemp, syntaxTree.receiver->span());
                        };
                    }

                    std::function<std::unique_ptr<BoundExpression>()> makeIdxRead;
                    if (elemRecv.index->kind() == BoundNodeKind::VariableExpression) {
                        const VariableSymbol iv =
                            static_cast<const BoundVariableExpression&>(*elemRecv.index).variable;
                        result->elementIndex = std::move(elemRecv.index);
                        makeIdxRead = [this, iv, &syntaxTree]() {
                            return variableExpression(iv, syntaxTree.receiver->span());
                        };
                    } else if (elemRecv.index->kind() == BoundNodeKind::LiteralExpression) {
                        const auto& lit = static_cast<const BoundLiteralExpression&>(*elemRecv.index);
                        const auto litVal = lit.value;
                        const auto litType = lit.type;
                        const auto litTypeName = lit.typeName;
                        const auto litSpan = lit.span;
                        result->elementIndex = std::move(elemRecv.index);
                        makeIdxRead = [litVal, litType, litTypeName, litSpan]() {
                            auto copy = std::make_unique<BoundLiteralExpression>();
                            copy->span = litSpan;
                            copy->type = litType;
                            copy->typeName = litTypeName;
                            copy->value = litVal;
                            return copy;
                        };
                    } else {
                        const VariableSymbol idxTemp = allocateTempVariable(
                            "struct_arr_idx",
                            elemRecv.index->type,
                            elemRecv.index->typeName,
                            elemRecv.index->span);
                        auto idxAssign = std::make_unique<BoundAssignmentExpression>();
                        idxAssign->span = elemRecv.index->span;
                        idxAssign->type = idxTemp.type;
                        idxAssign->typeName = idxTemp.typeName;
                        idxAssign->variable = idxTemp;
                        idxAssign->expression = std::move(elemRecv.index);
                        result->elementIndex = std::move(idxAssign);
                        makeIdxRead = [this, idxTemp, &syntaxTree]() {
                            return makeVariableAccess(idxTemp, syntaxTree.receiver->span());
                        };
                    }

                    makeRootReadExpr = [makeArrRead, makeIdxRead, eType, eTypeName, &syntaxTree]() {
                        auto r = std::make_unique<BoundElementAccessExpression>();
                        r->span = syntaxTree.receiver->span();
                        r->type = eType;
                        r->typeName = eTypeName;
                        r->receiver = makeArrRead();
                        r->index = makeIdxRead();
                        r->elementType = eType;
                        r->elementTypeName = eTypeName;
                        return r;
                    };
                } else {
                    result->elementReceiver = std::move(elemRecv.receiver);
                    result->elementIndex = std::move(elemRecv.index);
                }
            } else {
                diagnostics_.report("RS2482", "struct field assignment requires a variable receiver", syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }

            const auto makeFullReceiverReadExpr = [&]() -> std::unique_ptr<BoundExpression> {
                auto expr = makeRootReadExpr();
                for (const auto& hop : intermediateHops) {
                    auto next = std::make_unique<BoundStructFieldAccessExpression>();
                    next->span = syntaxTree.receiver->span();
                    next->type = hop.field.type;
                    next->typeName = hop.field.typeName;
                    next->receiver = std::move(expr);
                    next->ownerType = hop.ownerType;
                    next->field = hop.field;
                    expr = std::move(next);
                }
                return expr;
            };

            std::unique_ptr<BoundExpression> structAssignedValue;
            if (compoundOp) {
                auto lhsValue = std::make_unique<BoundStructFieldAccessExpression>();
                lhsValue->span = syntaxTree.nameToken.span;
                lhsValue->type = field->type;
                lhsValue->typeName = field->typeName;
                lhsValue->receiver = makeFullReceiverReadExpr();
                lhsValue->ownerType = type->second;
                lhsValue->field = *field;
                auto rhsValue = bindExpression(*syntaxTree.expression);
                auto combined = bindBinaryOperation(
                    std::move(lhsValue),
                    syntaxTree.nameToken.span,
                    *compoundOp,
                    std::move(rhsValue),
                    syntaxTree.expression->span(),
                    syntaxTree.span());
                structAssignedValue = convertExpression(
                    std::move(combined),
                    field->type,
                    syntaxTree.span(),
                    "field assignment",
                    field->typeName);
            } else {
                structAssignedValue = bindTargetExpression(
                    *syntaxTree.expression, field->type, field->typeName,
                    "field assignment");
            }
            if (!isInsideResourceHandleImplementation() &&
                isManagedResourceHandleType(field->type, field->typeName)) {
                auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                oldAccess->span = syntaxTree.nameToken.span;
                oldAccess->type = field->type;
                oldAccess->typeName = field->typeName;
                oldAccess->receiver = makeFullReceiverReadExpr();
                oldAccess->ownerType = type->second;
                oldAccess->field = *field;
                structAssignedValue = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(structAssignedValue), syntaxTree.span());
            }
            result->intermediateHops = std::move(intermediateHops);
            result->expression = std::move(structAssignedValue);
            return result;
        }

        const bool needsFieldRhRelease =
            !field->staticField &&
            !isInsideResourceHandleImplementation() &&
            isManagedResourceHandleType(field->type, field->typeName);
        std::optional<VariableSymbol> sharedFieldRecvVar;
        if (!field->staticField && (compoundOp.has_value() || needsFieldRhRelease)) {
            if (receiver->kind() == BoundNodeKind::VariableExpression) {
                sharedFieldRecvVar = static_cast<const BoundVariableExpression&>(*receiver).variable;
            } else {
                const auto recvTemp = allocateTempVariable(
                    "field_recv", receiver->type, receiver->typeName, syntaxTree.receiver->span());
                auto recvAssign = std::make_unique<BoundAssignmentExpression>();
                recvAssign->span = syntaxTree.receiver->span();
                recvAssign->type = receiver->type;
                recvAssign->typeName = receiver->typeName;
                recvAssign->variable = recvTemp;
                recvAssign->expression = std::move(receiver);
                receiver = std::move(recvAssign);
                sharedFieldRecvVar = recvTemp;
            }
        }

        std::unique_ptr<BoundExpression> value;
        if (compoundOp) {
            std::unique_ptr<BoundExpression> lhsValue;
            if (field->staticField) {
                auto access = std::make_unique<BoundMemberAccessExpression>();
                access->span = syntaxTree.nameToken.span;
                access->type = field->type;
                access->typeName = field->typeName;
                access->ownerType = type->second;
                access->field = *field;
                if (!type->second.staticConstructor.has_value() &&
                    !field->hasConstantValue && field->initializerSyntax) {
                    access->staticInitializer = bindStaticFieldInitializer(
                        type->second, *field);
                }
                lhsValue = std::move(access);
            } else {
                auto access = std::make_unique<BoundMemberAccessExpression>();
                access->span = syntaxTree.nameToken.span;
                access->type = field->type;
                access->typeName = field->typeName;
                access->receiver = makeVariableAccess(*sharedFieldRecvVar, syntaxTree.receiver->span());
                access->ownerType = type->second;
                access->field = *field;
                lhsValue = std::move(access);
            }
            auto rhsValue = bindExpression(*syntaxTree.expression);
            auto combined = bindBinaryOperation(
                std::move(lhsValue),
                syntaxTree.nameToken.span,
                *compoundOp,
                std::move(rhsValue),
                syntaxTree.expression->span(),
                syntaxTree.span());
            value = convertExpression(
                std::move(combined),
                field->type,
                syntaxTree.span(),
                "field assignment",
                field->typeName);
        } else {
            value = bindTargetExpression(
                *syntaxTree.expression, field->type, field->typeName,
                "field assignment");
        }
        if (field->staticField) {
            auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
            oldAccess->span = syntaxTree.nameToken.span;
            oldAccess->type = field->type;
            oldAccess->typeName = field->typeName;
            oldAccess->ownerType = type->second;
            oldAccess->field = *field;
            auto result = std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->ownerType = type->second;
            result->field = *field;
            result->expression = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(value), syntaxTree.span());
            return result;
        }
        std::unique_ptr<BoundExpression> wrappedExpr = std::move(value);
        if (needsFieldRhRelease) {
            auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
            oldAccess->span = syntaxTree.nameToken.span;
            oldAccess->type = field->type;
            oldAccess->typeName = field->typeName;
            oldAccess->receiver = makeVariableAccess(*sharedFieldRecvVar, syntaxTree.receiver->span());
            oldAccess->ownerType = type->second;
            oldAccess->field = *field;
            wrappedExpr = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(wrappedExpr), syntaxTree.span());
        }
        auto result = std::make_unique<BoundMemberAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = field->type;
        result->typeName = field->typeName;
        result->receiver = std::move(receiver);
        result->ownerType = type->second;
        result->field = *field;
        result->expression = std::move(wrappedExpr);
        return result;
    }
    if (const auto* property = findProperty(type->second, syntaxTree.nameToken.text)) {
        if (!isMemberAccessible(
                property->accessibility,
                property->declaringTypeId)) {
            diagnostics_.report(
                "RS2534",
                "property '" + property->name + "' is inaccessible",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        checkObsoleteReference(
            property->id, "property", property->name, syntaxTree.nameToken.span);
        recordSymbolReference(
            occurrences_,
            currentSourceName_,
            property->id,
            SymbolKind::Property,
            property->name,
            syntaxTree.nameToken.span);
        if (property->staticProperty) {
            diagnostics_.report(
                "RS2484",
                "static property '" + property->name +
                    "' must be accessed through its type",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        const FieldSymbol* writableBackingField = nullptr;
        if (!property->setter) {
            const bool isOnThis =
                receiver->kind() == BoundNodeKind::VariableExpression &&
                static_cast<const BoundVariableExpression&>(*receiver).variable.name == "this";
            if (isOnThis && property->backingFieldIndex < type->second.fields.size()) {
                const auto& candidate = type->second.fields[property->backingFieldIndex];
                if (isReadonlyFieldWritable(
                        candidate,
                        currentConstructor_,
                        currentStaticMethod_,
                        currentOwnerType_)) {
                    writableBackingField = &candidate;
                }
            }
            if (!writableBackingField) {
                diagnostics_.report("RS2483", "property '" + property->name + "' is read-only", syntaxTree.nameToken.span);
                return makeError(syntaxTree.span());
            }
        }
        if (type->second.kind == TypeKind::Struct && property->backingFieldIndex != std::numeric_limits<std::size_t>::max()) {
            std::vector<StructAssignmentHop> hopsRev;
            std::unique_ptr<BoundExpression> cursor = std::move(receiver);
            while (cursor && cursor->kind() == BoundNodeKind::StructFieldAccessExpression) {
                auto& sHop = static_cast<BoundStructFieldAccessExpression&>(*cursor);
                if (sHop.field.constField ||
                    (sHop.field.readonlyField &&
                     !isReadonlyFieldWritable(
                         sHop.field, currentConstructor_, currentStaticMethod_, currentOwnerType_))) {
                    diagnostics_.report(
                        "RS2483",
                        std::string("cannot assign to field of ") +
                            (sHop.field.constField ? "const" : "readonly") +
                            " struct '" + sHop.field.name + "'",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                hopsRev.push_back(StructAssignmentHop{sHop.ownerType, sHop.field});
                cursor = std::move(sHop.receiver);
            }
            std::vector<StructAssignmentHop> intermediateHops(hopsRev.rbegin(), hopsRev.rend());

            const auto& field = type->second.fields[property->backingFieldIndex];
            auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = property->type;
            result->typeName = property->typeName;
            result->ownerType = type->second;
            result->field = field;

            const bool needsResourceRelease =
                !isInsideResourceHandleImplementation() &&
                isManagedResourceHandleType(field.type, field.typeName);
            const bool needsOldRead = compoundOp.has_value() || needsResourceRelease;
            std::function<std::unique_ptr<BoundExpression>()> makeRootReadExpr;

            const VariableSymbol* variable = nullptr;
            if (cursor && cursor->kind() == BoundNodeKind::VariableExpression) {
                variable = &static_cast<const BoundVariableExpression&>(*cursor).variable;
            } else if (cursor && cursor->kind() == BoundNodeKind::MemberAccessExpression) {
                const auto& mRecv = static_cast<const BoundMemberAccessExpression&>(*cursor);
                if (mRecv.receiver && mRecv.receiver->kind() == BoundNodeKind::VariableExpression) {
                    const auto& candVar = static_cast<const BoundVariableExpression&>(*mRecv.receiver).variable;
                    if (const auto* origVar = lookupVariable(candVar.name);
                        origVar && (storageTypeOf(*origVar) != origVar->type || storageTypeNameOf(*origVar) != origVar->typeName)) {
                        variable = origVar;
                    }
                }
            }
            if (variable) {
                if (intermediateHops.empty() && variable->name == "this" && !currentConstructor_) {
                    diagnostics_.report(
                        "RS2486",
                        "struct properties cannot be assigned through an instance method in the Phase 3E value model",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                if (variable->isConst || variable->modifier == ParameterModifier::In) {
                    diagnostics_.report(
                        "RS8702",
                        "cannot assign to field of read-only 'in' or 'Read<T>' variable '" +
                            variable->name + "'",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                if (variable->isUsingResource) {
                    diagnostics_.report(
                        "RS2651",
                        "cannot modify members of '" + variable->name +
                            "' because it is a 'using' variable",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                const VariableSymbol capturedRootVar = *variable;
                result->rootKind = StructAssignmentRootKind::Variable;
                result->variable = capturedRootVar;
                if (storageTypeOf(capturedRootVar) != capturedRootVar.type ||
                    storageTypeNameOf(capturedRootVar) != capturedRootVar.typeName) {
                    const auto wrapper = visibleTypes_.find(storageTypeNameOf(capturedRootVar));
                    if (wrapper == visibleTypes_.end() || wrapper->second.fields.empty()) {
                        diagnostics_.report(
                            "RS8705",
                            "mutable struct receiver storage descriptor is unavailable",
                            syntaxTree.receiver->span());
                        return makeError(syntaxTree.span());
                    }
                    result->wrappedVariable = true;
                    result->wrapperType = wrapper->second;
                    result->wrapperValueField = wrapper->second.fields.front();
                }
                makeRootReadExpr = [this, capturedRootVar, &syntaxTree]() {
                    return makeVariableAccess(capturedRootVar, syntaxTree.receiver->span());
                };
            } else if (cursor && cursor->kind() == BoundNodeKind::MemberAccessExpression) {
                auto& memberRecv = static_cast<BoundMemberAccessExpression&>(*cursor);
                if (memberRecv.nullConditional) {
                    diagnostics_.report("RS2482", "struct property assignment requires a variable receiver", syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                if (memberRecv.field.constField ||
                    (memberRecv.field.readonlyField &&
                     !isReadonlyFieldWritable(
                         memberRecv.field, currentConstructor_, currentStaticMethod_, currentOwnerType_))) {
                    diagnostics_.report(
                        "RS2483",
                        std::string("cannot assign to property of ") +
                            (memberRecv.field.constField ? "const" : "readonly") +
                            " struct '" + memberRecv.field.name + "'",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                if (memberRecv.field.isUsingResource) {
                    diagnostics_.report(
                        "RS2651",
                        "cannot modify members of '" + memberRecv.field.name +
                            "' because it is a 'using' variable",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                const TypeSymbol mOwner = memberRecv.ownerType;
                const FieldSymbol mField = memberRecv.field;
                if (mField.staticField) {
                    result->rootKind = StructAssignmentRootKind::StaticField;
                    result->staticFieldReceiver = true;
                    result->staticReceiverOwnerType = mOwner;
                    result->staticReceiverField = mField;
                    result->staticReceiverInitializer = std::move(memberRecv.staticInitializer);
                    makeRootReadExpr = [mOwner, mField, &syntaxTree]() {
                        auto r = std::make_unique<BoundMemberAccessExpression>();
                        r->span = syntaxTree.receiver->span();
                        r->type = mField.type;
                        r->typeName = mField.typeName;
                        r->ownerType = mOwner;
                        r->field = mField;
                        return r;
                    };
                } else if (memberRecv.receiver && mOwner.kind == TypeKind::Class) {
                    result->rootKind = StructAssignmentRootKind::ObjectField;
                    result->objectOwnerType = mOwner;
                    result->objectField = mField;
                    if (needsOldRead) {
                        if (memberRecv.receiver->kind() == BoundNodeKind::VariableExpression) {
                            const VariableSymbol rv =
                                static_cast<const BoundVariableExpression&>(*memberRecv.receiver).variable;
                            result->objectReceiver = std::move(memberRecv.receiver);
                            makeRootReadExpr = [this, rv, mOwner, mField, &syntaxTree]() {
                                auto r = std::make_unique<BoundMemberAccessExpression>();
                                r->span = syntaxTree.receiver->span();
                                r->type = mField.type;
                                r->typeName = mField.typeName;
                                r->receiver = variableExpression(rv, syntaxTree.receiver->span());
                                r->ownerType = mOwner;
                                r->field = mField;
                                return r;
                            };
                        } else {
                            const VariableSymbol recvTemp = allocateTempVariable(
                                "struct_obj_recv",
                                memberRecv.receiver->type,
                                memberRecv.receiver->typeName,
                                memberRecv.receiver->span);
                            auto recvAssign = std::make_unique<BoundAssignmentExpression>();
                            recvAssign->span = memberRecv.receiver->span;
                            recvAssign->type = recvTemp.type;
                            recvAssign->typeName = recvTemp.typeName;
                            recvAssign->variable = recvTemp;
                            recvAssign->expression = std::move(memberRecv.receiver);
                            result->objectReceiver = std::move(recvAssign);
                            makeRootReadExpr = [this, recvTemp, mOwner, mField, &syntaxTree]() {
                                auto r = std::make_unique<BoundMemberAccessExpression>();
                                r->span = syntaxTree.receiver->span();
                                r->type = mField.type;
                                r->typeName = mField.typeName;
                                r->receiver = makeVariableAccess(recvTemp, syntaxTree.receiver->span());
                                r->ownerType = mOwner;
                                r->field = mField;
                                return r;
                            };
                        }
                    } else {
                        result->objectReceiver = std::move(memberRecv.receiver);
                    }
                } else {
                    diagnostics_.report("RS2482", "struct property assignment requires a variable receiver", syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
            } else if (cursor && cursor->kind() == BoundNodeKind::ElementAccessExpression) {
                auto& elemRecv = static_cast<BoundElementAccessExpression&>(*cursor);
                const PrimitiveType eType = elemRecv.elementType;
                const std::string eTypeName = elemRecv.elementTypeName;
                result->rootKind = StructAssignmentRootKind::ArrayElement;
                result->elementType = eType;
                result->elementTypeName = eTypeName;
                if (needsOldRead) {
                    std::function<std::unique_ptr<BoundExpression>()> makeArrRead;
                    if (elemRecv.receiver->kind() == BoundNodeKind::VariableExpression) {
                        const VariableSymbol av =
                            static_cast<const BoundVariableExpression&>(*elemRecv.receiver).variable;
                        result->elementReceiver = std::move(elemRecv.receiver);
                        makeArrRead = [this, av, &syntaxTree]() {
                            return variableExpression(av, syntaxTree.receiver->span());
                        };
                    } else {
                        const VariableSymbol arrTemp = allocateTempVariable(
                            "struct_arr_recv",
                            elemRecv.receiver->type,
                            elemRecv.receiver->typeName,
                            elemRecv.receiver->span);
                        auto arrAssign = std::make_unique<BoundAssignmentExpression>();
                        arrAssign->span = elemRecv.receiver->span;
                        arrAssign->type = arrTemp.type;
                        arrAssign->typeName = arrTemp.typeName;
                        arrAssign->variable = arrTemp;
                        arrAssign->expression = std::move(elemRecv.receiver);
                        result->elementReceiver = std::move(arrAssign);
                        makeArrRead = [this, arrTemp, &syntaxTree]() {
                            return makeVariableAccess(arrTemp, syntaxTree.receiver->span());
                        };
                    }

                    std::function<std::unique_ptr<BoundExpression>()> makeIdxRead;
                    if (elemRecv.index->kind() == BoundNodeKind::VariableExpression) {
                        const VariableSymbol iv =
                            static_cast<const BoundVariableExpression&>(*elemRecv.index).variable;
                        result->elementIndex = std::move(elemRecv.index);
                        makeIdxRead = [this, iv, &syntaxTree]() {
                            return variableExpression(iv, syntaxTree.receiver->span());
                        };
                    } else if (elemRecv.index->kind() == BoundNodeKind::LiteralExpression) {
                        const auto& lit = static_cast<const BoundLiteralExpression&>(*elemRecv.index);
                        const auto litVal = lit.value;
                        const auto litType = lit.type;
                        const auto litTypeName = lit.typeName;
                        const auto litSpan = lit.span;
                        result->elementIndex = std::move(elemRecv.index);
                        makeIdxRead = [litVal, litType, litTypeName, litSpan]() {
                            auto copy = std::make_unique<BoundLiteralExpression>();
                            copy->span = litSpan;
                            copy->type = litType;
                            copy->typeName = litTypeName;
                            copy->value = litVal;
                            return copy;
                        };
                    } else {
                        const VariableSymbol idxTemp = allocateTempVariable(
                            "struct_arr_idx",
                            elemRecv.index->type,
                            elemRecv.index->typeName,
                            elemRecv.index->span);
                        auto idxAssign = std::make_unique<BoundAssignmentExpression>();
                        idxAssign->span = elemRecv.index->span;
                        idxAssign->type = idxTemp.type;
                        idxAssign->typeName = idxTemp.typeName;
                        idxAssign->variable = idxTemp;
                        idxAssign->expression = std::move(elemRecv.index);
                        result->elementIndex = std::move(idxAssign);
                        makeIdxRead = [this, idxTemp, &syntaxTree]() {
                            return makeVariableAccess(idxTemp, syntaxTree.receiver->span());
                        };
                    }

                    makeRootReadExpr = [makeArrRead, makeIdxRead, eType, eTypeName, &syntaxTree]() {
                        auto r = std::make_unique<BoundElementAccessExpression>();
                        r->span = syntaxTree.receiver->span();
                        r->type = eType;
                        r->typeName = eTypeName;
                        r->receiver = makeArrRead();
                        r->index = makeIdxRead();
                        r->elementType = eType;
                        r->elementTypeName = eTypeName;
                        return r;
                    };
                } else {
                    result->elementReceiver = std::move(elemRecv.receiver);
                    result->elementIndex = std::move(elemRecv.index);
                }
            } else {
                diagnostics_.report("RS2482", "struct property assignment requires a variable receiver", syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }

            const auto makeFullReceiverReadExpr = [&]() -> std::unique_ptr<BoundExpression> {
                auto expr = makeRootReadExpr();
                for (const auto& hop : intermediateHops) {
                    auto next = std::make_unique<BoundStructFieldAccessExpression>();
                    next->span = syntaxTree.receiver->span();
                    next->type = hop.field.type;
                    next->typeName = hop.field.typeName;
                    next->receiver = std::move(expr);
                    next->ownerType = hop.ownerType;
                    next->field = hop.field;
                    expr = std::move(next);
                }
                return expr;
            };

            std::unique_ptr<BoundExpression> structAssignedValue;
            if (compoundOp) {
                if (!property->getter) {
                    diagnostics_.report(
                        "RS2481",
                        "property '" + property->name + "' is write-only",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                auto lhsValue = std::make_unique<BoundCallExpression>();
                lhsValue->span = syntaxTree.nameToken.span;
                lhsValue->type = property->type;
                lhsValue->typeName = property->typeName;
                lhsValue->function = *property->getter;
                lhsValue->arguments.push_back(makeFullReceiverReadExpr());
                auto rhsValue = bindExpression(*syntaxTree.expression);
                auto combined = bindBinaryOperation(
                    std::move(lhsValue),
                    syntaxTree.nameToken.span,
                    *compoundOp,
                    std::move(rhsValue),
                    syntaxTree.expression->span(),
                    syntaxTree.span());
                structAssignedValue = convertExpression(
                    std::move(combined),
                    property->type,
                    syntaxTree.span(),
                    "property assignment",
                    property->typeName);
            } else {
                structAssignedValue = bindTargetExpression(
                    *syntaxTree.expression, property->type,
                    property->typeName, "property assignment");
            }
            if (needsResourceRelease) {
                auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                oldAccess->span = syntaxTree.nameToken.span;
                oldAccess->type = field.type;
                oldAccess->typeName = field.typeName;
                oldAccess->receiver = makeFullReceiverReadExpr();
                oldAccess->ownerType = type->second;
                oldAccess->field = field;
                structAssignedValue = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(structAssignedValue), syntaxTree.span());
            }

            result->intermediateHops = std::move(intermediateHops);
            result->expression = std::move(structAssignedValue);
            return result;
        }
        std::unique_ptr<BoundExpression> assigned;
        if (!compoundOp) {
            assigned = bindTargetExpression(
                *syntaxTree.expression, property->type,
                property->typeName, "property assignment");
        } else if (!property->getter) {
            diagnostics_.report(
                "RS2481",
                "property '" + property->name + "' is write-only",
                syntaxTree.nameToken.span);
            assigned = makeError(syntaxTree.span());
        } else {
            std::unique_ptr<BoundExpression> getterRecv;
            if (receiver->kind() == BoundNodeKind::VariableExpression) {
                const auto& rv = static_cast<const BoundVariableExpression&>(*receiver).variable;
                getterRecv = makeVariableAccess(rv, syntaxTree.receiver->span());
            } else {
                const auto recvTemp = allocateTempVariable(
                    "prop_recv", receiver->type, receiver->typeName, syntaxTree.receiver->span());
                auto recvAssign = std::make_unique<BoundAssignmentExpression>();
                recvAssign->span = syntaxTree.receiver->span();
                recvAssign->type = receiver->type;
                recvAssign->typeName = receiver->typeName;
                recvAssign->variable = recvTemp;
                recvAssign->expression = std::move(receiver);
                receiver = std::move(recvAssign);
                getterRecv = makeVariableAccess(recvTemp, syntaxTree.receiver->span());
            }
            auto lhsValue = std::make_unique<BoundCallExpression>();
            lhsValue->span = syntaxTree.nameToken.span;
            lhsValue->type = property->type;
            lhsValue->typeName = property->typeName;
            lhsValue->function = *property->getter;
            lhsValue->arguments.push_back(std::move(getterRecv));
            auto rhsValue = bindExpression(*syntaxTree.expression);
            auto combined = bindBinaryOperation(
                std::move(lhsValue),
                syntaxTree.nameToken.span,
                *compoundOp,
                std::move(rhsValue),
                syntaxTree.expression->span(),
                syntaxTree.span());
            assigned = convertExpression(
                std::move(combined),
                property->type,
                syntaxTree.span(),
                "property assignment",
                property->typeName);
        }
        if (writableBackingField) {
            const auto& thisVar = static_cast<const BoundVariableExpression&>(*receiver).variable;
            auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
            oldAccess->span = syntaxTree.nameToken.span;
            oldAccess->type = writableBackingField->type;
            oldAccess->typeName = writableBackingField->typeName;
            oldAccess->receiver = makeVariableAccess(thisVar, syntaxTree.receiver->span());
            oldAccess->ownerType = type->second;
            oldAccess->field = *writableBackingField;
            auto result = std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = property->type;
            result->typeName = property->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = type->second;
            result->field = *writableBackingField;
            result->expression = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(assigned), syntaxTree.span());
            return result;
        }
        auto result = std::make_unique<BoundPropertyAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = property->type;
        result->typeName = property->typeName;
        result->setter = *property->setter;
        result->arguments.push_back(std::move(receiver));
        result->assignedValue = std::move(assigned);
        return result;
    }
    diagnostics_.report("RS2406", "type '" + receiver->typeName + "' has no member '" + syntaxTree.nameToken.text + "'", syntaxTree.nameToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindElementAssignmentExpression(
    const syntax::ElementAssignmentExpressionSyntax& syntaxTree) {
    auto receiver = bindExpression(*syntaxTree.receiver);
    if ((receiver->type == PrimitiveType::Object || receiver->type == PrimitiveType::Struct) &&
        !receiver->typeName.empty()) {
        const auto ownerIt = visibleTypes_.find(receiver->typeName);
        if (ownerIt != visibleTypes_.end()) {
            auto idxExpr = bindExpression(*syntaxTree.index);
            std::unique_ptr<BoundExpression> valExpr;
            if (const auto compoundOp = compoundBinaryOperatorToken(syntaxTree.equalsToken)) {
                const bool isStructReceiver = (receiver->type == PrimitiveType::Struct);
                VariableSymbol recvTemp{};
                if (!isStructReceiver) {
                    recvTemp = allocateTempVariable(
                        "indexer_recv", receiver->type, receiver->typeName, syntaxTree.receiver->span());
                    auto recvAssign = std::make_unique<BoundAssignmentExpression>();
                    recvAssign->span = syntaxTree.receiver->span();
                    recvAssign->type = receiver->type;
                    recvAssign->typeName = receiver->typeName;
                    recvAssign->variable = recvTemp;
                    recvAssign->expression = std::move(receiver);
                    receiver = std::move(recvAssign);
                }

                const auto idxTemp = allocateTempVariable(
                    "indexer_idx", idxExpr->type, idxExpr->typeName, syntaxTree.index->span());
                auto idxAssign = std::make_unique<BoundAssignmentExpression>();
                idxAssign->span = syntaxTree.index->span();
                idxAssign->type = idxExpr->type;
                idxAssign->typeName = idxExpr->typeName;
                idxAssign->variable = idxTemp;
                idxAssign->expression = std::move(idxExpr);
                idxExpr = std::move(idxAssign);

                auto getMethods = findMethods(ownerIt->second, "Get", false);
                std::vector<std::unique_ptr<BoundExpression>> getArgs;
                getArgs.push_back(makeVariableAccess(idxTemp, syntaxTree.index->span()));
                bool getAmbig = false;
                const auto getPlan = selectFlexibleCall(
                    getMethods, getArgs, nullptr, nullptr, visibleTypes_, getAmbig);
                if (getPlan.function && !getAmbig) {
                    std::vector<std::unique_ptr<syntax::ExpressionSyntax>> emptySyntaxArgs;
                    auto dn0 = std::make_unique<syntax::NameExpressionSyntax>();
                    dn0->identifierToken.span = syntaxTree.index->span();
                    emptySyntaxArgs.push_back(std::move(dn0));
                    std::vector<std::optional<syntax::SyntaxToken>> emptyMods;
                    auto getPrep = prepareCallArguments(
                        *getPlan.function, getPlan.sources, std::move(getArgs),
                        emptySyntaxArgs, emptyMods, syntaxTree.span(), "indexer argument");
                    auto getReceiver = isStructReceiver
                        ? bindExpression(*syntaxTree.receiver)
                        : makeVariableAccess(recvTemp, syntaxTree.receiver->span());
                    auto lhsVal = bindSelectedCall(
                        *getPlan.function,
                        std::move(getPrep.arguments),
                        getPrep.syntaxArguments,
                        getPrep.modifiers,
                        std::move(getPrep.evaluationOrder),
                        std::move(getReceiver),
                        syntaxTree.span(),
                        "indexer argument",
                        false);
                    auto rhsVal = bindExpression(*syntaxTree.expression);
                    valExpr = bindBinaryOperation(
                        std::move(lhsVal),
                        syntaxTree.receiver->span(),
                        *compoundOp,
                        std::move(rhsVal),
                        syntaxTree.expression->span(),
                        syntaxTree.span());
                } else {
                    valExpr = bindExpression(*syntaxTree.expression);
                }
            } else {
                valExpr = bindExpression(*syntaxTree.expression);
            }
            std::vector<std::unique_ptr<BoundExpression>> args;
            args.push_back(std::move(idxExpr));
            args.push_back(std::move(valExpr));
            auto methods = findMethods(ownerIt->second, "Put", false);
            if (methods.empty()) {
                methods = findMethods(ownerIt->second, "Set", false);
            }
            bool ambiguous = false;
            const auto plan = selectFlexibleCall(
                methods, args, nullptr, nullptr, visibleTypes_, ambiguous);
            if (plan.function && !ambiguous) {
                std::vector<std::unique_ptr<syntax::ExpressionSyntax>> synthArgs;
                auto dn0 = std::make_unique<syntax::NameExpressionSyntax>();
                dn0->identifierToken.span = syntaxTree.index->span();
                synthArgs.push_back(std::move(dn0));
                auto dn1 = std::make_unique<syntax::NameExpressionSyntax>();
                dn1->identifierToken.span = syntaxTree.expression->span();
                synthArgs.push_back(std::move(dn1));
                std::vector<std::optional<syntax::SyntaxToken>> synthMods;
                auto prepared = prepareCallArguments(
                    *plan.function, plan.sources, std::move(args),
                    synthArgs, synthMods, syntaxTree.span(), "indexer assignment");
                auto callExpr = bindSelectedCall(
                    *plan.function,
                    std::move(prepared.arguments),
                    prepared.syntaxArguments,
                    prepared.modifiers,
                    std::move(prepared.evaluationOrder),
                    std::move(receiver),
                    syntaxTree.span(),
                    "indexer assignment",
                    false);
                return callExpr;
            }
        }
    }
    if (receiver->type != PrimitiveType::Array || receiver->typeName.empty()) {
        diagnostics_.report("RS2421", "element assignment requires an array", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    if (!decodeArrayTypeName(receiver->typeName, elementType, elementTypeName)) {
        diagnostics_.report("RS2422", "array element descriptor is invalid", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    if (!elementTypeName.empty()) {
        const auto found = visibleTypes_.find(elementTypeName);
        if (found != visibleTypes_.end()) {
            elementType = found->second.kind == TypeKind::Class ? PrimitiveType::Object :
                found->second.kind == TypeKind::Struct ? PrimitiveType::Struct : PrimitiveType::Enum;
        }
    }
    auto index = convertExpression(bindExpression(*syntaxTree.index), PrimitiveType::Int, syntaxTree.index->span(), "array index");
    std::unique_ptr<BoundExpression> value;
    if (const auto compoundOp = compoundBinaryOperatorToken(syntaxTree.equalsToken)) {
        const auto recvTemp = allocateTempVariable(
            "arr_recv", receiver->type, receiver->typeName, syntaxTree.receiver->span());
        auto recvAssign = std::make_unique<BoundAssignmentExpression>();
        recvAssign->span = syntaxTree.receiver->span();
        recvAssign->type = receiver->type;
        recvAssign->typeName = receiver->typeName;
        recvAssign->variable = recvTemp;
        recvAssign->expression = std::move(receiver);
        receiver = std::move(recvAssign);

        const auto idxTemp = allocateTempVariable(
            "arr_idx", PrimitiveType::Int, {}, syntaxTree.index->span());
        auto idxAssign = std::make_unique<BoundAssignmentExpression>();
        idxAssign->span = syntaxTree.index->span();
        idxAssign->type = PrimitiveType::Int;
        idxAssign->variable = idxTemp;
        idxAssign->expression = std::move(index);
        index = std::move(idxAssign);

        auto lhsValue = std::make_unique<BoundElementAccessExpression>();
        lhsValue->span = syntaxTree.span();
        lhsValue->type = elementType;
        lhsValue->typeName = elementTypeName;
        lhsValue->receiver = makeVariableAccess(recvTemp, syntaxTree.receiver->span());
        lhsValue->index = makeVariableAccess(idxTemp, syntaxTree.index->span());
        lhsValue->elementType = elementType;
        lhsValue->elementTypeName = elementTypeName;
        auto rhsValue = bindExpression(*syntaxTree.expression);
        auto combined = bindBinaryOperation(
            std::move(lhsValue),
            syntaxTree.receiver->span(),
            *compoundOp,
            std::move(rhsValue),
            syntaxTree.expression->span(),
            syntaxTree.span());
        value = convertExpression(
            std::move(combined),
            elementType,
            syntaxTree.span(),
            "array element assignment",
            elementTypeName);
    } else {
        value = convertExpression(bindExpression(*syntaxTree.expression), elementType, syntaxTree.expression->span(), "array element assignment", elementTypeName);
    }
    if (!isInsideResourceHandleImplementation() &&
        isManagedResourceHandleType(elementType, elementTypeName)) {
        std::unique_ptr<BoundExpression> oldArrRecv;
        if (receiver->kind() == BoundNodeKind::VariableExpression) {
            const auto& rv = static_cast<const BoundVariableExpression&>(*receiver).variable;
            oldArrRecv = variableExpression(rv, syntaxTree.receiver->span());
        } else {
            const auto recvTemp = allocateTempVariable(
                "rh_arr", receiver->type, receiver->typeName, syntaxTree.receiver->span());
            auto recvAssign = std::make_unique<BoundAssignmentExpression>();
            recvAssign->span = syntaxTree.receiver->span();
            recvAssign->type = receiver->type;
            recvAssign->typeName = receiver->typeName;
            recvAssign->variable = recvTemp;
            recvAssign->expression = std::move(receiver);
            receiver = std::move(recvAssign);
            oldArrRecv = makeVariableAccess(recvTemp, syntaxTree.receiver->span());
        }
        std::unique_ptr<BoundExpression> oldIdx;
        if (index->kind() == BoundNodeKind::LiteralExpression) {
            const auto& lit = static_cast<const BoundLiteralExpression&>(*index);
            auto copyLit = std::make_unique<BoundLiteralExpression>();
            copyLit->span = lit.span;
            copyLit->type = lit.type;
            copyLit->typeName = lit.typeName;
            copyLit->value = lit.value;
            oldIdx = std::move(copyLit);
        } else if (index->kind() == BoundNodeKind::VariableExpression) {
            const auto& iv = static_cast<const BoundVariableExpression&>(*index).variable;
            oldIdx = variableExpression(iv, syntaxTree.index->span());
        } else {
            const auto idxTemp = allocateTempVariable(
                "rh_idx", PrimitiveType::Int, {}, syntaxTree.index->span());
            auto idxAssign = std::make_unique<BoundAssignmentExpression>();
            idxAssign->span = syntaxTree.index->span();
            idxAssign->type = PrimitiveType::Int;
            idxAssign->variable = idxTemp;
            idxAssign->expression = std::move(index);
            index = std::move(idxAssign);
            oldIdx = makeVariableAccess(idxTemp, syntaxTree.index->span());
        }
        auto oldElem = std::make_unique<BoundElementAccessExpression>();
        oldElem->span = syntaxTree.span();
        oldElem->type = elementType;
        oldElem->typeName = elementTypeName;
        oldElem->receiver = std::move(oldArrRecv);
        oldElem->index = std::move(oldIdx);
        oldElem->elementType = elementType;
        oldElem->elementTypeName = elementTypeName;
        value = wrapResourceHandleOverwriteRelease(
            std::move(oldElem), std::move(value), syntaxTree.span());
    }
    auto result = std::make_unique<BoundElementAssignmentExpression>();
    result->span = syntaxTree.span();
    result->type = elementType;
    result->typeName = elementTypeName;
    result->receiver = std::move(receiver);
    result->index = std::move(index);
    result->expression = std::move(value);
    result->elementType = elementType;
    result->elementTypeName = std::move(elementTypeName);
    return result;
}


} // namespace realscript::semantic
