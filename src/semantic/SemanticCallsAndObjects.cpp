#include "SemanticInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace realscript::semantic {

using namespace detail;

std::unique_ptr<BoundExpression> Binder::bindDelegateCreation(
    const syntax::ExpressionSyntax& syntaxTree,
    const TypeSymbol& delegateType,
    const std::string& context) {
    if (syntaxTree.kind() == syntax::SyntaxKind::ParenthesizedExpression) {
        return bindDelegateCreation(
            *static_cast<const syntax::ParenthesizedExpressionSyntax&>(
                syntaxTree).expression,
            delegateType,
            context);
    }
    const auto* invoke = delegateInvokeMethod(delegateType);
    if (!invoke) {
        diagnostics_.report(
            "RS8801",
            "delegate type '" + canonicalTypeName(delegateType) +
                "' has no Invoke signature",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    if (syntaxTree.kind() == syntax::SyntaxKind::LambdaExpression) {
        const auto& lambda = static_cast<const
            syntax::LambdaExpressionSyntax&>(syntaxTree);
        const auto lambdaOrdinal = nextLambdaOrdinal_++;
        const auto invokeOffset = visibleParameterOffset(*invoke);
        if (lambda.parameterTokens.size() + invokeOffset !=
            invoke->parameters.size()) {
            diagnostics_.report(
                "RS8804",
                "lambda parameter count does not match delegate '" +
                    canonicalTypeName(delegateType) + "'",
                lambda.span());
            return makeError(lambda.span());
        }
        std::unordered_set<std::string> lambdaParameters;
        for (const auto& parameter : lambda.parameterTokens) {
            if (!lambdaParameters.insert(parameter.text).second) {
                diagnostics_.report(
                    "RS8804", "duplicate lambda parameter '" +
                        parameter.text + "'",
                    parameter.span);
            }
        }

        std::vector<syntax::SyntaxToken> referencedNames;
        collectLambdaNames(*lambda.body, referencedNames);
        std::vector<VariableSymbol> captured;
        std::unordered_set<SymbolId> capturedIds;
        for (const auto& name : referencedNames) {
            if (lambdaParameters.find(name.text) !=
                lambdaParameters.end()) {
                continue;
            }
            const auto* variable = lookupVariable(name.text);
            if (variable && capturedIds.insert(variable->id).second) {
                captured.push_back(*variable);
                continue;
            }
            if (currentOwnerType_ && !currentStaticMethod_ &&
                (findField(*currentOwnerType_, name.text) ||
                 findProperty(*currentOwnerType_, name.text))) {
                const auto* self = lookupVariable("this");
                if (self && capturedIds.insert(self->id).second) {
                    captured.push_back(*self);
                }
            }
        }
        std::sort(
            captured.begin(), captured.end(),
            [](const auto& left, const auto& right) {
                if (left.declarationSpan.start !=
                    right.declarationSpan.start) {
                    return left.declarationSpan.start <
                        right.declarationSpan.start;
                }
                return left.id < right.id;
            });

        TypeSymbol closure;
        closure.kind = TypeKind::Class;
        closure.accessibility = Accessibility::Private;
        closure.synthetic = true;
        closure.sealedType = true;
        closure.moduleName = currentModuleName_;
        closure.name = "$closure_" +
            std::to_string(currentFunctionId_) + "_" +
            std::to_string(lambdaOrdinal);
        closure.id = stableTypeId(closure);
        closure.sourceName = currentSourceName_;
        closure.declarationSpan = lambda.arrowToken.span;
        for (const auto& variable : captured) {
            FieldSymbol field;
            field.name = variable.name;
            field.accessibility = Accessibility::Private;
            field.declaringTypeId = closure.id;
            field.declaringTypeName = canonicalTypeName(closure);
            field.type = storageTypeOf(variable);
            field.typeName = storageTypeNameOf(variable);
            field.index = closure.fields.size();
            field.synthetic = true;
            field.sourceName = currentSourceName_;
            field.declarationSpan = variable.declarationSpan;
            field.id = stableTypeId(
                canonicalTypeName(closure) + "::capture:" +
                variable.name + ":" + std::to_string(variable.id));
            closure.fields.push_back(std::move(field));
        }

        FunctionSymbol lambdaFunction;
        lambdaFunction.accessibility = Accessibility::Private;
        lambdaFunction.declaringTypeId = closure.id;
        lambdaFunction.declaringTypeName = canonicalTypeName(closure);
        lambdaFunction.moduleName = currentModuleName_;
        lambdaFunction.name = "$lambda_" +
            std::to_string(lambdaOrdinal);
        lambdaFunction.ownerTypeName = closure.name;
        lambdaFunction.ownerTypeId = closure.id;
        lambdaFunction.returnType = invoke->returnType;
        lambdaFunction.returnTypeName = invoke->returnTypeName;
        lambdaFunction.method = true;
        lambdaFunction.synthetic = true;
        lambdaFunction.sourceName = currentSourceName_;
        lambdaFunction.declarationSpan = lambda.arrowToken.span;
        lambdaFunction.bodySpan = lambda.span();
        VariableSymbol self;
        self.name = "this";
        self.type = PrimitiveType::Object;
        self.typeName = canonicalTypeName(closure);
        self.parameter = true;
        self.index = 0;
        self.declarationSpan = lambda.arrowToken.span;
        lambdaFunction.parameters.push_back(std::move(self));
        for (std::size_t index = 0;
             index < lambda.parameterTokens.size(); ++index) {
            auto parameter = invoke->parameters[index + invokeOffset];
            parameter.name = lambda.parameterTokens[index].text;
            parameter.parameter = true;
            parameter.index = lambdaFunction.parameters.size();
            parameter.declarationSpan =
                lambda.parameterTokens[index].span;
            lambdaFunction.parameters.push_back(std::move(parameter));
        }
        lambdaFunction.id = stableFunctionId(lambdaFunction);
        for (auto& parameter : lambdaFunction.parameters) {
            parameter.id = stableTypeId(
                std::to_string(lambdaFunction.id) + "::local:" +
                std::to_string(parameter.index) + ":" +
                parameter.name);
        }
        closure.methods.push_back(lambdaFunction);

        FunctionBindingInput binding;
        binding.symbol = lambdaFunction;
        binding.sourceName = currentSourceName_;
        binding.eventLambda = &lambda;
        binding.hygienicAccessibleModules = hygienicAccessibleModules_;
        binding.parameterNames.push_back("this");
        binding.parameterSpans.push_back(lambda.arrowToken.span);
        for (const auto& parameter : lambda.parameterTokens) {
            binding.parameterNames.push_back(parameter.text);
            binding.parameterSpans.push_back(parameter.span);
        }
        pendingFunctionBindings_.push_back(std::move(binding));
        pendingTypes_.push_back(closure);
        visibleTypes_[closure.name] = closure;
        visibleTypes_[canonicalTypeName(closure)] = closure;

        auto result = std::make_unique<BoundDelegateCreationExpression>();
        result->span = lambda.span();
        result->type = PrimitiveType::Object;
        result->typeName = canonicalTypeName(delegateType);
        result->delegateType = delegateType;
        result->function = lambdaFunction;
        result->closureType = closure;
        result->captureFields = closure.fields;
        for (const auto& variable : captured) {
            result->captures.push_back(
                (storageTypeOf(variable) != variable.type || storageTypeNameOf(variable) != variable.typeName)
                    ? storageVariableExpression(variable, lambda.span())
                    : variableExpression(variable, lambda.span()));
        }
        return result;
    }

    std::string methodName;
    std::vector<const FunctionSymbol*> candidates;
    std::unique_ptr<BoundExpression> receiver;
    bool staticMemberGroup = false;
    TypeSymbol methodOwner;
    if (syntaxTree.kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<
            const syntax::NameExpressionSyntax&>(syntaxTree);
        methodName = name.identifierToken.text;
        const auto globals = visibleFunctions_.find(methodName);
        if (globals != visibleFunctions_.end()) {
            for (const auto& function : globals->second) {
                if (!function.method && isMemberAccessible(
                        function.accessibility,
                        function.declaringTypeId,
                        function.moduleName)) {
                    candidates.push_back(&function);
                }
            }
        }
        if (currentOwnerType_ && !currentStaticMethod_) {
            for (const auto* method : findMethods(
                     *currentOwnerType_, methodName, false)) {
                if (isMemberAccessible(
                        method->accessibility,
                        method->declaringTypeId,
                        method->moduleName)) {
                    candidates.push_back(method);
                }
            }
            if (!candidates.empty()) {
                const auto* self = lookupVariable("this");
                if (self) receiver = variableExpression(
                    *self, syntaxTree.span());
            }
        }
    } else if (syntaxTree.kind() ==
               syntax::SyntaxKind::MemberAccessExpression) {
        const auto& member = static_cast<
            const syntax::MemberAccessExpressionSyntax&>(syntaxTree);
        methodName = member.nameToken.text;
        if (member.receiver->kind() ==
            syntax::SyntaxKind::NameExpression) {
            const auto& name = static_cast<
                const syntax::NameExpressionSyntax&>(*member.receiver);
            const auto found = visibleTypes_.find(
                name.identifierToken.text);
            if (!lookupVariable(name.identifierToken.text) &&
                found != visibleTypes_.end()) {
                methodOwner = found->second;
                staticMemberGroup = true;
            }
        }
        if (!staticMemberGroup) {
            receiver = bindExpression(*member.receiver);
            if (receiver->type != PrimitiveType::Object ||
                receiver->typeName.empty()) {
                diagnostics_.report(
                    "RS8802",
                    "delegate instance method group requires an object receiver",
                    member.receiver->span());
                return makeError(syntaxTree.span());
            }
            const auto found = visibleTypes_.find(receiver->typeName);
            if (found == visibleTypes_.end()) {
                diagnostics_.report(
                    "RS8802", "delegate receiver type is unavailable",
                    member.receiver->span());
                return makeError(syntaxTree.span());
            }
            methodOwner = found->second;
        }
        for (const auto* method : findMethods(
                 methodOwner, methodName, staticMemberGroup)) {
            if (isMemberAccessible(
                    method->accessibility,
                    method->declaringTypeId,
                    method->moduleName)) {
                candidates.push_back(method);
            }
        }
    } else {
        diagnostics_.report(
            "RS8802", context + " requires a method group or lambda",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    const FunctionSymbol* selected = nullptr;
    for (const auto* candidate : candidates) {
        if (!delegateMethodMatches(*candidate, *invoke)) continue;
        if (selected && selected->id != candidate->id) {
            diagnostics_.report(
                "RS8803",
                "method group '" + methodName +
                    "' is ambiguous for delegate '" +
                    canonicalTypeName(delegateType) + "'",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        selected = candidate;
    }
    if (!selected) {
        diagnostics_.report(
            "RS8802",
            "method group '" + methodName +
                "' does not match delegate '" +
                canonicalTypeName(delegateType) + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    checkObsoleteReference(
        selected->id, "function", selected->name, syntaxTree.span());
    if (selected->staticMethod || !selected->method) receiver.reset();
    if (selected->method && !selected->staticMethod && !receiver) {
        const auto* self = lookupVariable("this");
        if (!self) {
            diagnostics_.report(
                "RS8802", "instance method group has no receiver",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        receiver = variableExpression(*self, syntaxTree.span());
    }

    auto result = std::make_unique<BoundDelegateCreationExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Object;
    result->typeName = canonicalTypeName(delegateType);
    result->delegateType = delegateType;
    result->function = *selected;
    result->receiver = std::move(receiver);
    result->interfaceDispatch = result->receiver &&
        selected->interfaceMethod &&
        selected->interfaceSlot != std::numeric_limits<std::uint32_t>::max();
    result->interfaceTypeId = result->interfaceDispatch
        ? selected->ownerTypeId
        : 0;
    result->interfaceSlot = result->interfaceDispatch
        ? selected->interfaceSlot
        : std::numeric_limits<std::uint32_t>::max();
    result->virtualDispatch = result->receiver &&
        !result->interfaceDispatch &&
        selected->virtualSlot != std::numeric_limits<std::uint32_t>::max();
    result->virtualSlot = result->virtualDispatch
        ? selected->virtualSlot
        : std::numeric_limits<std::uint32_t>::max();
    return result;
}


std::unique_ptr<BoundStatement> Binder::bindBaseConstructorInitializer(
    const FunctionBindingInput& input) {
    if ((!input.constructorSyntax && !input.symbol.constructor) ||
        input.symbol.staticMethod || !currentOwnerType_ ||
        currentOwnerType_->kind != TypeKind::Class ||
        currentOwnerType_->baseTypeId == 0) {
        return nullptr;
    }

    const auto diagSpan = input.constructorSyntax
        ? input.constructorSyntax->identifierToken.span
        : input.symbol.declarationSpan;
    const auto callSpan = input.constructorSyntax
        ? input.constructorSyntax->span()
        : input.symbol.declarationSpan;

    const auto* baseType = findVisibleType(currentOwnerType_->baseTypeId);
    if (!baseType) {
        diagnostics_.report(
            "RS2531",
            "base constructor type is not visible for '" +
                canonicalTypeName(*currentOwnerType_) + "'",
            diagSpan);
        return nullptr;
    }

    std::vector<std::unique_ptr<BoundExpression>> arguments;
    if (input.constructorSyntax) {
        arguments.reserve(input.constructorSyntax->baseArguments.size());
        for (const auto& argument : input.constructorSyntax->baseArguments) {
            arguments.push_back(bindExpression(*argument));
        }
    }

    if (baseType->constructors.empty()) {
        if (!arguments.empty()) {
            diagnostics_.report(
                "RS2532",
                "base type '" + canonicalTypeName(*baseType) +
                    "' has no constructor accepting the supplied arguments",
                callSpan);
        }
        return nullptr;
    }

    std::vector<const FunctionSymbol*> constructors;
    constructors.reserve(baseType->constructors.size());
    for (const auto& constructor : baseType->constructors) {
        if (isMemberAccessible(
                constructor.accessibility,
                constructor.declaringTypeId,
                constructor.moduleName)) {
            constructors.push_back(&constructor);
        }
    }
    bool ambiguous = false;
    const auto* selected = selectBest(
        constructors, arguments, nullptr, visibleTypes_, ambiguous);
    if (!selected) {
        diagnostics_.report(
            "RS2532",
            "no applicable base constructor for type '" +
                canonicalTypeName(*baseType) + "'",
            callSpan);
        return nullptr;
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2533",
            "base constructor call is ambiguous for type '" +
                canonicalTypeName(*baseType) + "'",
            callSpan);
        return nullptr;
    }

    const auto* thisVariable = lookupVariable("this");
    if (!thisVariable) return nullptr;
    std::vector<std::optional<syntax::SyntaxToken>> modifiers;
    const std::vector<const syntax::ExpressionSyntax*> emptySyntaxArgs;
    auto call = bindSelectedCall(
        *selected,
        std::move(arguments),
        input.constructorSyntax
            ? syntaxPointers(input.constructorSyntax->baseArguments)
            : emptySyntaxArgs,
        modifiers,
        {},
        variableExpression(*thisVariable, diagSpan),
        callSpan,
        "base constructor argument",
        true);
    auto statement = std::make_unique<BoundExpressionStatement>();
    statement->span = callSpan;
    statement->expression = std::move(call);
    return statement;
}


std::unique_ptr<BoundExpression> Binder::bindSelectedCall(
    const FunctionSymbol& function,
    std::vector<std::unique_ptr<BoundExpression>> arguments,
    const std::vector<const syntax::ExpressionSyntax*>& syntaxArguments,
    const std::vector<std::optional<syntax::SyntaxToken>>&
        argumentModifiers,
    std::vector<std::size_t> argumentEvaluationOrder,
    std::unique_ptr<BoundExpression> receiver,
    text::TextSpan span,
    const std::string& context,
    bool forceStaticDispatch) {
    if (!function.synthetic) {
        checkObsoleteReference(
            function.id,
            function.constructor ? "constructor" : (function.method ? "method" : "function"),
            function.ownerTypeName.empty()
                ? function.name
                : (function.ownerTypeName + "." + function.name),
            span);
    }
    if (currentDeterministicContext_ && function.nonDeterministic) {
        diagnostics_.report(
            "RS2495",
            "cannot call non-deterministic function '" + function.name +
                "' from a [Deterministic] context",
            span);
    }
    const auto offset = visibleParameterOffset(function);
    const auto argumentSpan = [&](std::size_t index) {
        return index < syntaxArguments.size() && syntaxArguments[index]
            ? syntaxArguments[index]->span()
            : arguments[index]->span;
    };
    const bool interfaceDispatch = receiver &&
        function.method && !function.staticMethod &&
        !forceStaticDispatch && function.interfaceMethod &&
        function.interfaceSlot != std::numeric_limits<std::uint32_t>::max();
    const bool virtualDispatch = receiver &&
        function.method && !function.staticMethod &&
        !forceStaticDispatch && !interfaceDispatch &&
        function.virtualSlot != std::numeric_limits<std::uint32_t>::max();
    bool hasModifiers = function.method && !function.staticMethod &&
        !function.parameters.empty() &&
        function.parameters.front().modifier != ParameterModifier::None;
    for (std::size_t index = offset;
         index < function.parameters.size(); ++index) {
        hasModifiers = hasModifiers ||
            function.parameters[index].modifier !=
                ParameterModifier::None;
    }
    if (!hasModifiers) {
        auto result = std::make_unique<BoundCallExpression>();
        result->span = span;
        result->type = storageReturnTypeOf(function);
        result->typeName = storageReturnTypeNameOf(function);
        result->function = function;
        result->staticConstructor = findStaticConstructorForFunction(
            visibleTypes_, function);
        result->virtualDispatch = virtualDispatch;
        result->virtualSlot = virtualDispatch
            ? function.virtualSlot
            : std::numeric_limits<std::uint32_t>::max();
        result->interfaceDispatch = interfaceDispatch;
        result->interfaceTypeId = interfaceDispatch
            ? function.ownerTypeId
            : 0;
        result->interfaceSlot = interfaceDispatch
            ? function.interfaceSlot
            : std::numeric_limits<std::uint32_t>::max();
        const auto receiverOffset = receiver ? std::size_t{1} : std::size_t{0};
        if (receiver) result->arguments.push_back(std::move(receiver));
        for (std::size_t index = 0;
             index < arguments.size(); ++index) {
            const auto& parameter =
                function.parameters[index + offset];
            result->arguments.push_back(convertExpression(
                std::move(arguments[index]),
                parameter.type,
                argumentSpan(index),
                context,
                parameter.typeName));
        }
        if (receiverOffset) result->argumentEvaluationOrder.push_back(0);
        if (argumentEvaluationOrder.empty()) {
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                result->argumentEvaluationOrder.push_back(
                    index + receiverOffset);
            }
        } else {
            for (const auto index : argumentEvaluationOrder) {
                result->argumentEvaluationOrder.push_back(
                    index + receiverOffset);
            }
        }
        return result;
    }

    auto result = std::make_unique<BoundReferenceCallExpression>();
    result->span = span;
    result->type = storageReturnTypeOf(function);
    result->typeName = storageReturnTypeNameOf(function);
    result->function = function;
    result->staticConstructor = findStaticConstructorForFunction(
        visibleTypes_, function);
    result->virtualDispatch = virtualDispatch;
    result->virtualSlot = virtualDispatch
        ? function.virtualSlot
        : std::numeric_limits<std::uint32_t>::max();
    result->interfaceDispatch = interfaceDispatch;
    result->interfaceTypeId = interfaceDispatch
        ? function.ownerTypeId
        : 0;
    result->interfaceSlot = interfaceDispatch
        ? function.interfaceSlot
        : std::numeric_limits<std::uint32_t>::max();
    const auto bindReferenceTarget = [&](BoundReferenceCallArgument& argument,
                                         std::unique_ptr<BoundExpression> value,
                                         text::TextSpan targetSpan,
                                         bool isImplicitReceiver) {
        if (value->kind() == BoundNodeKind::VariableExpression) {
            argument.targetKind = ReferenceTargetKind::Variable;
            argument.variable = static_cast<const
                BoundVariableExpression&>(*value).variable;
            if (argument.variable.isConst &&
                argument.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2483",
                    "cannot pass const variable '" +
                        argument.variable.name + "' as ref or out",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            if (!isImplicitReceiver &&
                argument.variable.isUsingResource &&
                argument.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2651",
                    "cannot pass '" + argument.variable.name +
                        "' as a ref or out argument because it is a 'using' variable",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            argument.value = std::move(value);
            return true;
        }
        if (value->kind() == BoundNodeKind::MemberAccessExpression) {
            auto& member = static_cast<
                BoundMemberAccessExpression&>(*value);
            if (!isImplicitReceiver &&
                member.field.isUsingResource &&
                argument.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2651",
                    "cannot pass 'using' variable as a ref or out argument",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            if (member.field.constField &&
                argument.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2483",
                    "cannot pass const field '" +
                        member.field.name + "' as ref or out",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            if (member.field.readonlyField &&
                argument.modifier != ParameterModifier::In &&
                !isReadonlyFieldWritable(
                    member.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                diagnostics_.report(
                    "RS2483",
                    "cannot pass readonly field '" +
                        member.field.name + "' as ref or out",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            argument.targetKind = ReferenceTargetKind::ObjectField;
            argument.targetOwnerType = member.ownerType;
            argument.targetField = member.field;
            if (member.field.staticField) {
                argument.value = std::move(value);
            } else {
                argument.targetReceiver = std::move(member.receiver);
            }
            return true;
        }
        if (value->kind() == BoundNodeKind::ElementAccessExpression) {
            auto& element = static_cast<
                BoundElementAccessExpression&>(*value);
            argument.targetKind = ReferenceTargetKind::ArrayElement;
            argument.targetReceiver = std::move(element.receiver);
            argument.targetIndex = std::move(element.index);
            argument.targetElementType = element.elementType;
            argument.targetElementTypeName = element.elementTypeName;
            return true;
        }
        if (value->kind() == BoundNodeKind::StructFieldAccessExpression) {
            auto& leafMember = static_cast<BoundStructFieldAccessExpression&>(*value);
            if ((leafMember.field.constField || leafMember.field.readonlyField) &&
                argument.modifier != ParameterModifier::In &&
                !isReadonlyFieldWritable(
                    leafMember.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                diagnostics_.report(
                    "RS2483",
                    std::string("cannot pass ") +
                        (leafMember.field.constField ? "const" : "readonly") +
                        " field '" + leafMember.field.name + "' as ref or out",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            const TypeSymbol leafOwnerType = leafMember.ownerType;
            const FieldSymbol leafField = leafMember.field;
            std::vector<StructAssignmentHop> hopsRev;
            std::unique_ptr<BoundExpression> cursor = std::move(leafMember.receiver);
            while (cursor && cursor->kind() == BoundNodeKind::StructFieldAccessExpression) {
                auto& sHop = static_cast<BoundStructFieldAccessExpression&>(*cursor);
                if ((sHop.field.constField || sHop.field.readonlyField) &&
                    argument.modifier != ParameterModifier::In &&
                    !isReadonlyFieldWritable(
                        sHop.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                    diagnostics_.report(
                        "RS2483",
                        std::string("cannot pass field of ") +
                            (sHop.field.constField ? "const" : "readonly") +
                            " struct '" + sHop.field.name + "' as ref or out",
                        targetSpan);
                    return false;
                }
                hopsRev.push_back(StructAssignmentHop{sHop.ownerType, sHop.field});
                cursor = std::move(sHop.receiver);
            }

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

            argument.targetKind = ReferenceTargetKind::StructField;
            argument.targetOwnerType = leafOwnerType;
            argument.targetField = leafField;
            argument.intermediateHops.assign(hopsRev.rbegin(), hopsRev.rend());

            if (rootVar) {
                if (rootVar->isConst && argument.modifier != ParameterModifier::In) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot pass const variable '" + rootVar->name + "' as ref or out",
                        targetSpan);
                    return false;
                }
                if (rootVar->modifier == ParameterModifier::In &&
                    argument.modifier != ParameterModifier::In) {
                    diagnostics_.report(
                        "RS8703",
                        "an in parameter cannot be forwarded as ref or out",
                        targetSpan);
                    return false;
                }
                if (!isImplicitReceiver && rootVar->isUsingResource &&
                    argument.modifier != ParameterModifier::In) {
                    diagnostics_.report(
                        "RS2651",
                        "cannot pass '" + rootVar->name +
                            "' as a ref or out argument because it is a 'using' variable",
                        targetSpan);
                    return false;
                }
                argument.structRootKind = StructAssignmentRootKind::Variable;
                argument.variable = *rootVar;
                if (storageTypeOf(*rootVar) != rootVar->type ||
                    storageTypeNameOf(*rootVar) != rootVar->typeName) {
                    const auto wrapper = visibleTypes_.find(storageTypeNameOf(*rootVar));
                    if (wrapper != visibleTypes_.end() && !wrapper->second.fields.empty()) {
                        argument.wrappedStructRootVariable = true;
                        argument.structRootWrapperType = wrapper->second;
                        argument.structRootWrapperValueField = wrapper->second.fields.front();
                    }
                }
                return true;
            }
            if (cursor && cursor->kind() == BoundNodeKind::MemberAccessExpression) {
                auto& mRecv = static_cast<BoundMemberAccessExpression&>(*cursor);
                if (mRecv.nullConditional) {
                    diagnostics_.report("RS8821", "nested struct reference target requires a variable owner", targetSpan);
                    return false;
                }
                if ((mRecv.field.constField || mRecv.field.readonlyField) &&
                    argument.modifier != ParameterModifier::In &&
                    !isReadonlyFieldWritable(
                        mRecv.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                    diagnostics_.report(
                        "RS2483",
                        std::string("cannot pass ") +
                            (mRecv.field.constField ? "const" : "readonly") +
                            " field '" + mRecv.field.name + "' as ref or out",
                        targetSpan);
                    return false;
                }
                if (mRecv.field.staticField) {
                    argument.structRootKind = StructAssignmentRootKind::StaticField;
                    argument.structRootOwnerType = mRecv.ownerType;
                    argument.structRootField = mRecv.field;
                    argument.structRootStaticInitializer = std::move(mRecv.staticInitializer);
                    return true;
                }
                if (mRecv.receiver && mRecv.ownerType.kind == TypeKind::Class) {
                    argument.structRootKind = StructAssignmentRootKind::ObjectField;
                    argument.structRootOwnerType = mRecv.ownerType;
                    argument.structRootField = mRecv.field;
                    argument.targetReceiver = std::move(mRecv.receiver);
                    return true;
                }
            }
            if (cursor && cursor->kind() == BoundNodeKind::ElementAccessExpression) {
                auto& elemRecv = static_cast<BoundElementAccessExpression&>(*cursor);
                argument.structRootKind = StructAssignmentRootKind::ArrayElement;
                argument.targetReceiver = std::move(elemRecv.receiver);
                argument.targetIndex = std::move(elemRecv.index);
                argument.targetElementType = elemRecv.elementType;
                argument.targetElementTypeName = elemRecv.elementTypeName;
                return true;
            }
            diagnostics_.report(
                "RS8821",
                "nested struct reference target requires a variable owner",
                targetSpan);
            return false;
        }
        diagnostics_.report(
            "RS8703",
            "reference argument requires a variable, field, or indexer l-value",
            targetSpan);
        argument.value = std::move(value);
        return false;
    };
    if (receiver) {
        BoundReferenceCallArgument receiverArgument;
        const auto& self = function.parameters.front();
        receiverArgument.modifier = self.modifier;
        if (self.modifier == ParameterModifier::Ref) {
            const auto receiverSpan = receiver->span;
            const auto canMutateStructChainInPlace = [&](const BoundExpression* expr) -> bool {
                const BoundExpression* cur = expr;
                while (cur && cur->kind() == BoundNodeKind::StructFieldAccessExpression) {
                    const auto& sf = static_cast<const BoundStructFieldAccessExpression&>(*cur);
                    if ((sf.field.constField || sf.field.readonlyField) &&
                        !isReadonlyFieldWritable(
                            sf.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                        return false;
                    }
                    cur = sf.receiver.get();
                }
                if (!cur) return false;
                if (cur->kind() == BoundNodeKind::VariableExpression) {
                    const auto& v = static_cast<const BoundVariableExpression&>(*cur).variable;
                    return !v.isConst && v.modifier != ParameterModifier::In;
                }
                if (cur->kind() == BoundNodeKind::ElementAccessExpression) {
                    return true;
                }
                if (cur->kind() == BoundNodeKind::MemberAccessExpression) {
                    const auto& m = static_cast<const BoundMemberAccessExpression&>(*cur);
                    if (m.nullConditional) return false;
                    if (m.receiver && m.receiver->kind() == BoundNodeKind::VariableExpression) {
                        const auto& cv = static_cast<const BoundVariableExpression&>(*m.receiver).variable;
                        if (const auto* ov = lookupVariable(cv.name);
                            ov && (storageTypeOf(*ov) != ov->type || storageTypeNameOf(*ov) != ov->typeName)) {
                            return !ov->isConst && ov->modifier != ParameterModifier::In;
                        }
                    }
                    if ((m.field.constField || m.field.readonlyField) &&
                        !isReadonlyFieldWritable(
                            m.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                        return false;
                    }
                    return m.field.staticField || (m.receiver && m.ownerType.kind == TypeKind::Class);
                }
                return false;
            };
            const bool directWritebackTarget = canMutateStructChainInPlace(receiver.get());
            if (!directWritebackTarget) {
                receiverArgument.targetKind = ReferenceTargetKind::None;
                receiverArgument.defensiveCopy = true;
                receiverArgument.value = std::move(receiver);
                const auto wrapper = visibleTypes_.find(
                    storageTypeNameOf(self));
                if (wrapper == visibleTypes_.end() ||
                    wrapper->second.fields.empty()) {
                    diagnostics_.report(
                        "RS8705",
                        "mutable struct receiver storage descriptor is unavailable",
                        receiverSpan);
                } else {
                    receiverArgument.wrapperType = wrapper->second;
                    receiverArgument.valueField =
                        wrapper->second.fields.front();
                }
            } else if (bindReferenceTarget(
                    receiverArgument, std::move(receiver), receiverSpan, true)) {
                const auto& variable = receiverArgument.variable;
                const auto wrapper = visibleTypes_.find(
                    storageTypeNameOf(self));
                if (wrapper == visibleTypes_.end() ||
                    wrapper->second.fields.empty()) {
                    diagnostics_.report(
                        "RS8705",
                        "mutable struct receiver storage descriptor is unavailable",
                        receiverSpan);
                } else {
                    receiverArgument.wrapperType = wrapper->second;
                    receiverArgument.valueField =
                        wrapper->second.fields.front();
                    if (receiverArgument.targetKind ==
                            ReferenceTargetKind::Variable &&
                        (variable.modifier == ParameterModifier::Ref ||
                         variable.modifier == ParameterModifier::Out) &&
                        storageTypeNameOf(variable) ==
                            storageTypeNameOf(self)) {
                        receiverArgument.forwarded = true;
                        receiverArgument.value =
                            storageVariableExpression(variable, receiverSpan);
                    } else if (receiverArgument.targetKind ==
                            ReferenceTargetKind::Variable &&
                        variable.modifier == ParameterModifier::In) {
                        receiverArgument.defensiveCopy = true;
                    }
                }
            }
        } else {
            receiverArgument.value = std::move(receiver);
        }
        result->arguments.push_back(std::move(receiverArgument));
    }

    for (std::size_t index = 0;
         index < arguments.size(); ++index) {
        const auto& parameter = function.parameters[index + offset];
        BoundReferenceCallArgument argument;
        argument.modifier = parameter.modifier;
        if (parameter.modifier == ParameterModifier::None) {
            argument.value = convertExpression(
                std::move(arguments[index]),
                parameter.type,
                argumentSpan(index),
                context,
                parameter.typeName);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        if (index >= argumentModifiers.size() ||
            syntaxModifier(argumentModifiers[index]) !=
                parameter.modifier) {
            diagnostics_.report(
                "RS8703",
                "reference argument must use the matching modifier",
                argumentSpan(index));
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        if (parameter.modifier == ParameterModifier::In) {
            argument.value = convertExpression(
                std::move(arguments[index]),
                parameter.type,
                argumentSpan(index),
                "in argument",
                parameter.typeName);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        bool boundTarget = false;
        if (syntaxArguments[index] && syntaxArguments[index]->kind() ==
            syntax::SyntaxKind::NameExpression) {
            const auto& name = static_cast<const
                syntax::NameExpressionSyntax&>(*syntaxArguments[index]);
            if (const auto* namedVariable = lookupVariable(
                    name.identifierToken.text)) {
                if (namedVariable->isConst &&
                    parameter.modifier != ParameterModifier::In) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot pass const variable '" +
                            namedVariable->name + "' as ref or out",
                        argumentSpan(index));
                }
                if (namedVariable->isUsingResource &&
                    parameter.modifier != ParameterModifier::In) {
                    diagnostics_.report(
                        "RS2651",
                        "cannot pass '" + namedVariable->name +
                            "' as a ref or out argument because it is a 'using' variable",
                        argumentSpan(index));
                }
                argument.targetKind = ReferenceTargetKind::Variable;
                argument.variable = *namedVariable;
                argument.value = std::move(arguments[index]);
                boundTarget = true;
            }
        }
        if (!boundTarget && !bindReferenceTarget(
                argument, std::move(arguments[index]),
                argumentSpan(index), false)) {
            result->arguments.push_back(std::move(argument));
            continue;
        }
        const auto* variable = argument.targetKind ==
                ReferenceTargetKind::Variable
            ? &argument.variable
            : nullptr;
        if (variable && variable->modifier == ParameterModifier::In &&
            parameter.modifier != ParameterModifier::In) {
            diagnostics_.report(
                "RS8703",
                "an in parameter cannot be forwarded as ref or out",
                argumentSpan(index));
        }
        const auto wrapper = visibleTypes_.find(
            storageTypeNameOf(parameter));
        if (wrapper == visibleTypes_.end() ||
            wrapper->second.fields.empty()) {
            diagnostics_.report(
                "RS8705",
                "reference argument storage descriptor is unavailable",
                argumentSpan(index));
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        argument.wrapperType = wrapper->second;
        argument.valueField = wrapper->second.fields.front();
        if (variable &&
            (variable->modifier == ParameterModifier::Ref ||
             variable->modifier == ParameterModifier::Out)) {
            if (storageTypeNameOf(*variable) !=
                storageTypeNameOf(parameter)) {
                diagnostics_.report(
                    "RS8703",
                    "forwarded reference parameter has an "
                    "incompatible storage type",
                    argumentSpan(index));
            }
            argument.forwarded = true;
            argument.value = storageVariableExpression(
                *variable, argumentSpan(index));
        } else if (parameter.modifier == ParameterModifier::Out &&
                   !argument.defensiveCopy &&
                   !isInsideResourceHandleImplementation() &&
                   isManagedResourceHandleType(parameter.type, parameter.typeName)) {
            const auto rhIt = visibleTypes_.find(parameter.typeName);
            if (rhIt != visibleTypes_.end()) {
                for (const auto& m : rhIt->second.methods) {
                    if (m.staticMethod && m.name == "__AssignHandle" &&
                        m.parameters.size() == 2 &&
                        m.returnType == PrimitiveType::Struct) {
                        argument.writebackAssignHandle = m;
                        break;
                    }
                }
            }
        }
        result->arguments.push_back(std::move(argument));
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindCallExpression(
    const syntax::CallExpressionSyntax& syntaxTree) {
    if (syntaxTree.identifierToken.text == "__rs_stable_hash" &&
        syntaxTree.arguments.size() == 1 &&
        syntaxTree.typeArguments.empty()) {
        auto arg = bindExpression(*syntaxTree.arguments.front());
        const auto argType = arg->type;
        const auto argTypeName = arg->typeName;
        const std::string typeTag = argTypeName.empty()
            ? std::string(primitiveTypeName(argType))
            : argTypeName;
        auto call = std::make_unique<BoundCallExpression>();
        call->span = syntaxTree.span();
        call->type = PrimitiveType::Int;
        call->function = makeIntrinsicFunction(
            "$intrinsic::stable_hash::" + typeTag,
            PrimitiveType::Int,
            "",
            {{argType, argTypeName}});
        call->arguments.push_back(std::move(arg));
        return call;
    }
    if (syntaxTree.identifierToken.text == "__rs_array_sort_range" &&
        syntaxTree.arguments.size() == 2 &&
        syntaxTree.typeArguments.empty()) {
        auto arrArg = bindExpression(*syntaxTree.arguments[0]);
        auto countArg = convertExpression(
            bindExpression(*syntaxTree.arguments[1]),
            PrimitiveType::Int,
            syntaxTree.arguments[1]->span(),
            "sort count");
        const auto arrTypeName = arrArg->typeName;
        auto call = std::make_unique<BoundCallExpression>();
        call->span = syntaxTree.span();
        call->type = PrimitiveType::Void;
        call->function = makeIntrinsicFunction(
            "$intrinsic::array_sort_range::" + arrTypeName,
            PrimitiveType::Void,
            "",
            {{PrimitiveType::Array, arrTypeName}, {PrimitiveType::Int, ""}});
        call->arguments.push_back(std::move(arrArg));
        call->arguments.push_back(std::move(countArg));
        return call;
    }
    if ((syntaxTree.identifierToken.text == "checked" ||
         syntaxTree.identifierToken.text == "unchecked") &&
        syntaxTree.arguments.size() == 1 &&
        syntaxTree.typeArguments.empty() &&
        (syntaxTree.argumentModifiers.empty() ||
         !syntaxTree.argumentModifiers.front())) {
        const auto previous = checkedArithmetic_;
        checkedArithmetic_ = syntaxTree.identifierToken.text == "checked";
        auto result = bindExpression(*syntaxTree.arguments.front());
        checkedArithmetic_ = previous;
        return result;
    }
    const TypeSymbol* calledDelegateType = nullptr;
    if (const auto* variable = lookupVariable(
            syntaxTree.identifierToken.text);
        variable && variable->type == PrimitiveType::Object &&
        !variable->typeName.empty()) {
        const auto found = visibleTypes_.find(variable->typeName);
        if (found != visibleTypes_.end() && found->second.delegateType) {
            calledDelegateType = &found->second;
        }
    }
    if (!calledDelegateType && currentOwnerType_) {
        const auto* field = findField(
            *currentOwnerType_, syntaxTree.identifierToken.text);
        if (field && (!currentStaticMethod_ || field->staticField) &&
            field->type == PrimitiveType::Object &&
            !field->typeName.empty()) {
            const auto found = visibleTypes_.find(field->typeName);
            if (found != visibleTypes_.end() && found->second.delegateType) {
                calledDelegateType = &found->second;
            }
        }
    }
    if (calledDelegateType) {
        const auto* invoke = delegateInvokeMethod(*calledDelegateType);
        if (!invoke) {
            diagnostics_.report(
                "RS8801", "delegate has no Invoke signature",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        const auto offset = visibleParameterOffset(*invoke);
        if (syntaxTree.arguments.size() + offset !=
            invoke->parameters.size()) {
            diagnostics_.report(
                "RS8805", "delegate invocation argument count mismatch",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        auto result =
            std::make_unique<BoundDelegateInvocationExpression>();
        result->span = syntaxTree.span();
        result->type = invoke->returnType;
        result->typeName = invoke->returnTypeName;
        result->delegateType = *calledDelegateType;
        syntax::NameExpressionSyntax delegateName;
        delegateName.identifierToken = syntaxTree.identifierToken;
        result->delegate = bindNameExpression(delegateName);
        for (std::size_t index = 0;
             index < syntaxTree.arguments.size(); ++index) {
            const auto& parameter = invoke->parameters[index + offset];
            BoundDelegateInvocationExpression::Argument argument;
            argument.modifier = parameter.modifier;
            const auto supplied = index <
                    syntaxTree.argumentModifiers.size()
                ? syntaxModifier(
                    syntaxTree.argumentModifiers[index])
                : ParameterModifier::None;
            if (supplied != parameter.modifier) {
                diagnostics_.report(
                    "RS8806",
                    "delegate argument modifier does not match parameter",
                    syntaxTree.arguments[index]->span());
            }
            if (parameter.modifier == ParameterModifier::None ||
                parameter.modifier == ParameterModifier::In) {
                argument.value = bindTargetExpression(
                    *syntaxTree.arguments[index],
                    parameter.type,
                    parameter.typeName,
                    "delegate argument");
                result->arguments.push_back(std::move(argument));
                continue;
            }
            if (syntaxTree.arguments[index]->kind() !=
                syntax::SyntaxKind::NameExpression) {
                diagnostics_.report(
                    "RS8703",
                    "ref and out delegate arguments must name a variable",
                    syntaxTree.arguments[index]->span());
                argument.value = bindExpression(
                    *syntaxTree.arguments[index]);
                result->arguments.push_back(std::move(argument));
                continue;
            }
            const auto& name = static_cast<const
                syntax::NameExpressionSyntax&>(
                    *syntaxTree.arguments[index]);
            const auto* variable = lookupVariable(
                name.identifierToken.text);
            if (!variable) {
                diagnostics_.report(
                    "RS8703",
                    "reference delegate argument must name a local or parameter",
                    syntaxTree.arguments[index]->span());
                result->arguments.push_back(std::move(argument));
                continue;
            }
            argument.variable = *variable;
            if (variable->isConst &&
                parameter.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2483",
                    "cannot pass const variable '" +
                        variable->name + "' as ref or out",
                    syntaxTree.arguments[index]->span());
            }
            if (variable->isUsingResource &&
                parameter.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2651",
                    "cannot pass '" + variable->name +
                        "' as a ref or out argument because it is a 'using' variable",
                    syntaxTree.arguments[index]->span());
            }
            if (variable->modifier == ParameterModifier::In &&
                parameter.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS8703",
                    "an in parameter cannot be forwarded as ref or out",
                    syntaxTree.arguments[index]->span());
            }
            const auto wrapper = visibleTypes_.find(
                storageTypeNameOf(parameter));
            if (wrapper == visibleTypes_.end() ||
                wrapper->second.fields.empty()) {
                diagnostics_.report(
                    "RS8705",
                    "reference delegate argument storage descriptor is unavailable",
                    syntaxTree.arguments[index]->span());
                result->arguments.push_back(std::move(argument));
                continue;
            }
            argument.wrapperType = wrapper->second;
            argument.valueField = wrapper->second.fields.front();
            if ((variable->modifier == ParameterModifier::Ref ||
                 variable->modifier == ParameterModifier::Out) &&
                storageTypeOf(*variable) == PrimitiveType::Object &&
                storageTypeNameOf(*variable) ==
                    storageTypeNameOf(parameter)) {
                argument.forwarded = true;
                argument.value = storageVariableExpression(
                    *variable, syntaxTree.arguments[index]->span());
            } else if (parameter.modifier == ParameterModifier::Ref) {
                argument.value = bindTargetExpression(
                    *syntaxTree.arguments[index],
                    parameter.type,
                    parameter.typeName,
                    "delegate ref argument");
            } else if (parameter.modifier == ParameterModifier::Out) {
                (void)bindTargetExpression(
                    *syntaxTree.arguments[index],
                    parameter.type,
                    parameter.typeName,
                    "delegate out argument");
                if (!isInsideResourceHandleImplementation() &&
                    isManagedResourceHandleType(parameter.type, parameter.typeName)) {
                    const auto rhIt = visibleTypes_.find(parameter.typeName);
                    if (rhIt != visibleTypes_.end()) {
                        for (const auto& m : rhIt->second.methods) {
                            if (m.staticMethod && m.name == "__AssignHandle" &&
                                m.parameters.size() == 2 &&
                                m.returnType == PrimitiveType::Struct) {
                                argument.writebackAssignHandle = m;
                                break;
                            }
                        }
                    }
                }
            }
            result->arguments.push_back(std::move(argument));
        }
        return result;
    }

    std::vector<const FunctionSymbol*> targetCandidates;
    const auto targetGlobals = visibleFunctions_.find(
        syntaxTree.identifierToken.text);
    if (targetGlobals != visibleFunctions_.end()) {
        for (const auto& function : targetGlobals->second) {
            if (!function.method && isMemberAccessible(
                    function.accessibility,
                    function.declaringTypeId,
                    function.moduleName)) {
                targetCandidates.push_back(&function);
            }
        }
    }
    if (currentOwnerType_) {
        if (!currentStaticMethod_) {
            for (const auto* method : findMethods(
                     *currentOwnerType_,
                     syntaxTree.identifierToken.text,
                     false)) {
                if (isMemberAccessible(
                        method->accessibility,
                        method->declaringTypeId,
                        method->moduleName)) {
                    targetCandidates.push_back(method);
                }
            }
        }
        for (const auto* method : findMethods(
                 *currentOwnerType_,
                 syntaxTree.identifierToken.text,
                 true)) {
            if (isMemberAccessible(
                    method->accessibility,
                    method->declaringTypeId,
                    method->moduleName)) {
                targetCandidates.push_back(method);
            }
        }
    }
    const FunctionSymbol* targetCandidate =
        targetCandidates.size() == 1
            ? targetCandidates.front()
            : nullptr;
    const auto targetOffset = targetCandidate
        ? visibleParameterOffset(*targetCandidate)
        : 0u;
    if (targetCandidate &&
        targetCandidate->parameters.size() !=
            syntaxTree.arguments.size() + targetOffset) {
        targetCandidate = nullptr;
    }
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (std::size_t index = 0;
         index < syntaxTree.arguments.size(); ++index) {
        if (targetCandidate) {
            const auto& parameter =
                targetCandidate->parameters[index + targetOffset];
            const auto targetType =
                visibleTypes_.find(parameter.typeName);
            const bool delegateTarget =
                parameter.type == PrimitiveType::Object &&
                targetType != visibleTypes_.end() &&
                targetType->second.delegateType;
            arguments.push_back(delegateTarget
                ? bindTargetExpression(
                    *syntaxTree.arguments[index],
                    parameter.type,
                    parameter.typeName,
                    "argument")
                : bindExpression(*syntaxTree.arguments[index]));
        } else {
            arguments.push_back(bindExpression(
                *syntaxTree.arguments[index]));
        }
    }

    if (currentOwnerType_ && !currentStaticMethod_) {
        for (const auto& event : currentOwnerType_->events) {
            if (event.name != syntaxTree.identifierToken.text) continue;
            auto result =
                std::make_unique<BoundEventInvocationExpression>();
            result->span = syntaxTree.span();
            result->type = PrimitiveType::Void;
            result->ownerType = *currentOwnerType_;
            result->event = event;
            const auto delegate = visibleTypes_.find(event.delegateName);
            if (delegate == visibleTypes_.end() ||
                !delegate->second.delegateType) {
                diagnostics_.report(
                    "RS8312",
                    "event delegate type was not resolved",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            result->delegateType = delegate->second;
            const auto* thisVariable = lookupVariable("this");
            if (!thisVariable) return makeError(syntaxTree.span());
            result->receiver = variableExpression(
                *thisVariable, syntaxTree.span());
            if (arguments.size() != event.parameters.size()) {
                diagnostics_.report(
                    "RS8313",
                    "event argument count does not match delegate",
                    syntaxTree.span());
            }
            const auto count = std::min(
                arguments.size(), event.parameters.size());
            for (std::size_t index = 0; index < count; ++index) {
                if (index < syntaxTree.argumentModifiers.size() &&
                    syntaxTree.argumentModifiers[index]) {
                    diagnostics_.report(
                        "RS8314",
                        "event arguments cannot use ref, out, or in",
                        syntaxTree.arguments[index]->span());
                }
                result->arguments.push_back(convertExpression(
                    std::move(arguments[index]),
                    event.parameters[index].type,
                    syntaxTree.arguments[index]->span(),
                    "event argument",
                    event.parameters[index].typeName));
            }
            return result;
        }
    }

    std::vector<const FunctionSymbol*> candidates;
    const auto globals = visibleFunctions_.find(
        syntaxTree.identifierToken.text);
    if (globals != visibleFunctions_.end()) {
        for (const auto& function : globals->second) {
            if (!function.method && isMemberAccessible(
                    function.accessibility,
                    function.declaringTypeId,
                    function.moduleName)) {
                candidates.push_back(&function);
            }
        }
    }
    if (currentOwnerType_) {
        if (!currentStaticMethod_) {
            auto methods = findMethods(
                *currentOwnerType_,
                syntaxTree.identifierToken.text,
                false);
            for (const auto* method : methods) {
                if (isMemberAccessible(
                        method->accessibility,
                        method->declaringTypeId,
                        method->moduleName)) {
                    candidates.push_back(method);
                }
            }
        }
        auto staticMethods = findMethods(
            *currentOwnerType_,
            syntaxTree.identifierToken.text,
            true);
        for (const auto* method : staticMethods) {
            if (isMemberAccessible(
                    method->accessibility,
                    method->declaringTypeId,
                    method->moduleName)) {
                candidates.push_back(method);
            }
        }
    }

    bool ambiguous = false;
    const auto plan = selectFlexibleCall(
        candidates, arguments, &syntaxTree.argumentNames,
        &syntaxTree.argumentModifiers, visibleTypes_, ambiguous);
    const auto* best = plan.function;
    if (!best) {
        diagnostics_.report(
            "RS2107",
            "no applicable overload for function '" +
                syntaxTree.identifierToken.text + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2108",
            "call to function '" +
                syntaxTree.identifierToken.text +
                "' is ambiguous",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    recordSymbolReference(
        occurrences_,
        currentSourceName_,
        best->id,
        SymbolKind::Function,
        best->name,
        syntaxTree.identifierToken.span);

    std::unique_ptr<BoundExpression> receiver;
    if (best->method && !best->staticMethod) {
        const auto* thisVariable = lookupVariable("this");
        if (!thisVariable) return makeError(syntaxTree.span());
        receiver = variableExpression(
            *thisVariable, syntaxTree.span());
    }
    auto prepared = prepareCallArguments(
        *best, plan.sources, std::move(arguments),
        syntaxTree.arguments, syntaxTree.argumentModifiers,
        syntaxTree.span(), "argument");
    return bindSelectedCall(
        *best,
        std::move(prepared.arguments),
        prepared.syntaxArguments,
        prepared.modifiers,
        std::move(prepared.evaluationOrder),
        std::move(receiver),
        syntaxTree.span(),
        "argument");
}


std::unique_ptr<BoundExpression> Binder::tryBindExtensionMethodCall(
    const syntax::MemberCallExpressionSyntax& syntaxTree,
    std::unique_ptr<BoundExpression>& receiver,
    std::vector<std::unique_ptr<BoundExpression>>& arguments) {
    if (!receiver) return nullptr;
    std::vector<const FunctionSymbol*> extCandidates;
    const auto globals = visibleFunctions_.find(syntaxTree.nameToken.text);
    if (globals != visibleFunctions_.end()) {
        for (const auto& fn : globals->second) {
            if (!fn.method && fn.extensionMethod && !fn.parameters.empty() &&
                isMemberAccessible(
                    fn.accessibility, fn.declaringTypeId, fn.moduleName)) {
                extCandidates.push_back(&fn);
            }
        }
    }
    for (const auto& [visName, visType] : visibleTypes_) {
        (void)visName;
        for (const auto& m : visType.methods) {
            if (m.staticMethod && m.extensionMethod &&
                m.name == syntaxTree.nameToken.text && !m.parameters.empty() &&
                isMemberAccessible(
                    m.accessibility, m.declaringTypeId, m.moduleName)) {
                extCandidates.push_back(&m);
            }
        }
    }
    if (extCandidates.empty()) return nullptr;

    std::vector<std::unique_ptr<BoundExpression>> extArgs;
    extArgs.reserve(arguments.size() + 1);
    extArgs.push_back(std::move(receiver));
    for (auto& arg : arguments) extArgs.push_back(std::move(arg));

    std::vector<std::optional<syntax::SyntaxToken>> extNames;
    if (!syntaxTree.argumentNames.empty()) {
        extNames.push_back(std::nullopt);
        for (const auto& n : syntaxTree.argumentNames) extNames.push_back(n);
    }
    std::vector<std::optional<syntax::SyntaxToken>> extMods;
    if (!syntaxTree.argumentModifiers.empty()) {
        extMods.push_back(std::nullopt);
        for (const auto& m : syntaxTree.argumentModifiers) extMods.push_back(m);
    }

    bool extAmbiguous = false;
    const auto extPlan = selectFlexibleCall(
        extCandidates,
        extArgs,
        extNames.empty() ? nullptr : &extNames,
        extMods.empty() ? nullptr : &extMods,
        visibleTypes_,
        extAmbiguous);
    if (!extPlan.function || extAmbiguous) {
        receiver = std::move(extArgs.front());
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            arguments[i] = std::move(extArgs[i + 1]);
        }
        return nullptr;
    }

    recordSymbolReference(
        occurrences_,
        currentSourceName_,
        extPlan.function->id,
        SymbolKind::Function,
        extPlan.function->name,
        syntaxTree.nameToken.span);
    if (extMods.empty()) {
        extMods.resize(syntaxTree.arguments.size() + 1);
    }
    if (!extPlan.function->parameters.empty()) {
        const auto recvMod = extPlan.function->parameters.front().modifier;
        if (recvMod == ParameterModifier::Ref) {
            extMods[0] = syntax::SyntaxToken{
                syntax::SyntaxKind::RefKeyword,
                syntaxTree.receiver->span(),
                "ref",
                {},
                true};
        } else if (recvMod == ParameterModifier::In) {
            extMods[0] = syntax::SyntaxToken{
                syntax::SyntaxKind::InKeyword,
                syntaxTree.receiver->span(),
                "in",
                {},
                true};
        }
    }
    std::vector<std::unique_ptr<syntax::ExpressionSyntax>> extSyntaxArgs;
    extSyntaxArgs.reserve(syntaxTree.arguments.size() + 1);
    extSyntaxArgs.push_back(syntax::cloneExpressionSyntax(*syntaxTree.receiver));
    for (const auto& a : syntaxTree.arguments) {
        extSyntaxArgs.push_back(a ? syntax::cloneExpressionSyntax(*a) : nullptr);
    }
    auto prepared = prepareCallArguments(
        *extPlan.function,
        extPlan.sources,
        std::move(extArgs),
        extSyntaxArgs,
        extMods,
        syntaxTree.span(),
        "extension method argument");
    return applyNullConditional(
        bindSelectedCall(
            *extPlan.function,
            std::move(prepared.arguments),
            prepared.syntaxArguments,
            prepared.modifiers,
            std::move(prepared.evaluationOrder),
            nullptr,
            syntaxTree.span(),
            "extension method argument",
            false),
        syntaxTree.dotToken,
        syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindMemberCallExpression(
    const syntax::MemberCallExpressionSyntax& syntaxTree) {
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (const auto& argument : syntaxTree.arguments) {
        arguments.push_back(bindExpression(*argument));
    }

    bool staticCall = false;
    TypeSymbol owner;
    std::unique_ptr<BoundExpression> receiver;
    if (syntaxTree.receiver->kind() ==
        syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<
            const syntax::NameExpressionSyntax&>(
                *syntaxTree.receiver);
        const auto type = visibleTypes_.find(
            name.identifierToken.text);
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
            checkObsoleteReference(
                type->second.id,
                "type",
                canonicalTypeName(type->second),
                name.identifierToken.span);
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                type->second.id,
                SymbolKind::Type,
                type->second.name,
                name.identifierToken.span);
            owner = type->second;
            staticCall = true;
        }
    }
    if (!staticCall) {
        receiver = bindExpression(*syntaxTree.receiver);
        if (receiver->type == PrimitiveType::Array) {
            const auto& methodName = syntaxTree.nameToken.text;
            if ((methodName == "Length" || methodName == "Count") &&
                arguments.empty()) {
                auto result = std::make_unique<BoundArrayLengthExpression>();
                result->span = syntaxTree.span();
                result->type = PrimitiveType::Int;
                result->receiver = std::move(receiver);
                return applyNullConditional(
                    std::move(result), syntaxTree.dotToken, syntaxTree.span());
            }
            PrimitiveType elemType = PrimitiveType::Error;
            std::string elemTypeName;
            if (decodeArrayTypeName(receiver->typeName, elemType, elemTypeName)) {
                if (!elemTypeName.empty()) {
                    const auto foundElem = visibleTypes_.find(elemTypeName);
                    if (foundElem != visibleTypes_.end()) {
                        elemType = foundElem->second.kind == TypeKind::Class
                            ? PrimitiveType::Object
                            : (foundElem->second.kind == TypeKind::Struct
                                ? PrimitiveType::Struct
                                : PrimitiveType::Enum);
                    }
                }
                std::string intrinsicName;
                PrimitiveType returnType = PrimitiveType::Error;
                std::vector<std::pair<PrimitiveType, std::string>> expectedParams;
                if (methodName == "IndexOf" && arguments.size() == 1) {
                    intrinsicName = "$intrinsic::array_index_of::" + receiver->typeName;
                    returnType = PrimitiveType::Int;
                    expectedParams = {{elemType, elemTypeName}};
                } else if (methodName == "LastIndexOf" && arguments.size() == 1) {
                    intrinsicName = "$intrinsic::array_last_index_of::" + receiver->typeName;
                    returnType = PrimitiveType::Int;
                    expectedParams = {{elemType, elemTypeName}};
                } else if (methodName == "Contains" && arguments.size() == 1) {
                    intrinsicName = "$intrinsic::array_contains::" + receiver->typeName;
                    returnType = PrimitiveType::Bool;
                    expectedParams = {{elemType, elemTypeName}};
                } else if (methodName == "Reverse" && arguments.empty()) {
                    intrinsicName = "$intrinsic::array_reverse::" + receiver->typeName;
                    returnType = PrimitiveType::Void;
                } else if (methodName == "Sort" && arguments.empty()) {
                    intrinsicName = "$intrinsic::array_sort::" + receiver->typeName;
                    returnType = PrimitiveType::Void;
                }
                if (!intrinsicName.empty()) {
                    std::vector<std::pair<PrimitiveType, std::string>> fullParams;
                    fullParams.push_back({PrimitiveType::Array, receiver->typeName});
                    for (const auto& p : expectedParams) fullParams.push_back(p);
                    auto call = std::make_unique<BoundCallExpression>();
                    call->span = syntaxTree.span();
                    call->type = returnType;
                    call->function = makeIntrinsicFunction(
                        intrinsicName, returnType, "", fullParams);
                    call->function.method = true;
                    call->function.staticMethod = false;
                    call->arguments.push_back(std::move(receiver));
                    for (std::size_t i = 0; i < arguments.size(); ++i) {
                        call->arguments.push_back(convertExpression(
                            std::move(arguments[i]),
                            expectedParams[i].first,
                            syntaxTree.arguments[i]->span(),
                            "array method argument",
                            expectedParams[i].second));
                    }
                    return applyNullConditional(
                        std::move(call), syntaxTree.dotToken, syntaxTree.span());
                }
            }
        }
        if (receiver->type == PrimitiveType::String) {
            const auto& methodName = syntaxTree.nameToken.text;
            std::string intrinsicName;
            PrimitiveType returnType = PrimitiveType::Error;
            std::string returnTypeName;
            std::vector<std::pair<PrimitiveType, std::string>> expectedParams;
            if ((methodName == "Length" || methodName == "Count") && arguments.empty()) {
                intrinsicName = "$intrinsic::string_length";
                returnType = PrimitiveType::Int;
            } else if (methodName == "IsEmpty" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_is_empty";
                returnType = PrimitiveType::Bool;
            } else if (methodName == "Contains" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_contains";
                returnType = PrimitiveType::Bool;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "StartsWith" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_starts_with";
                returnType = PrimitiveType::Bool;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "EndsWith" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_ends_with";
                returnType = PrimitiveType::Bool;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "IndexOf" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_index_of";
                returnType = PrimitiveType::Int;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "LastIndexOf" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_last_index_of";
                returnType = PrimitiveType::Int;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "Substring" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_substring_1";
                returnType = PrimitiveType::String;
                expectedParams = {{PrimitiveType::Int, ""}};
            } else if (methodName == "Substring" && arguments.size() == 2) {
                intrinsicName = "$intrinsic::string_substring_2";
                returnType = PrimitiveType::String;
                expectedParams = {{PrimitiveType::Int, ""}, {PrimitiveType::Int, ""}};
            } else if (methodName == "Trim" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_trim";
                returnType = PrimitiveType::String;
            } else if (methodName == "TrimStart" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_trim_start";
                returnType = PrimitiveType::String;
            } else if (methodName == "TrimEnd" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_trim_end";
                returnType = PrimitiveType::String;
            } else if (methodName == "Replace" && arguments.size() == 2) {
                intrinsicName = "$intrinsic::string_replace";
                returnType = PrimitiveType::String;
                expectedParams = {{PrimitiveType::String, ""}, {PrimitiveType::String, ""}};
            } else if (methodName == "ToUpper" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_to_upper";
                returnType = PrimitiveType::String;
            } else if (methodName == "ToLower" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_to_lower";
                returnType = PrimitiveType::String;
            } else if (methodName == "Split" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_split";
                returnType = PrimitiveType::Array;
                returnTypeName = arrayTypeName(PrimitiveType::String, "");
                expectedParams = {{PrimitiveType::String, ""}};
            }
            if (!intrinsicName.empty()) {
                std::vector<std::pair<PrimitiveType, std::string>> fullParams;
                fullParams.push_back({PrimitiveType::String, ""});
                for (const auto& p : expectedParams) fullParams.push_back(p);
                auto call = std::make_unique<BoundCallExpression>();
                call->span = syntaxTree.span();
                call->type = returnType;
                call->typeName = returnTypeName;
                call->function = makeIntrinsicFunction(
                    intrinsicName, returnType, returnTypeName, fullParams);
                call->function.method = true;
                call->function.staticMethod = false;
                call->arguments.push_back(std::move(receiver));
                for (std::size_t i = 0; i < arguments.size(); ++i) {
                    call->arguments.push_back(convertExpression(
                        std::move(arguments[i]),
                        expectedParams[i].first,
                        syntaxTree.arguments[i]->span(),
                        "string method argument",
                        expectedParams[i].second));
                }
                return applyNullConditional(
                    std::move(call), syntaxTree.dotToken, syntaxTree.span());
            }
            if (auto extCall = tryBindExtensionMethodCall(syntaxTree, receiver, arguments)) {
                return extCall;
            }
            diagnostics_.report(
                "RS2472",
                "no applicable method '" + methodName + "' on type 'string'",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        if ((receiver->type != PrimitiveType::Object &&
             receiver->type != PrimitiveType::Struct) ||
            receiver->typeName.empty()) {
            if (auto extCall = tryBindExtensionMethodCall(syntaxTree, receiver, arguments)) {
                return extCall;
            }
            diagnostics_.report(
                "RS2471",
                "member call requires a class or struct receiver",
                syntaxTree.receiver->span());
            return makeError(syntaxTree.span());
        }
        const auto type = visibleTypes_.find(
            receiver->typeName);
        if (type == visibleTypes_.end()) {
            diagnostics_.report(
                "RS2405",
                "type descriptor '" + receiver->typeName +
                    "' is unavailable",
                syntaxTree.receiver->span());
            return makeError(syntaxTree.span());
        }
        owner = type->second;
    }

    if (!staticCall && owner.kind == TypeKind::Struct && owner.synthetic &&
        owner.name.rfind("Nullable__", 0) == 0 &&
        syntaxTree.arguments.empty() && owner.fields.size() >= 2) {
        const FieldSymbol* intrinsicField = nullptr;
        if (syntaxTree.nameToken.text == "HasValue") {
            intrinsicField = &owner.fields[0];
        } else if (syntaxTree.nameToken.text == "Value" ||
                   syntaxTree.nameToken.text == "GetValueOrDefault") {
            intrinsicField = &owner.fields[1];
        }
        if (intrinsicField) {
            auto result = std::make_unique<BoundStructFieldAccessExpression>();
            result->span = syntaxTree.span();
            result->type = intrinsicField->type;
            result->typeName = intrinsicField->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = owner;
            result->field = *intrinsicField;
            return result;
        }
    }

    auto methods = findMethods(
        owner, syntaxTree.nameToken.text, staticCall);
    methods.erase(
        std::remove_if(
            methods.begin(), methods.end(),
            [&](const auto* method) {
                return !isMemberAccessible(
                    method->accessibility,
                    method->declaringTypeId,
                    method->moduleName);
            }),
        methods.end());
    bool ambiguous = false;
    const auto plan = selectFlexibleCall(
        methods, arguments, &syntaxTree.argumentNames,
        &syntaxTree.argumentModifiers, visibleTypes_, ambiguous);
    const auto* best = plan.function;
    if (!best && !staticCall && receiver) {
        if (auto extCall = tryBindExtensionMethodCall(syntaxTree, receiver, arguments)) {
            return extCall;
        }
    }
    if (!best) {
        diagnostics_.report(
            "RS2472",
            "no applicable method '" +
                syntaxTree.nameToken.text +
                "' on type '" + canonicalTypeName(owner) + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2473",
            "method call '" + syntaxTree.nameToken.text +
                "' is ambiguous",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    recordSymbolReference(
        occurrences_,
        currentSourceName_,
        best->id,
        SymbolKind::Function,
        best->name,
        syntaxTree.nameToken.span);

    auto prepared = prepareCallArguments(
        *best, plan.sources, std::move(arguments),
        syntaxTree.arguments, syntaxTree.argumentModifiers,
        syntaxTree.span(), "method argument");
    return applyNullConditional(bindSelectedCall(
        *best,
        std::move(prepared.arguments),
        prepared.syntaxArguments,
        prepared.modifiers,
        std::move(prepared.evaluationOrder),
        std::move(receiver),
        syntaxTree.span(),
        "method argument",
        syntaxTree.receiver->kind() ==
            syntax::SyntaxKind::BaseExpression),
        syntaxTree.dotToken, syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindNewObjectExpression(
    const syntax::NewObjectExpressionSyntax& syntaxTree) {
    const auto found = visibleTypes_.find(syntaxTree.type.name.text);
    if (found == visibleTypes_.end() || found->second.kind == TypeKind::Enum) {
        diagnostics_.report("RS2403", "cannot allocate unknown or enum type '" + syntaxTree.type.name.text + "'", syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    checkObsoleteReference(
        found->second.id,
        "type",
        canonicalTypeName(found->second),
        syntaxTree.type.span());
    recordSymbolReference(
        occurrences_,
        currentSourceName_,
        found->second.id,
        SymbolKind::Type,
        found->second.name,
        syntaxTree.type.name.span);
    if (!isTypeAccessible(found->second)) {
        diagnostics_.report(
            "RS2534",
            "type '" + canonicalTypeName(found->second) + "' is inaccessible",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    if (found->second.interfaceType) {
        diagnostics_.report(
            "RS2530",
            "cannot instantiate interface '" +
                canonicalTypeName(found->second) + "'",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    if (found->second.delegateType) {
        diagnostics_.report(
            "RS8807",
            "delegate values must be created from a method group or lambda",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    if (found->second.kind == TypeKind::Class && found->second.staticType) {
        diagnostics_.report(
            "RS2488",
            "cannot instantiate static class '" +
                canonicalTypeName(found->second) + "'",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    if (found->second.kind == TypeKind::Class && found->second.abstractType) {
        diagnostics_.report(
            "RS2524",
            "cannot instantiate abstract class '" +
                canonicalTypeName(found->second) + "'",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (const auto& argument : syntaxTree.arguments) arguments.push_back(bindExpression(*argument));
    std::vector<const FunctionSymbol*> constructors;
    for (const auto& ctor : found->second.constructors) {
        if (isMemberAccessible(
                ctor.accessibility, ctor.declaringTypeId, ctor.moduleName)) {
            constructors.push_back(&ctor);
        }
    }
    bool ambiguous = false;
    const FunctionSymbol* best = nullptr;
    FlexibleCallPlan constructorPlan;
    if (!constructors.empty() || !arguments.empty()) {
        constructorPlan = selectFlexibleCall(
            constructors, arguments, &syntaxTree.argumentNames,
            nullptr, visibleTypes_, ambiguous);
        best = constructorPlan.function;
    }
    const bool implicitStructDefault =
        found->second.kind == TypeKind::Struct && arguments.empty() && !best;
    if ((!arguments.empty() || !found->second.constructors.empty()) && !best &&
        !implicitStructDefault) {
        diagnostics_.report("RS2474", "no applicable constructor for type '" + canonicalTypeName(found->second) + "'", syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report("RS2475", "constructor call is ambiguous", syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    PreparedCallArguments preparedConstructor;
    if (best) {
        checkObsoleteReference(
            best->id,
            "constructor",
            canonicalTypeName(found->second) + "." + best->name,
            syntaxTree.span());
        const std::vector<std::optional<syntax::SyntaxToken>> noModifiers;
        preparedConstructor = prepareCallArguments(
            *best, constructorPlan.sources, std::move(arguments),
            syntaxTree.arguments, noModifiers, syntaxTree.span(),
            "constructor argument");
    }

    const auto bindInitializers = [&]() {
        std::vector<BoundNewObjectExpression::Initializer> initializers;
        std::unordered_set<std::string> initializedMembers;
        for (const auto& syntaxInitializer : syntaxTree.initializers) {
            BoundNewObjectExpression::Initializer initializer;
            if (syntaxInitializer.isMemberInitializer()) {
                const auto& name = syntaxInitializer.nameToken->text;
                if (!initializedMembers.insert(name).second) {
                    diagnostics_.report(
                        "RS8911", "member '" + name +
                            "' is initialized more than once",
                        syntaxInitializer.span());
                    continue;
                }
                if (syntaxInitializer.expressions.size() != 1) continue;
                if (const auto* field = findField(found->second, name)) {
                    if (!isMemberAccessible(
                            field->accessibility, field->declaringTypeId)) {
                        diagnostics_.report(
                            "RS2534", "field '" + name +
                                "' is inaccessible",
                            syntaxInitializer.span());
                        continue;
                    }
                    if (field->constField || field->readonlyField) {
                        diagnostics_.report(
                            "RS2483",
                            std::string("cannot assign to ") +
                                (field->constField ? "const" : "readonly") +
                                " field '" + field->name + "' in object initializer",
                            syntaxInitializer.nameToken->span);
                        continue;
                    }
                    if (field->staticField) {
                        diagnostics_.report(
                            "RS2484",
                            "cannot initialize static field '" + field->name +
                                "' in object initializer",
                            syntaxInitializer.nameToken->span);
                        continue;
                    }
                    checkObsoleteReference(
                        field->id, "field", field->name, syntaxInitializer.nameToken->span);
                    recordSymbolReference(
                        occurrences_,
                        currentSourceName_,
                        field->id,
                        SymbolKind::Field,
                        field->name,
                        syntaxInitializer.nameToken->span);
                    initializer.kind = BoundNewObjectExpression::Initializer::Kind::Field;
                    initializer.field = *field;
                    initializer.arguments.push_back(bindTargetExpression(
                        *syntaxInitializer.expressions.front(),
                        field->type, field->typeName,
                        "object initializer"));
                    initializers.push_back(std::move(initializer));
                    continue;
                }
                if (const auto* property = findProperty(found->second, name)) {
                    if (property->staticProperty) {
                        diagnostics_.report(
                            "RS2484",
                            "cannot initialize static property '" + property->name +
                                "' in object initializer",
                            syntaxInitializer.nameToken->span);
                        continue;
                    }
                    if (!property->setter || !isMemberAccessible(
                            property->accessibility,
                            property->declaringTypeId)) {
                        diagnostics_.report(
                            "RS8912", "property '" + name +
                                "' is not settable here",
                            syntaxInitializer.span());
                        continue;
                    }
                    recordSymbolReference(
                        occurrences_,
                        currentSourceName_,
                        property->id,
                        SymbolKind::Property,
                        property->name,
                        syntaxInitializer.nameToken->span);
                    initializer.kind = BoundNewObjectExpression::Initializer::Kind::Property;
                    initializer.function = *property->setter;
                    initializer.arguments.push_back(bindTargetExpression(
                        *syntaxInitializer.expressions.front(),
                        property->type, property->typeName,
                        "object initializer"));
                    initializers.push_back(std::move(initializer));
                    continue;
                }
                diagnostics_.report(
                    "RS8913", "type '" + canonicalTypeName(found->second) +
                        "' has no initializable member named '" + name + "'",
                    syntaxInitializer.span());
                continue;
            }

            std::vector<std::unique_ptr<BoundExpression>> values;
            for (const auto& expression : syntaxInitializer.expressions) {
                values.push_back(bindExpression(*expression));
            }
            const auto addMethods = findMethods(found->second, "Add", false);
            bool addAmbiguous = false;
            const auto* add = selectBest(
                addMethods, values, nullptr, visibleTypes_, addAmbiguous);
            if (!add || addAmbiguous) {
                diagnostics_.report(
                    addAmbiguous ? "RS8915" : "RS8914",
                    addAmbiguous
                        ? "collection initializer Add call is ambiguous"
                        : "no applicable Add method for collection initializer",
                    syntaxInitializer.span());
                continue;
            }
            initializer.kind = BoundNewObjectExpression::Initializer::Kind::Collection;
            initializer.function = *add;
            const auto addOffset = visibleParameterOffset(*add);
            for (std::size_t index = 0; index < values.size(); ++index) {
                initializer.arguments.push_back(convertExpression(
                    std::move(values[index]),
                    add->parameters[index + addOffset].type,
                    syntaxInitializer.expressions[index]->span(),
                    "collection initializer",
                    add->parameters[index + addOffset].typeName));
            }
            initializers.push_back(std::move(initializer));
        }
        return initializers;
    };
    auto boundInitializers = bindInitializers();

    if (found->second.kind == TypeKind::Struct) {
        auto result = std::make_unique<BoundNewStructExpression>();
        result->span = syntaxTree.span();
        result->type = PrimitiveType::Struct;
        result->typeName = canonicalTypeName(found->second);
        result->structType = found->second;
        if (!best) {
            for (const auto& field : found->second.fields) {
                if (field.staticField || field.constField ||
                    field.hasConstantValue || !field.initializerSyntax) {
                    continue;
                }
                auto initExpr = bindStaticFieldInitializer(found->second, field);
                if (!initExpr) continue;
                BoundNewObjectExpression::Initializer defaultInit;
                defaultInit.kind = BoundNewObjectExpression::Initializer::Kind::Field;
                defaultInit.field = field;
                defaultInit.arguments.push_back(std::move(initExpr));
                result->defaultFieldInitializers.push_back(std::move(defaultInit));
            }
        }
        if (best) result->constructor = *best;
        const auto offset = best ? visibleParameterOffset(*best) : 0;
        for (std::size_t i = 0;
             i < preparedConstructor.arguments.size(); ++i) {
            const auto argumentSpan = preparedConstructor.arguments[i]->span;
            result->arguments.push_back(convertExpression(
                std::move(preparedConstructor.arguments[i]),
                best->parameters[i + offset].type,
                argumentSpan, "constructor argument",
                best->parameters[i + offset].typeName));
        }
        result->argumentEvaluationOrder =
            std::move(preparedConstructor.evaluationOrder);
        result->initializers = std::move(boundInitializers);
        return result;
    }

    auto result = std::make_unique<BoundNewObjectExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Object;
    result->typeName = canonicalTypeName(found->second);
    result->objectType = found->second;
    if (!best) {
        for (const auto& field : found->second.fields) {
            if (field.staticField || field.constField ||
                field.hasConstantValue || !field.initializerSyntax) {
                continue;
            }
            auto initExpr = bindStaticFieldInitializer(found->second, field);
            if (!initExpr) continue;
            BoundNewObjectExpression::Initializer defaultInit;
            defaultInit.kind = BoundNewObjectExpression::Initializer::Kind::Field;
            defaultInit.field = field;
            defaultInit.arguments.push_back(std::move(initExpr));
            result->defaultFieldInitializers.push_back(std::move(defaultInit));
        }
    }
    if (best) result->constructor = *best;
    const auto offset = best ? visibleParameterOffset(*best) : 0;
    for (std::size_t i = 0;
         i < preparedConstructor.arguments.size(); ++i) {
        const auto argumentSpan = preparedConstructor.arguments[i]->span;
        result->arguments.push_back(convertExpression(
            std::move(preparedConstructor.arguments[i]),
            best->parameters[i + offset].type,
            argumentSpan, "constructor argument",
            best->parameters[i + offset].typeName));
    }
    result->argumentEvaluationOrder =
        std::move(preparedConstructor.evaluationOrder);
    result->initializers = std::move(boundInitializers);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindNewArrayExpression(
    const syntax::NewArrayExpressionSyntax& syntaxTree) {
    std::string elementTypeName;
    const auto elementType = bindType(syntaxTree.elementType, false, &elementTypeName);
    if (elementType == PrimitiveType::Error || elementType == PrimitiveType::Void || elementType == PrimitiveType::Array) {
        diagnostics_.report("RS2420", "array element type is not supported", syntaxTree.elementType.span());
        return makeError(syntaxTree.span());
    }
    if (syntaxTree.explicitLength && !syntaxTree.initialValues.empty()) {
        syntax::TokenValue constLen;
        PrimitiveType constLenType = PrimitiveType::Error;
        const bool isConstInt = evaluateConstantSyntaxExpression(
            *syntaxTree.length,
            currentOwnerType_ ? &*currentOwnerType_ : nullptr,
            visibleTypes_,
            constLen,
            constLenType,
            &scopes_) &&
            std::holds_alternative<std::int64_t>(constLen);
        if (!isConstInt ||
            std::get<std::int64_t>(constLen) !=
                static_cast<std::int64_t>(syntaxTree.initialValues.size())) {
            diagnostics_.report(
                "RS2421",
                "array initializer length does not match explicit array size",
                syntaxTree.length->span());
        }
    }
    auto length = convertExpression(bindExpression(*syntaxTree.length), PrimitiveType::Int, syntaxTree.length->span(), "array length");
    auto result = std::make_unique<BoundNewArrayExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Array;
    result->typeName = arrayTypeName(elementType, elementTypeName);
    result->elementType = elementType;
    result->elementTypeName = elementTypeName;
    result->length = std::move(length);
    for (const auto& itemSyntax : syntaxTree.initialValues) {
        if (!itemSyntax) continue;
        result->initialValues.push_back(bindTargetExpression(
            *itemSyntax,
            elementType,
            elementTypeName,
            "array initializer"));
    }
    return result;
}


} // namespace realscript::semantic
