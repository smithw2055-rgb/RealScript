#include "realscript/semantic/Semantic.h"
#include "FlowAnalysis.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace realscript::semantic {
namespace {

bool isLambdaParameter(
    const std::vector<std::unordered_set<std::string>>& parameterScopes,
    const std::string& name) {
    for (auto scope = parameterScopes.rbegin();
         scope != parameterScopes.rend(); ++scope) {
        if (scope->find(name) != scope->end()) return true;
    }
    return false;
}

void collectLambdaCaptureNames(
    const syntax::ExpressionSyntax& expression,
    bool insideLambda,
    std::vector<std::unordered_set<std::string>>& parameterScopes,
    std::unordered_set<std::string>& names) {
    const auto record = [&](const std::string& name) {
        if (insideLambda && !isLambdaParameter(parameterScopes, name)) {
            names.insert(name);
        }
    };
    const auto visit = [&](const auto& expressions) {
        for (const auto& value : expressions) {
            collectLambdaCaptureNames(
                *value, insideLambda, parameterScopes, names);
        }
    };
    switch (expression.kind()) {
    case syntax::SyntaxKind::NameExpression:
        record(static_cast<const syntax::NameExpressionSyntax&>(
            expression).identifierToken.text);
        return;
    case syntax::SyntaxKind::LambdaExpression: {
        const auto& lambda = static_cast<const
            syntax::LambdaExpressionSyntax&>(expression);
        std::unordered_set<std::string> parameters;
        for (const auto& parameter : lambda.parameterTokens) {
            parameters.insert(parameter.text);
        }
        parameterScopes.push_back(std::move(parameters));
        collectLambdaCaptureNames(
            *lambda.body, true, parameterScopes, names);
        parameterScopes.pop_back();
        return;
    }
    case syntax::SyntaxKind::UnaryExpression:
        collectLambdaCaptureNames(*static_cast<const
            syntax::UnaryExpressionSyntax&>(expression).operand,
            insideLambda, parameterScopes, names);
        return;
    case syntax::SyntaxKind::BinaryExpression: {
        const auto& value = static_cast<const
            syntax::BinaryExpressionSyntax&>(expression);
        collectLambdaCaptureNames(
            *value.left, insideLambda, parameterScopes, names);
        collectLambdaCaptureNames(
            *value.right, insideLambda, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::AssignmentExpression: {
        const auto& value = static_cast<const
            syntax::AssignmentExpressionSyntax&>(expression);
        record(value.identifierToken.text);
        collectLambdaCaptureNames(
            *value.expression, insideLambda, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::ParenthesizedExpression:
        collectLambdaCaptureNames(*static_cast<const
            syntax::ParenthesizedExpressionSyntax&>(expression).expression,
            insideLambda, parameterScopes, names);
        return;
    case syntax::SyntaxKind::CastExpression:
        collectLambdaCaptureNames(*static_cast<const
            syntax::CastExpressionSyntax&>(expression).expression,
            insideLambda, parameterScopes, names);
        return;
    case syntax::SyntaxKind::CallExpression: {
        const auto& value = static_cast<const
            syntax::CallExpressionSyntax&>(expression);
        record(value.identifierToken.text);
        visit(value.arguments);
        return;
    }
    case syntax::SyntaxKind::MemberCallExpression: {
        const auto& value = static_cast<const
            syntax::MemberCallExpressionSyntax&>(expression);
        collectLambdaCaptureNames(
            *value.receiver, insideLambda, parameterScopes, names);
        visit(value.arguments);
        return;
    }
    case syntax::SyntaxKind::MemberAccessExpression:
        collectLambdaCaptureNames(*static_cast<const
            syntax::MemberAccessExpressionSyntax&>(expression).receiver,
            insideLambda, parameterScopes, names);
        return;
    case syntax::SyntaxKind::ElementAccessExpression: {
        const auto& value = static_cast<const
            syntax::ElementAccessExpressionSyntax&>(expression);
        collectLambdaCaptureNames(
            *value.receiver, insideLambda, parameterScopes, names);
        collectLambdaCaptureNames(
            *value.index, insideLambda, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::ElementAssignmentExpression: {
        const auto& value = static_cast<const
            syntax::ElementAssignmentExpressionSyntax&>(expression);
        collectLambdaCaptureNames(
            *value.receiver, insideLambda, parameterScopes, names);
        collectLambdaCaptureNames(
            *value.index, insideLambda, parameterScopes, names);
        collectLambdaCaptureNames(
            *value.expression, insideLambda, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::MemberAssignmentExpression: {
        const auto& value = static_cast<const
            syntax::MemberAssignmentExpressionSyntax&>(expression);
        collectLambdaCaptureNames(
            *value.receiver, insideLambda, parameterScopes, names);
        collectLambdaCaptureNames(
            *value.expression, insideLambda, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::NewObjectExpression:
        visit(static_cast<const syntax::NewObjectExpressionSyntax&>(
            expression).arguments);
        return;
    case syntax::SyntaxKind::NewArrayExpression:
        collectLambdaCaptureNames(*static_cast<const
            syntax::NewArrayExpressionSyntax&>(expression).length,
            insideLambda, parameterScopes, names);
        return;
    default:
        return;
    }
}

void collectLambdaCaptureNames(
    const syntax::StatementSyntax& statement,
    std::vector<std::unordered_set<std::string>>& parameterScopes,
    std::unordered_set<std::string>& names) {
    const auto expression = [&](const auto& value) {
        if (value) collectLambdaCaptureNames(
            *value, false, parameterScopes, names);
    };
    switch (statement.kind()) {
    case syntax::SyntaxKind::BlockStatement:
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(statement).statements) {
            collectLambdaCaptureNames(*child, parameterScopes, names);
        }
        return;
    case syntax::SyntaxKind::ReturnStatement:
        expression(static_cast<const
            syntax::ReturnStatementSyntax&>(statement).expression);
        return;
    case syntax::SyntaxKind::IfStatement: {
        const auto& value = static_cast<const
            syntax::IfStatementSyntax&>(statement);
        expression(value.condition);
        collectLambdaCaptureNames(
            *value.thenStatement, parameterScopes, names);
        if (value.elseStatement) collectLambdaCaptureNames(
            *value.elseStatement, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::WhileStatement: {
        const auto& value = static_cast<const
            syntax::WhileStatementSyntax&>(statement);
        expression(value.condition);
        collectLambdaCaptureNames(*value.body, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::ForStatement: {
        const auto& value = static_cast<const
            syntax::ForStatementSyntax&>(statement);
        if (value.initializer) collectLambdaCaptureNames(
            *value.initializer, parameterScopes, names);
        expression(value.condition);
        expression(value.increment);
        collectLambdaCaptureNames(*value.body, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::ForeachStatement: {
        const auto& value = static_cast<const
            syntax::ForeachStatementSyntax&>(statement);
        expression(value.collection);
        collectLambdaCaptureNames(*value.body, parameterScopes, names);
        return;
    }
    case syntax::SyntaxKind::DoWhileStatement: {
        const auto& value = static_cast<const
            syntax::DoWhileStatementSyntax&>(statement);
        collectLambdaCaptureNames(*value.body, parameterScopes, names);
        expression(value.condition);
        return;
    }
    case syntax::SyntaxKind::SwitchStatement: {
        const auto& value = static_cast<const
            syntax::SwitchStatementSyntax&>(statement);
        expression(value.expression);
        for (const auto& section : value.sections) {
            expression(section.label);
            for (const auto& child : section.statements) {
                collectLambdaCaptureNames(
                    *child, parameterScopes, names);
            }
        }
        return;
    }
    case syntax::SyntaxKind::YieldWaitStatement:
        expression(static_cast<const
            syntax::YieldWaitStatementSyntax&>(statement).delay);
        return;
    case syntax::SyntaxKind::EventSubscriptionStatement: {
        const auto& value = static_cast<const
            syntax::EventSubscriptionStatementSyntax&>(statement);
        expression(value.receiver);
        expression(value.handler);
        return;
    }
    case syntax::SyntaxKind::VariableDeclarationStatement:
        expression(static_cast<const
            syntax::VariableDeclarationStatementSyntax&>(statement).initializer);
        return;
    case syntax::SyntaxKind::ExpressionStatement:
        expression(static_cast<const
            syntax::ExpressionStatementSyntax&>(statement).expression);
        return;
    default:
        return;
    }
}

} // namespace

Binder::Binder(diagnostics::DiagnosticBag& diagnostics)
    : diagnostics_(diagnostics) {}

SemanticModel Binder::bind(const syntax::CompilationUnitSyntax& syntaxTree) {
    ModuleBindingInput input;
    input.moduleName = syntaxTree.moduleDeclaration
        ? syntaxTree.moduleDeclaration->fullName()
        : "";
    input.units = {&syntaxTree};

    for (const auto& classSyntax : syntaxTree.classes) {
        auto type = declareTypeShell(input.moduleName, classSyntax);
        input.visibleTypes[type.name] = type;
        input.visibleTypes[canonicalTypeName(type)] = type;
        input.types.push_back(type);
    }
    for (const auto& structSyntax : syntaxTree.structs) {
        auto type = declareTypeShell(input.moduleName, structSyntax);
        input.visibleTypes[type.name] = type;
        input.visibleTypes[canonicalTypeName(type)] = type;
        input.types.push_back(type);
    }
    for (const auto& enumSyntax : syntaxTree.enums) {
        auto type = declareTypeShell(input.moduleName, enumSyntax);
        input.visibleTypes[type.name] = type;
        input.visibleTypes[canonicalTypeName(type)] = type;
        input.types.push_back(type);
    }

    std::size_t typeIndex = 0;
    for (const auto& classSyntax : syntaxTree.classes) {
        (void)populateTypeFields(input.types[typeIndex], classSyntax, input.visibleTypes, diagnostics_);
        input.visibleTypes[input.types[typeIndex].name] = input.types[typeIndex];
        input.visibleTypes[canonicalTypeName(input.types[typeIndex])] = input.types[typeIndex];
        ++typeIndex;
    }
    for (const auto& structSyntax : syntaxTree.structs) {
        (void)populateTypeFields(input.types[typeIndex], structSyntax, input.visibleTypes, diagnostics_);
        input.visibleTypes[input.types[typeIndex].name] = input.types[typeIndex];
        input.visibleTypes[canonicalTypeName(input.types[typeIndex])] = input.types[typeIndex];
        ++typeIndex;
    }
    for (const auto& enumSyntax : syntaxTree.enums) {
        (void)populateEnumMembers(input.types[typeIndex], enumSyntax, diagnostics_);
        input.visibleTypes[input.types[typeIndex].name] = input.types[typeIndex];
        input.visibleTypes[canonicalTypeName(input.types[typeIndex])] = input.types[typeIndex];
        ++typeIndex;
    }

    std::unordered_set<std::string> functionKeys;
    auto addBinding = [&](FunctionBindingInput binding) {
        const auto key = canonicalFunctionKey(binding.symbol);
        if (!functionKeys.insert(key).second) {
            diagnostics_.report("RS2000", "function overload '" + key + "' is already declared", {});
        }
        input.visibleFunctions[binding.symbol.name].push_back(binding.symbol);
        input.declarations.push_back(binding.symbol);
        input.functionBindings.push_back(std::move(binding));
    };

    for (const auto& functionSyntax : syntaxTree.functions) {
        FunctionBindingInput binding;
        binding.symbol = declareFunctionSymbol(input.moduleName, functionSyntax, input.visibleTypes, diagnostics_);
        binding.body = &functionSyntax.body;
        for (const auto& parameter : functionSyntax.parameters) {
            binding.parameterNames.push_back(parameter.identifierToken.text);
            binding.parameterSpans.push_back(parameter.identifierToken.span);
        }
        addBinding(std::move(binding));
    }

    auto addMembers = [&](auto const& declarations) {
        for (const auto& typeSyntax : declarations) {
            const auto found = input.visibleTypes.find(typeSyntax.identifierToken.text);
            if (found == input.visibleTypes.end()) continue;
            auto owner = found->second;
            for (const auto& methodSyntax : typeSyntax.methods) {
                FunctionBindingInput binding;
                binding.symbol = declareFunctionSymbol(input.moduleName, methodSyntax, input.visibleTypes, diagnostics_, &owner);
                binding.body = &methodSyntax.body;
                if (!binding.symbol.staticMethod) {
                    binding.parameterNames.push_back("this");
                    binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                }
                for (const auto& parameter : methodSyntax.parameters) {
                    binding.parameterNames.push_back(parameter.identifierToken.text);
                    binding.parameterSpans.push_back(parameter.identifierToken.span);
                }
                owner.methods.push_back(binding.symbol);
                addBinding(std::move(binding));
            }
            for (const auto& ctorSyntax : typeSyntax.constructors) {
                FunctionBindingInput binding;
                binding.symbol = declareConstructorSymbol(input.moduleName, ctorSyntax, owner, input.visibleTypes, diagnostics_);
                binding.body = &ctorSyntax.body;
                binding.constructorSyntax = &ctorSyntax;
                binding.parameterNames.push_back("this");
                binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                for (const auto& parameter : ctorSyntax.parameters) {
                    binding.parameterNames.push_back(parameter.identifierToken.text);
                    binding.parameterSpans.push_back(parameter.identifierToken.span);
                }
                owner.constructors.push_back(binding.symbol);
                addBinding(std::move(binding));
            }
            for (const auto& propertySyntax : typeSyntax.properties) {
                auto property = declarePropertySymbol(input.moduleName, propertySyntax, owner, input.visibleTypes, diagnostics_);
                const bool autoProperty =
                    (propertySyntax.getter && propertySyntax.getter->semicolonToken) ||
                    (propertySyntax.setter && propertySyntax.setter->semicolonToken);
                if (autoProperty) {
                    property.backingFieldIndex = owner.fields.size();
                    FieldSymbol backing;
                    backing.name = "$" + property.name;
                    backing.type = property.type;
                    backing.typeName = property.typeName;
                    backing.index = owner.fields.size();
                    backing.synthetic = true;
                    backing.declarationSpan = propertySyntax.identifierToken.span;
                    backing.id = stableTypeId(canonicalTypeName(owner) +
                        "::field:" + backing.name);
                    owner.fields.push_back(std::move(backing));
                }
                if (property.getter) {
                    FunctionBindingInput binding;
                    binding.symbol = *property.getter;
                    binding.body = propertySyntax.getter->body.get();
                    if (!binding.symbol.staticMethod) {
                        binding.parameterNames.push_back("this");
                        binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                    }
                    binding.syntheticAutoGetter = propertySyntax.getter->semicolonToken.has_value();
                    if (autoProperty) binding.syntheticField = owner.fields[property.backingFieldIndex];
                    addBinding(std::move(binding));
                }
                if (property.setter) {
                    FunctionBindingInput binding;
                    binding.symbol = *property.setter;
                    binding.body = propertySyntax.setter->body.get();
                    if (!binding.symbol.staticMethod) {
                        binding.parameterNames.push_back("this");
                        binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                    }
                    binding.parameterNames.push_back("value");
                    binding.parameterSpans.push_back(propertySyntax.identifierToken.span);
                    binding.syntheticAutoSetter = propertySyntax.setter->semicolonToken.has_value();
                    if (autoProperty) binding.syntheticField = owner.fields[property.backingFieldIndex];
                    addBinding(std::move(binding));
                }
                owner.properties.push_back(std::move(property));
            }
            for (auto& type : input.types) {
                if (type.id == owner.id) type = owner;
            }
            input.visibleTypes[owner.name] = owner;
            input.visibleTypes[canonicalTypeName(owner)] = owner;
        }
    };
    addMembers(syntaxTree.classes);
    addMembers(syntaxTree.structs);

    return bindModule(input);
}

SemanticModel Binder::bindModule(const ModuleBindingInput& input) {
    visibleFunctions_ = input.visibleFunctions;
    occurrences_.clear();
    visibleTypes_ = input.visibleTypes;
    pendingFunctionBindings_.clear();
    pendingTypes_.clear();

    SemanticModel result;
    result.moduleName = input.moduleName;
    result.types = input.types;

    if (!input.functionBindings.empty()) {
        for (const auto& binding : input.functionBindings) {
            result.functions.push_back(bindFunction(binding));
        }
        for (std::size_t index = 0;
             index < pendingFunctionBindings_.size(); ++index) {
            const auto binding = pendingFunctionBindings_[index];
            result.functions.push_back(bindFunction(binding));
        }
        result.types.insert(
            result.types.end(),
            pendingTypes_.begin(), pendingTypes_.end());
        result.occurrences = occurrences_;
        return result;
    }

    std::size_t declarationIndex = 0;
    for (const auto* unit : input.units) {
        for (const auto& functionSyntax : unit->functions) {
            if (declarationIndex >= input.declarations.size()) {
                diagnostics_.report("RS2009", "function declaration table is incomplete", functionSyntax.identifierToken.span);
                continue;
            }
            FunctionBindingInput binding;
            binding.symbol = input.declarations[declarationIndex++];
            binding.body = &functionSyntax.body;
            for (const auto& parameter : functionSyntax.parameters) {
                binding.parameterNames.push_back(parameter.identifierToken.text);
                binding.parameterSpans.push_back(parameter.identifierToken.span);
            }
            result.functions.push_back(bindFunction(binding));
        }
    }
    result.occurrences = occurrences_;
    return result;
}

BoundFunction Binder::bindFunction(const FunctionBindingInput& input) {
    BoundFunction result;
    result.symbol = input.symbol;

    scopes_.clear();
    referenceAliasScopes_.clear();
    scopeSpans_.clear();
    allVariables_.clear();
    pushScope(input.body ? input.body->span() : input.symbol.bodySpan);
    currentSourceName_ = input.sourceName.empty()
        ? input.symbol.sourceName
        : input.sourceName;
    currentFunctionId_ = input.symbol.id;
    currentModuleName_ = input.symbol.moduleName;
    nextLambdaOrdinal_ = 0;
    capturedVariableNames_.clear();
    sequenceLocalFields_.clear();
    for (const auto& entry : input.sequenceParameterFields) {
        sequenceLocalFields_[entry.first] = entry.second;
    }
    for (const auto& entry : input.sequenceLocalFields) {
        sequenceLocalFields_[entry.first] = entry.second;
    }
    if (input.body) {
        std::vector<std::unordered_set<std::string>> parameterScopes;
        collectLambdaCaptureNames(
            *input.body, parameterScopes, capturedVariableNames_);
    }
    loopDepth_ = 0;
    breakableDepth_ = 0;
    checkedArithmetic_ = true;
    result.symbol.sourceName = currentSourceName_;
    currentReturnType_ = input.symbol.returnType;
    currentReturnTypeName_ = input.symbol.returnTypeName;
    currentReturnModifier_ = input.symbol.returnModifier;
    currentStorageReturnType_ = storageReturnTypeOf(input.symbol);
    currentStorageReturnTypeName_ = storageReturnTypeNameOf(input.symbol);
    nextVariableIndex_ = input.symbol.parameters.size();
    currentStaticMethod_ = input.symbol.staticMethod;
    currentConstructor_ = input.symbol.constructor;
    currentOwnerType_.reset();
    if (!input.symbol.ownerTypeName.empty()) {
        const auto found = visibleTypes_.find(input.symbol.ownerTypeName);
        if (found != visibleTypes_.end()) currentOwnerType_ = found->second;
    }

    std::vector<std::unique_ptr<BoundStatement>>
        capturedParameterInitializers;
    for (std::size_t i = 0; i < input.symbol.parameters.size(); ++i) {
        auto parameter = input.symbol.parameters[i];
        if (i < input.parameterNames.size()) parameter.name = input.parameterNames[i];
        const auto span = i < input.parameterSpans.size() ? input.parameterSpans[i] : text::TextSpan{};
        parameter.declarationSpan = span;
        if (parameter.id == 0) {
            parameter.id = stableTypeId(std::to_string(currentFunctionId_) +
                "::local:" + std::to_string(parameter.index) + ":" + parameter.name);
        }
        const bool captured = parameter.name != "this" &&
            parameter.modifier == ParameterModifier::None &&
            capturedVariableNames_.find(parameter.name) !=
                capturedVariableNames_.end();
        if (captured) {
            const auto sourceName = parameter.name;
            auto rawParameter = parameter;
            rawParameter.name = "$capture_input_" + sourceName;
            rawParameter.id = stableTypeId(
                std::to_string(currentFunctionId_) + "::parameter:" +
                std::to_string(parameter.index) + ":" + sourceName);
            (void)declareVariable(rawParameter, span);

            auto capturedParameter = parameter;
            capturedParameter.parameter = false;
            capturedParameter.index = nextVariableIndex_++;
            const auto storage = ensureCaptureStorage(capturedParameter);
            capturedParameter.storageType = PrimitiveType::Object;
            capturedParameter.storageTypeName = canonicalTypeName(storage);
            const auto cellSpan = input.body
                ? text::TextSpan{input.body->span().start, 0}
                : span;
            capturedParameter.declarationSpan = cellSpan;
            (void)declareVariable(capturedParameter, cellSpan);

            auto declaration =
                std::make_unique<BoundVariableDeclarationStatement>();
            declaration->span = cellSpan;
            declaration->variable = capturedParameter;
            auto initializer =
                std::make_unique<BoundVariableExpression>();
            initializer->span = span;
            initializer->type = rawParameter.type;
            initializer->typeName = rawParameter.typeName;
            initializer->variable = rawParameter;
            declaration->initializer = std::move(initializer);
            capturedParameterInitializers.push_back(
                std::move(declaration));
        } else {
            (void)declareVariable(parameter, span);
        }
        result.symbol.parameters[i].name = parameter.name;
    }

    if (input.syntheticAutoGetter || input.syntheticAutoSetter) {
        result.body = std::make_unique<BoundBlockStatement>();
        if (!currentOwnerType_ || input.symbol.staticMethod) {
            diagnostics_.report("RS2462", "auto properties require an instance owner", {});
        } else {
            const auto* thisVariable = lookupVariable("this");
            if (input.syntheticAutoGetter) {
                auto receiver = std::make_unique<BoundVariableExpression>();
                receiver->type = thisVariable->type;
                receiver->typeName = thisVariable->typeName;
                receiver->variable = *thisVariable;
                std::unique_ptr<BoundExpression> access;
                if (currentOwnerType_->kind == TypeKind::Struct) {
                    auto structureAccess = std::make_unique<BoundStructFieldAccessExpression>();
                    structureAccess->type = input.syntheticField.type;
                    structureAccess->typeName = input.syntheticField.typeName;
                    structureAccess->receiver = std::move(receiver);
                    structureAccess->ownerType = *currentOwnerType_;
                    structureAccess->field = input.syntheticField;
                    access = std::move(structureAccess);
                } else {
                    auto memberAccess = std::make_unique<BoundMemberAccessExpression>();
                    memberAccess->type = input.syntheticField.type;
                    memberAccess->typeName = input.syntheticField.typeName;
                    memberAccess->receiver = std::move(receiver);
                    memberAccess->ownerType = *currentOwnerType_;
                    memberAccess->field = input.syntheticField;
                    access = std::move(memberAccess);
                }
                auto statement = std::make_unique<BoundReturnStatement>();
                statement->expression = std::move(access);
                result.body->statements.push_back(std::move(statement));
            } else {
                const auto* valueVariable = lookupVariable("value");
                if (currentOwnerType_->kind == TypeKind::Struct) {
                    diagnostics_.report(
                        "RS2486",
                        "struct properties cannot declare setters in the Phase 3E value model",
                        {});
                } else {
                    auto receiver = std::make_unique<BoundVariableExpression>();
                    receiver->type = thisVariable->type;
                    receiver->typeName = thisVariable->typeName;
                    receiver->variable = *thisVariable;
                    auto assignment = std::make_unique<BoundMemberAssignmentExpression>();
                    assignment->type = input.syntheticField.type;
                    assignment->typeName = input.syntheticField.typeName;
                    assignment->receiver = std::move(receiver);
                    assignment->ownerType = *currentOwnerType_;
                    assignment->field = input.syntheticField;
                    auto value = std::make_unique<BoundVariableExpression>();
                    value->type = valueVariable->type;
                    value->typeName = valueVariable->typeName;
                    value->variable = *valueVariable;
                    assignment->expression = std::move(value);
                    auto statement = std::make_unique<BoundExpressionStatement>();
                    statement->expression = std::move(assignment);
                    result.body->statements.push_back(std::move(statement));
                }
            }
        }
    } else if (input.eventLambda) {
        result.body = std::make_unique<BoundBlockStatement>();
        result.body->span = input.eventLambda->span();
        if (input.symbol.returnType == PrimitiveType::Void) {
            auto statement = std::make_unique<BoundExpressionStatement>();
            statement->span = input.eventLambda->body->span();
            statement->expression = bindExpression(
                *input.eventLambda->body);
            result.body->statements.push_back(std::move(statement));
        } else {
            auto statement = std::make_unique<BoundReturnStatement>();
            statement->span = input.eventLambda->body->span();
            statement->expression = bindTargetExpression(
                *input.eventLambda->body,
                input.symbol.returnType,
                input.symbol.returnTypeName,
                "lambda return value");
            result.body->statements.push_back(std::move(statement));
        }
    } else if (input.sequenceCancellation) {
        result.body = bindSequenceCancellation(input);
    } else if (input.sequence) {
        result.body = bindSequenceSegment(input);
    } else if (input.body) {
        auto initializer = bindBaseConstructorInitializer(input);
        result.body = bindBlockStatement(*input.body, false);
        if (initializer) {
            result.body->statements.insert(
                result.body->statements.begin(), std::move(initializer));
        }
    } else {
        result.body = std::make_unique<BoundBlockStatement>();
    }
    if (input.sequence && input.sequenceSegment == 0 &&
        input.sequenceParameterFields.size() > 1 && currentOwnerType_) {
        std::vector<std::unique_ptr<BoundStatement>> parameterStores;
        for (std::size_t index = 1;
             index < input.sequenceParameterFields.size(); ++index) {
            const auto& [name, field] = input.sequenceParameterFields[index];
            const auto* parameter = lookupVariable(name);
            const auto* receiver = lookupVariable("this");
            if (!parameter || !receiver) continue;
            auto assignment =
                std::make_unique<BoundMemberAssignmentExpression>();
            assignment->type = field.type;
            assignment->typeName = field.typeName;
            assignment->span = field.declarationSpan;
            assignment->receiver = makeVariableAccess(
                *receiver, field.declarationSpan);
            assignment->ownerType = *currentOwnerType_;
            assignment->field = field;
            assignment->expression = makeVariableAccess(
                *parameter, field.declarationSpan);
            auto statement = std::make_unique<BoundExpressionStatement>();
            statement->span = field.declarationSpan;
            statement->expression = std::move(assignment);
            parameterStores.push_back(std::move(statement));
        }
        result.body->statements.insert(
            result.body->statements.begin(),
            std::make_move_iterator(parameterStores.begin()),
            std::make_move_iterator(parameterStores.end()));
    }
    if (!capturedParameterInitializers.empty()) {
        auto insertion = result.body->statements.begin();
        if (input.constructorSyntax &&
            input.constructorSyntax->baseKeyword &&
            insertion != result.body->statements.end()) {
            ++insertion;
        }
        result.body->statements.insert(
            insertion,
            std::make_move_iterator(
                capturedParameterInitializers.begin()),
            std::make_move_iterator(
                capturedParameterInitializers.end()));
    }
    if (input.symbol.constructor &&
        input.symbol.returnType == PrimitiveType::Struct) {
        const auto* thisVariable = lookupVariable("this");
        auto statement = std::make_unique<BoundReturnStatement>();
        auto expression = std::make_unique<BoundVariableExpression>();
        expression->type = thisVariable->type;
        expression->typeName = thisVariable->typeName;
        expression->variable = *thisVariable;
        statement->expression = std::move(expression);
        result.body->statements.push_back(std::move(statement));
    }
    result.variableCount = nextVariableIndex_;
    result.variables = allVariables_;

    if (result.symbol.returnType != PrimitiveType::Void &&
        result.symbol.returnType != PrimitiveType::Error &&
        detail::canReachFunctionEnd(result, diagnostics_)) {
        diagnostics_.report(
            "RS2001",
            "not all control-flow paths in function '" + result.symbol.name + "' return a value",
            {});
    }

    popScope();
    currentOwnerType_.reset();
    currentStaticMethod_ = false;
    currentConstructor_ = false;
    currentModuleName_.clear();
    currentSourceName_.clear();
    currentFunctionId_ = 0;
    currentReturnModifier_ = ParameterModifier::None;
    currentStorageReturnType_ = PrimitiveType::Error;
    currentStorageReturnTypeName_.clear();
    allVariables_.clear();
    sequenceLocalFields_.clear();
    return result;
}

std::unique_ptr<BoundExpression> Binder::makeVariableAccess(
    const VariableSymbol& variable,
    text::TextSpan span) {
    if (storageTypeOf(variable) != variable.type) {
        const auto wrapper = visibleTypes_.find(
            storageTypeNameOf(variable));
        if (wrapper != visibleTypes_.end() &&
            !wrapper->second.fields.empty()) {
            auto storage = std::make_unique<BoundVariableExpression>();
            storage->type = storageTypeOf(variable);
            storage->typeName = storageTypeNameOf(variable);
            storage->variable = variable;
            storage->variable.type = storage->type;
            storage->variable.typeName = storage->typeName;
            storage->span = span;

            auto result = std::make_unique<BoundMemberAccessExpression>();
            result->type = variable.type;
            result->typeName = variable.typeName;
            result->receiver = std::move(storage);
            result->ownerType = wrapper->second;
            result->field = wrapper->second.fields.front();
            result->span = span;
            return result;
        }
    }
    auto result = std::make_unique<BoundVariableExpression>();
    result->type = variable.type;
    result->typeName = variable.typeName;
    result->variable = variable;
    result->span = span;
    return result;
}

std::unique_ptr<BoundExpression> Binder::makeSequenceFieldAccess(
    const FieldSymbol& field,
    text::TextSpan span) {
    if (!currentOwnerType_) return makeError(span);
    const auto* receiver = lookupVariable("this");
    if (!receiver) return makeError(span);
    if (currentOwnerType_->kind == TypeKind::Struct) {
        auto result = std::make_unique<BoundStructFieldAccessExpression>();
        result->type = field.type;
        result->typeName = field.typeName;
        result->span = span;
        result->receiver = makeVariableAccess(*receiver, span);
        result->ownerType = *currentOwnerType_;
        result->field = field;
        return result;
    }
    auto result = std::make_unique<BoundMemberAccessExpression>();
    result->type = field.type;
    result->typeName = field.typeName;
    result->span = span;
    result->receiver = makeVariableAccess(*receiver, span);
    result->ownerType = *currentOwnerType_;
    result->field = field;
    return result;
}

const FunctionSymbol* Binder::findScheduleFunction() const noexcept {
    const auto found = visibleFunctions_.find("Schedule");
    if (found == visibleFunctions_.end()) return nullptr;
    for (const auto& candidate : found->second) {
        if (candidate.method || candidate.parameters.size() != 3 ||
            candidate.parameters[0].type != PrimitiveType::Long ||
            candidate.parameters[1].type != PrimitiveType::String ||
            candidate.parameters[2].type != PrimitiveType::Int) {
            continue;
        }
        return &candidate;
    }
    return nullptr;
}

void Binder::appendSequenceRestartCancellation(
    BoundBlockStatement& result,
    const FunctionBindingInput& input) {
    const auto found = visibleFunctions_.find("CancelTimer");
    if (found == visibleFunctions_.end()) return;
    const FunctionSymbol* cancel = nullptr;
    for (const auto& candidate : found->second) {
        if (!candidate.method &&
            candidate.returnType == PrimitiveType::Bool &&
            candidate.parameters.size() == 1 &&
            candidate.parameters.front().type == PrimitiveType::Long) {
            cancel = &candidate;
            break;
        }
    }
    if (!cancel) return;
    auto call = std::make_unique<BoundCallExpression>();
    call->type = PrimitiveType::Bool;
    call->function = *cancel;
    call->span = input.sequence
        ? input.sequence->identifierToken.span
        : input.symbol.declarationSpan;
    call->arguments.push_back(makeSequenceFieldAccess(
        input.sequenceTimerField, call->span));
    auto statement = std::make_unique<BoundExpressionStatement>();
    statement->span = call->span;
    statement->expression = std::move(call);
    result.statements.push_back(std::move(statement));
}

std::unique_ptr<BoundBlockStatement> Binder::bindSequenceCancellation(
    const FunctionBindingInput& input) {
    auto result = std::make_unique<BoundBlockStatement>();
    result->span = input.symbol.bodySpan;
    const FunctionSymbol* cancel = nullptr;
    const auto found = visibleFunctions_.find("CancelTimer");
    if (found != visibleFunctions_.end()) {
        for (const auto& candidate : found->second) {
            if (!candidate.method && candidate.returnType == PrimitiveType::Bool &&
                candidate.parameters.size() == 1 &&
                candidate.parameters.front().type == PrimitiveType::Long) {
                cancel = &candidate;
                break;
            }
        }
    }
    if (!cancel) {
        diagnostics_.report(
            "RS8804",
            "sequence cancellation requires imported RealScript.Game.CancelTimer",
            input.symbol.declarationSpan);
        return result;
    }
    auto call = std::make_unique<BoundCallExpression>();
    call->type = PrimitiveType::Bool;
    call->function = *cancel;
    call->span = input.symbol.declarationSpan;
    call->arguments.push_back(makeSequenceFieldAccess(
        input.sequenceTimerField, input.symbol.declarationSpan));

    VariableSymbol cancelled;
    cancelled.name = "$sequence_cancelled";
    cancelled.type = PrimitiveType::Bool;
    cancelled.index = nextVariableIndex_++;
    cancelled.declarationSpan = input.symbol.bodySpan;
    cancelled.id = stableTypeId(
        std::to_string(currentFunctionId_) + "::local:" +
        std::to_string(cancelled.index) + ":" + cancelled.name);
    (void)declareVariable(cancelled, input.symbol.bodySpan);
    auto declaration = std::make_unique<BoundVariableDeclarationStatement>();
    declaration->span = input.symbol.declarationSpan;
    declaration->variable = cancelled;
    declaration->initializer = std::move(call);
    result->statements.push_back(std::move(declaration));

    const auto assignField = [&](const FieldSymbol& field,
                                 std::int64_t value) {
        auto assignment =
            std::make_unique<BoundMemberAssignmentExpression>();
        assignment->type = field.type;
        assignment->typeName = field.typeName;
        assignment->span = input.symbol.declarationSpan;
        const auto* receiver = lookupVariable("this");
        assignment->receiver = receiver
            ? makeVariableAccess(*receiver, input.symbol.declarationSpan)
            : makeError(input.symbol.declarationSpan);
        assignment->ownerType = *currentOwnerType_;
        assignment->field = field;
        auto literal = std::make_unique<BoundLiteralExpression>();
        literal->type = field.type;
        if (field.type == PrimitiveType::Bool) {
            literal->value = value != 0;
        } else {
            literal->value = value;
        }
        literal->span = input.symbol.declarationSpan;
        assignment->expression = std::move(literal);
        auto statement = std::make_unique<BoundExpressionStatement>();
        statement->span = input.symbol.declarationSpan;
        statement->expression = std::move(assignment);
        result->statements.push_back(std::move(statement));
    };
    assignField(input.sequenceStateField, 0);
    assignField(input.sequenceTimerField, 0);
    assignField(input.sequenceCompletedField, 1);

    auto statement = std::make_unique<BoundReturnStatement>();
    statement->span = input.symbol.declarationSpan;
    statement->expression = makeVariableAccess(
        cancelled, input.symbol.declarationSpan);
    result->statements.push_back(std::move(statement));
    return result;
}

std::unique_ptr<BoundBlockStatement>
Binder::bindSingleYieldLoopSequence(const FunctionBindingInput& input) {
    auto result = std::make_unique<BoundBlockStatement>();
    result->span = input.sequence ? input.sequence->body.span() : text::TextSpan{};
    if (!input.sequence) return result;

    const syntax::WhileStatementSyntax* loop = nullptr;
    std::size_t loopIndex = 0;
    for (std::size_t index = 0;
         index < input.sequence->body.statements.size(); ++index) {
        if (input.sequence->body.statements[index]->kind() ==
            syntax::SyntaxKind::WhileStatement) {
            loop = static_cast<const syntax::WhileStatementSyntax*>(
                input.sequence->body.statements[index].get());
            loopIndex = index;
            break;
        }
    }
    if (!loop || loop->body->kind() != syntax::SyntaxKind::BlockStatement) {
        diagnostics_.report(
            "RS8802", "invalid single-yield sequence loop",
            input.sequence->identifierToken.span);
        return result;
    }
    const auto& body = static_cast<const syntax::BlockStatementSyntax&>(
        *loop->body);
    std::size_t yieldIndex = body.statements.size();
    for (std::size_t index = 0; index < body.statements.size(); ++index) {
        if (body.statements[index]->kind() ==
            syntax::SyntaxKind::YieldWaitStatement) {
            yieldIndex = index;
            break;
        }
    }
    if (yieldIndex == body.statements.size() ||
        !input.sequenceNextCallback) {
        diagnostics_.report(
            "RS8802", "sequence loop continuation is incomplete",
            input.sequence->identifierToken.span);
        return result;
    }

    if (input.sequenceSegment == 0) {
        appendSequenceRestartCancellation(*result, input);
        const auto* receiver = lookupVariable("this");
        const auto* target = input.sequence->parameters.empty()
            ? nullptr
            : lookupVariable(
                input.sequence->parameters.front().identifierToken.text);
        if (receiver && target && currentOwnerType_) {
            auto assignment =
                std::make_unique<BoundMemberAssignmentExpression>();
            assignment->type = input.sequenceTargetField.type;
            assignment->typeName = input.sequenceTargetField.typeName;
            assignment->span = input.sequence->identifierToken.span;
            assignment->receiver = makeVariableAccess(
                *receiver, input.sequence->identifierToken.span);
            assignment->ownerType = *currentOwnerType_;
            assignment->field = input.sequenceTargetField;
            assignment->expression = makeVariableAccess(
                *target,
                input.sequence->parameters.front().identifierToken.span);
            auto statement = std::make_unique<BoundExpressionStatement>();
            statement->span = assignment->span;
            statement->expression = std::move(assignment);
            result->statements.push_back(std::move(statement));
        }
        for (std::size_t index = 0; index < loopIndex; ++index) {
            result->statements.push_back(bindStatement(
                *input.sequence->body.statements[index]));
        }
    } else {
        for (std::size_t index = yieldIndex + 1;
             index < body.statements.size(); ++index) {
            result->statements.push_back(bindStatement(*body.statements[index]));
        }
    }

    const auto& yieldSyntax = static_cast<const
        syntax::YieldWaitStatementSyntax&>(*body.statements[yieldIndex]);
    auto thenBlock = std::make_unique<BoundBlockStatement>();
    thenBlock->span = body.span();
    for (std::size_t index = 0; index < yieldIndex; ++index) {
        thenBlock->statements.push_back(bindStatement(*body.statements[index]));
    }
    const auto* schedule = findScheduleFunction();
    if (!schedule) {
        diagnostics_.report(
            "RS2493",
            "sequence requires imported RealScript.Game.Schedule",
            yieldSyntax.span());
    } else {
        auto call = std::make_unique<BoundCallExpression>();
        call->type = schedule->returnType;
        call->typeName = schedule->returnTypeName;
        call->span = yieldSyntax.span();
        call->function = *schedule;
        call->arguments.push_back(makeSequenceFieldAccess(
            input.sequenceTargetField, yieldSyntax.span()));
        auto callback = std::make_unique<BoundLiteralExpression>();
        callback->type = PrimitiveType::String;
        callback->span = yieldSyntax.waitTicksToken.span;
        callback->value = input.sequenceNextCallback->name;
        call->arguments.push_back(std::move(callback));
        call->arguments.push_back(convertExpression(
            bindExpression(*yieldSyntax.delay),
            PrimitiveType::Int,
            yieldSyntax.delay->span(),
            "sequence wait_ticks delay"));
        auto statement = std::make_unique<BoundExpressionStatement>();
        statement->span = yieldSyntax.span();
        const auto* receiver = lookupVariable("this");
        if (receiver && currentOwnerType_) {
            auto assignment =
                std::make_unique<BoundMemberAssignmentExpression>();
            assignment->type = input.sequenceTimerField.type;
            assignment->typeName = input.sequenceTimerField.typeName;
            assignment->span = yieldSyntax.span();
            assignment->receiver = makeVariableAccess(
                *receiver, yieldSyntax.span());
            assignment->ownerType = *currentOwnerType_;
            assignment->field = input.sequenceTimerField;
            assignment->expression = std::move(call);
            statement->expression = std::move(assignment);
        } else {
            statement->expression = std::move(call);
        }
        thenBlock->statements.push_back(std::move(statement));
    }

    auto elseBlock = std::make_unique<BoundBlockStatement>();
    elseBlock->span = input.sequence->body.span();
    for (std::size_t index = loopIndex + 1;
         index < input.sequence->body.statements.size(); ++index) {
        if (input.sequence->body.statements[index]->kind() ==
            syntax::SyntaxKind::YieldBreakStatement) break;
        elseBlock->statements.push_back(bindStatement(
            *input.sequence->body.statements[index]));
    }

    auto branch = std::make_unique<BoundIfStatement>();
    branch->span = loop->span();
    branch->condition = convertExpression(
        bindExpression(*loop->condition),
        PrimitiveType::Bool,
        loop->condition->span(),
        "sequence loop condition");
    branch->thenStatement = std::move(thenBlock);
    branch->elseStatement = std::move(elseBlock);
    result->statements.push_back(std::move(branch));
    return result;
}

std::unique_ptr<BoundBlockStatement>
Binder::bindSingleYieldBranchSequence(const FunctionBindingInput& input) {
    auto result = std::make_unique<BoundBlockStatement>();
    result->span = input.sequence ? input.sequence->body.span() : text::TextSpan{};
    if (!input.sequence) return result;

    const syntax::IfStatementSyntax* branchSyntax = nullptr;
    std::size_t branchIndex = 0;
    for (std::size_t index = 0;
         index < input.sequence->body.statements.size(); ++index) {
        if (input.sequence->body.statements[index]->kind() ==
            syntax::SyntaxKind::IfStatement) {
            branchSyntax = static_cast<const syntax::IfStatementSyntax*>(
                input.sequence->body.statements[index].get());
            branchIndex = index;
            break;
        }
    }
    if (!branchSyntax ||
        (input.sequenceSegment == 0 && !input.sequenceNextCallback)) {
        diagnostics_.report(
            "RS8805", "sequence branch continuation is incomplete",
            input.sequence->identifierToken.span);
        return result;
    }
    const auto blockOf = [](const syntax::StatementSyntax* statement)
        -> const syntax::BlockStatementSyntax* {
        return statement && statement->kind() == syntax::SyntaxKind::BlockStatement
            ? static_cast<const syntax::BlockStatementSyntax*>(statement)
            : nullptr;
    };
    const auto* thenBlockSyntax = blockOf(branchSyntax->thenStatement.get());
    const auto* elseBlockSyntax = blockOf(branchSyntax->elseStatement.get());
    const syntax::BlockStatementSyntax* yieldingBlock = nullptr;
    std::size_t yieldIndex = 0;
    bool yieldInThen = false;
    const auto findYield = [&](const syntax::BlockStatementSyntax* block,
                               bool inThen) {
        if (!block || yieldingBlock) return;
        for (std::size_t index = 0; index < block->statements.size(); ++index) {
            if (block->statements[index]->kind() ==
                syntax::SyntaxKind::YieldWaitStatement) {
                yieldingBlock = block;
                yieldIndex = index;
                yieldInThen = inThen;
                return;
            }
        }
    };
    findYield(thenBlockSyntax, true);
    findYield(elseBlockSyntax, false);
    if (!yieldingBlock) {
        diagnostics_.report(
            "RS8805", "sequence branch has no suspension point",
            branchSyntax->span());
        return result;
    }

    const auto appendAfterBranch = [&](BoundBlockStatement& block) {
        for (std::size_t index = branchIndex + 1;
             index < input.sequence->body.statements.size(); ++index) {
            if (input.sequence->body.statements[index]->kind() ==
                syntax::SyntaxKind::YieldBreakStatement) break;
            block.statements.push_back(bindStatement(
                *input.sequence->body.statements[index]));
        }
    };

    if (input.sequenceSegment != 0) {
        for (std::size_t index = yieldIndex + 1;
             index < yieldingBlock->statements.size(); ++index) {
            result->statements.push_back(bindStatement(
                *yieldingBlock->statements[index]));
        }
        appendAfterBranch(*result);
        return result;
    }

    appendSequenceRestartCancellation(*result, input);
    const auto* receiver = lookupVariable("this");
    const auto* target = input.sequence->parameters.empty()
        ? nullptr
        : lookupVariable(input.sequence->parameters.front().identifierToken.text);
    if (receiver && target && currentOwnerType_) {
        auto assignment = std::make_unique<BoundMemberAssignmentExpression>();
        assignment->type = input.sequenceTargetField.type;
        assignment->typeName = input.sequenceTargetField.typeName;
        assignment->span = input.sequence->identifierToken.span;
        assignment->receiver = makeVariableAccess(
            *receiver, input.sequence->identifierToken.span);
        assignment->ownerType = *currentOwnerType_;
        assignment->field = input.sequenceTargetField;
        assignment->expression = makeVariableAccess(
            *target, input.sequence->parameters.front().identifierToken.span);
        auto statement = std::make_unique<BoundExpressionStatement>();
        statement->span = assignment->span;
        statement->expression = std::move(assignment);
        result->statements.push_back(std::move(statement));
    }
    for (std::size_t index = 0; index < branchIndex; ++index) {
        result->statements.push_back(bindStatement(
            *input.sequence->body.statements[index]));
    }

    const auto makeYieldingArm = [&]() {
        auto block = std::make_unique<BoundBlockStatement>();
        block->span = yieldingBlock->span();
        for (std::size_t index = 0; index < yieldIndex; ++index) {
            block->statements.push_back(bindStatement(
                *yieldingBlock->statements[index]));
        }
        const auto& yieldSyntax = static_cast<const
            syntax::YieldWaitStatementSyntax&>(
                *yieldingBlock->statements[yieldIndex]);
        const auto* schedule = findScheduleFunction();
        if (!schedule) return block;
        auto call = std::make_unique<BoundCallExpression>();
        call->type = schedule->returnType;
        call->typeName = schedule->returnTypeName;
        call->span = yieldSyntax.span();
        call->function = *schedule;
        call->arguments.push_back(makeSequenceFieldAccess(
            input.sequenceTargetField, yieldSyntax.span()));
        auto callback = std::make_unique<BoundLiteralExpression>();
        callback->type = PrimitiveType::String;
        callback->span = yieldSyntax.waitTicksToken.span;
        callback->value = input.sequenceNextCallback->name;
        call->arguments.push_back(std::move(callback));
        call->arguments.push_back(convertExpression(
            bindExpression(*yieldSyntax.delay), PrimitiveType::Int,
            yieldSyntax.delay->span(), "sequence wait_ticks delay"));
        auto timerAssignment =
            std::make_unique<BoundMemberAssignmentExpression>();
        timerAssignment->type = input.sequenceTimerField.type;
        timerAssignment->span = yieldSyntax.span();
        timerAssignment->receiver = makeVariableAccess(
            *receiver, yieldSyntax.span());
        timerAssignment->ownerType = *currentOwnerType_;
        timerAssignment->field = input.sequenceTimerField;
        timerAssignment->expression = std::move(call);
        auto statement = std::make_unique<BoundExpressionStatement>();
        statement->span = yieldSyntax.span();
        statement->expression = std::move(timerAssignment);
        block->statements.push_back(std::move(statement));
        return block;
    };
    const auto makeImmediateArm = [&](const syntax::BlockStatementSyntax* syntax) {
        auto block = std::make_unique<BoundBlockStatement>();
        block->span = syntax ? syntax->span() : branchSyntax->span();
        if (syntax) {
            for (const auto& statement : syntax->statements) {
                block->statements.push_back(bindStatement(*statement));
            }
        }
        appendAfterBranch(*block);
        return block;
    };

    auto branch = std::make_unique<BoundIfStatement>();
    branch->span = branchSyntax->span();
    branch->condition = convertExpression(
        bindExpression(*branchSyntax->condition), PrimitiveType::Bool,
        branchSyntax->condition->span(), "sequence branch condition");
    branch->thenStatement = yieldInThen
        ? makeYieldingArm()
        : makeImmediateArm(thenBlockSyntax);
    branch->elseStatement = yieldInThen
        ? makeImmediateArm(elseBlockSyntax)
        : makeYieldingArm();
    result->statements.push_back(std::move(branch));
    return result;
}

std::unique_ptr<BoundBlockStatement>
Binder::bindStateMachineSequence(const FunctionBindingInput& input) {
    auto result = std::make_unique<BoundBlockStatement>();
    result->span = input.sequence ? input.sequence->body.span() : text::TextSpan{};
    if (!input.sequence || !input.sequenceNextCallback || !currentOwnerType_) {
        diagnostics_.report(
            "RS8806", "sequence state machine is incomplete",
            input.symbol.declarationSpan);
        return result;
    }
    if (currentOwnerType_->kind == TypeKind::Struct) {
        diagnostics_.report(
            "RS2491", "sequence methods require a class owner",
            input.sequence->identifierToken.span);
        return result;
    }

    const auto sequenceCompositionTarget =
        [&](const syntax::StatementSyntax& statement)
            -> std::optional<std::string> {
        if (statement.kind() !=
            syntax::SyntaxKind::ExpressionStatement) {
            return std::nullopt;
        }
        const auto& expression = static_cast<const
            syntax::ExpressionStatementSyntax&>(statement);
        if (!expression.expression ||
            expression.expression->kind() !=
                syntax::SyntaxKind::CallExpression) {
            return std::nullopt;
        }
        const auto& call = static_cast<const
            syntax::CallExpressionSyntax&>(*expression.expression);
        const auto stateName =
            "$sequence_state_" + call.identifierToken.text;
        for (const auto& field : currentOwnerType_->fields) {
            if (field.synthetic && field.name == stateName &&
                field.type == PrimitiveType::Int) {
                return call.identifierToken.text;
            }
        }
        return std::nullopt;
    };
    using YieldMap = std::unordered_map<
        const syntax::StatementSyntax*, std::int64_t>;
    YieldMap yieldIds;
    std::int64_t nextYieldId = 1;
    std::function<void(const syntax::StatementSyntax&)> collectYields;
    collectYields = [&](const syntax::StatementSyntax& statement) {
        switch (statement.kind()) {
        case syntax::SyntaxKind::YieldWaitStatement:
            yieldIds.emplace(&statement, nextYieldId++);
            return;
        case syntax::SyntaxKind::ExpressionStatement:
            if (sequenceCompositionTarget(statement)) {
                yieldIds.emplace(&statement, nextYieldId++);
            }
            return;
        case syntax::SyntaxKind::BlockStatement:
            for (const auto& child : static_cast<const
                 syntax::BlockStatementSyntax&>(statement).statements) {
                if (child->kind() ==
                        syntax::SyntaxKind::YieldBreakStatement ||
                    child->kind() ==
                        syntax::SyntaxKind::ReturnStatement) break;
                collectYields(*child);
            }
            return;
        case syntax::SyntaxKind::IfStatement: {
            const auto& branch = static_cast<const
                syntax::IfStatementSyntax&>(statement);
            collectYields(*branch.thenStatement);
            if (branch.elseStatement) collectYields(*branch.elseStatement);
            return;
        }
        case syntax::SyntaxKind::WhileStatement:
            collectYields(*static_cast<const syntax::WhileStatementSyntax&>(
                statement).body);
            return;
        case syntax::SyntaxKind::ForStatement:
            collectYields(*static_cast<const syntax::ForStatementSyntax&>(
                statement).body);
            return;
        case syntax::SyntaxKind::ForeachStatement:
            collectYields(*static_cast<const syntax::ForeachStatementSyntax&>(
                statement).body);
            return;
        case syntax::SyntaxKind::DoWhileStatement:
            collectYields(*static_cast<const syntax::DoWhileStatementSyntax&>(
                statement).body);
            return;
        case syntax::SyntaxKind::SwitchStatement:
            for (const auto& section : static_cast<const
                 syntax::SwitchStatementSyntax&>(statement).sections) {
                for (const auto& child : section.statements) collectYields(*child);
            }
            return;
        default:
            return;
        }
    };
    collectYields(input.sequence->body);

    std::function<std::vector<std::int64_t>(const syntax::StatementSyntax&)>
        descendantIds;
    descendantIds = [&](const syntax::StatementSyntax& statement) {
        std::vector<std::int64_t> ids;
        const auto append = [&](std::vector<std::int64_t> values) {
            ids.insert(ids.end(), values.begin(), values.end());
        };
        switch (statement.kind()) {
        case syntax::SyntaxKind::YieldWaitStatement: {
            const auto found = yieldIds.find(&statement);
            if (found != yieldIds.end()) ids.push_back(found->second);
            break;
        }
        case syntax::SyntaxKind::ExpressionStatement: {
            const auto found = yieldIds.find(&statement);
            if (found != yieldIds.end()) ids.push_back(found->second);
            break;
        }
        case syntax::SyntaxKind::BlockStatement:
            for (const auto& child : static_cast<const
                 syntax::BlockStatementSyntax&>(statement).statements) {
                append(descendantIds(*child));
                if (child->kind() ==
                        syntax::SyntaxKind::YieldBreakStatement ||
                    child->kind() ==
                        syntax::SyntaxKind::ReturnStatement) break;
            }
            break;
        case syntax::SyntaxKind::IfStatement: {
            const auto& branch = static_cast<const
                syntax::IfStatementSyntax&>(statement);
            append(descendantIds(*branch.thenStatement));
            if (branch.elseStatement) append(descendantIds(*branch.elseStatement));
            break;
        }
        case syntax::SyntaxKind::WhileStatement:
            append(descendantIds(*static_cast<const
                syntax::WhileStatementSyntax&>(statement).body));
            break;
        case syntax::SyntaxKind::ForStatement: {
            const auto& loop = static_cast<const
                syntax::ForStatementSyntax&>(statement);
            if (loop.initializer) append(descendantIds(*loop.initializer));
            append(descendantIds(*loop.body));
            break;
        }
        case syntax::SyntaxKind::ForeachStatement:
            append(descendantIds(*static_cast<const
                syntax::ForeachStatementSyntax&>(statement).body));
            break;
        case syntax::SyntaxKind::DoWhileStatement:
            append(descendantIds(*static_cast<const
                syntax::DoWhileStatementSyntax&>(statement).body));
            break;
        case syntax::SyntaxKind::SwitchStatement:
            for (const auto& section : static_cast<const
                 syntax::SwitchStatementSyntax&>(statement).sections) {
                for (const auto& child : section.statements) {
                    append(descendantIds(*child));
                }
            }
            break;
        default:
            break;
        }
        return ids;
    };

    const auto makeStateValue = [&](text::TextSpan span) {
        return makeSequenceFieldAccess(input.sequenceStateField, span);
    };
    const auto makeIntLiteral = [](std::int64_t value, text::TextSpan span) {
        auto literal = std::make_unique<BoundLiteralExpression>();
        literal->type = PrimitiveType::Int;
        literal->value = value;
        literal->span = span;
        return literal;
    };
    const auto makeBoolLiteral = [](bool value, text::TextSpan span) {
        auto literal = std::make_unique<BoundLiteralExpression>();
        literal->type = PrimitiveType::Bool;
        literal->value = value;
        literal->span = span;
        return literal;
    };
    const auto makeStateEquals = [&](std::int64_t value, text::TextSpan span) {
        auto expression = std::make_unique<BoundBinaryExpression>();
        expression->type = PrimitiveType::Bool;
        expression->operatorKind = BoundBinaryOperatorKind::Equals;
        expression->span = span;
        expression->left = makeStateValue(span);
        expression->right = makeIntLiteral(value, span);
        return expression;
    };
    const auto makeStateIn = [&](const std::vector<std::int64_t>& ids,
                                 text::TextSpan span)
        -> std::unique_ptr<BoundExpression> {
        std::unique_ptr<BoundExpression> condition;
        for (const auto id : ids) {
            auto equals = makeStateEquals(id, span);
            if (!condition) {
                condition = std::move(equals);
                continue;
            }
            auto either = std::make_unique<BoundBinaryExpression>();
            either->type = PrimitiveType::Bool;
            either->operatorKind = BoundBinaryOperatorKind::LogicalOr;
            either->span = span;
            either->left = std::move(condition);
            either->right = std::move(equals);
            condition = std::move(either);
        }
        if (!condition) {
            auto value = std::make_unique<BoundLiteralExpression>();
            value->type = PrimitiveType::Bool;
            value->value = false;
            value->span = span;
            condition = std::move(value);
        }
        return condition;
    };
    const auto makeStateAssignment = [&](std::int64_t value, text::TextSpan span) {
        auto assignment = std::make_unique<BoundMemberAssignmentExpression>();
        assignment->type = PrimitiveType::Int;
        assignment->span = span;
        assignment->receiver = makeSequenceFieldAccess(
            input.sequenceStateField, span);
        // Member assignment needs the owning object, not the field value.
        const auto* receiver = lookupVariable("this");
        assignment->receiver = receiver
            ? makeVariableAccess(*receiver, span)
            : makeError(span);
        assignment->ownerType = *currentOwnerType_;
        assignment->field = input.sequenceStateField;
        assignment->expression = makeIntLiteral(value, span);
        auto statement = std::make_unique<BoundExpressionStatement>();
        statement->span = span;
        statement->expression = std::move(assignment);
        return statement;
    };
    const auto makeFieldAssignmentExpression = [&](
        const FieldSymbol& field,
        std::unique_ptr<BoundExpression> value,
        text::TextSpan span) {
        auto assignment = std::make_unique<BoundMemberAssignmentExpression>();
        assignment->type = field.type;
        assignment->typeName = field.typeName;
        assignment->span = span;
        const auto* receiver = lookupVariable("this");
        assignment->receiver = receiver
            ? makeVariableAccess(*receiver, span)
            : makeError(span);
        assignment->ownerType = *currentOwnerType_;
        assignment->field = field;
        assignment->expression = std::move(value);
        return assignment;
    };
    const auto makeFieldAssignmentStatement = [&](
        const FieldSymbol& field,
        std::unique_ptr<BoundExpression> value,
        text::TextSpan span) {
        auto statement = std::make_unique<BoundExpressionStatement>();
        statement->span = span;
        statement->expression = makeFieldAssignmentExpression(
            field, std::move(value), span);
        return statement;
    };
    const auto makeCompositionPoll = [&](text::TextSpan span) {
        auto block = std::make_unique<BoundBlockStatement>();
        block->span = span;
        const auto* schedule = findScheduleFunction();
        if (!schedule) {
            diagnostics_.report(
                "RS2493",
                "nested sequence composition requires imported RealScript.Game.Schedule",
                span);
        } else {
            auto call = std::make_unique<BoundCallExpression>();
            call->type = schedule->returnType;
            call->typeName = schedule->returnTypeName;
            call->span = span;
            call->function = *schedule;
            call->arguments.push_back(makeSequenceFieldAccess(
                input.sequenceTargetField, span));
            auto callback = std::make_unique<BoundLiteralExpression>();
            callback->type = PrimitiveType::String;
            callback->value = input.sequenceNextCallback->name;
            callback->span = span;
            call->arguments.push_back(std::move(callback));
            call->arguments.push_back(makeIntLiteral(1, span));
            block->statements.push_back(makeFieldAssignmentStatement(
                input.sequenceTimerField, std::move(call), span));
        }
        auto stop = std::make_unique<BoundReturnStatement>();
        stop->span = span;
        block->statements.push_back(std::move(stop));
        return block;
    };

    std::function<std::unique_ptr<BoundStatement>(
        const syntax::StatementSyntax&)> transform;
    std::function<std::unique_ptr<BoundBlockStatement>(
        const syntax::BlockStatementSyntax&)> transformBlock;

    const auto wrapForResume = [&](const syntax::StatementSyntax& syntaxTree,
                                   std::unique_ptr<BoundStatement> statement) {
        const auto ids = descendantIds(syntaxTree);
        std::unique_ptr<BoundExpression> condition =
            makeStateEquals(0, syntaxTree.span());
        if (!ids.empty()) {
            auto either = std::make_unique<BoundBinaryExpression>();
            either->type = PrimitiveType::Bool;
            either->operatorKind = BoundBinaryOperatorKind::LogicalOr;
            either->span = syntaxTree.span();
            either->left = std::move(condition);
            either->right = makeStateIn(ids, syntaxTree.span());
            condition = std::move(either);
        }
        auto branch = std::make_unique<BoundIfStatement>();
        branch->span = syntaxTree.span();
        branch->condition = std::move(condition);
        branch->thenStatement = std::move(statement);
        return std::unique_ptr<BoundStatement>(std::move(branch));
    };

    transformBlock = [&](const syntax::BlockStatementSyntax& syntaxTree) {
        auto block = std::make_unique<BoundBlockStatement>();
        block->span = syntaxTree.span();
        for (const auto& child : syntaxTree.statements) {
            block->statements.push_back(wrapForResume(
                *child, transform(*child)));
            if (child->kind() ==
                    syntax::SyntaxKind::YieldBreakStatement ||
                child->kind() ==
                    syntax::SyntaxKind::ReturnStatement) break;
        }
        return block;
    };

    transform = [&](const syntax::StatementSyntax& syntaxTree)
        -> std::unique_ptr<BoundStatement> {
        switch (syntaxTree.kind()) {
        case syntax::SyntaxKind::BlockStatement:
            return transformBlock(static_cast<const
                syntax::BlockStatementSyntax&>(syntaxTree));
        case syntax::SyntaxKind::ExpressionStatement: {
            const auto targetName =
                sequenceCompositionTarget(syntaxTree);
            if (!targetName) return bindStatement(syntaxTree);
            const auto id = yieldIds.at(&syntaxTree);
            const FieldSymbol* childState = nullptr;
            const auto stateName = "$sequence_state_" + *targetName;
            for (const auto& field : currentOwnerType_->fields) {
                if (field.synthetic && field.name == stateName) {
                    childState = &field;
                    break;
                }
            }
            if (!childState) {
                diagnostics_.report(
                    "RS8811",
                    "nested sequence state field is unavailable",
                    syntaxTree.span());
                return bindStatement(syntaxTree);
            }

            auto resumed = std::make_unique<BoundIfStatement>();
            resumed->span = syntaxTree.span();
            auto childCompleted =
                std::make_unique<BoundBinaryExpression>();
            childCompleted->type = PrimitiveType::Bool;
            childCompleted->operatorKind =
                BoundBinaryOperatorKind::Equals;
            childCompleted->span = syntaxTree.span();
            childCompleted->left = makeSequenceFieldAccess(
                *childState, syntaxTree.span());
            childCompleted->right = makeIntLiteral(
                0, syntaxTree.span());
            resumed->condition = std::move(childCompleted);
            resumed->thenStatement = makeStateAssignment(
                0, syntaxTree.span());
            resumed->elseStatement =
                makeCompositionPoll(syntaxTree.span());

            auto suspended =
                std::make_unique<BoundBlockStatement>();
            suspended->span = syntaxTree.span();
            suspended->statements.push_back(makeStateAssignment(
                id, syntaxTree.span()));
            suspended->statements.push_back(
                bindStatement(syntaxTree));
            auto poll = makeCompositionPoll(syntaxTree.span());
            suspended->statements.insert(
                suspended->statements.end(),
                std::make_move_iterator(poll->statements.begin()),
                std::make_move_iterator(poll->statements.end()));

            auto dispatch = std::make_unique<BoundIfStatement>();
            dispatch->span = syntaxTree.span();
            dispatch->condition = makeStateEquals(
                id, syntaxTree.span());
            dispatch->thenStatement = std::move(resumed);
            dispatch->elseStatement = std::move(suspended);
            return dispatch;
        }
        case syntax::SyntaxKind::YieldWaitStatement: {
            const auto& yield = static_cast<const
                syntax::YieldWaitStatementSyntax&>(syntaxTree);
            const auto id = yieldIds.at(&yield);
            auto resumed = std::make_unique<BoundBlockStatement>();
            resumed->span = yield.span();
            resumed->statements.push_back(makeStateAssignment(0, yield.span()));

            auto suspended = std::make_unique<BoundBlockStatement>();
            suspended->span = yield.span();
            suspended->statements.push_back(makeStateAssignment(id, yield.span()));
            const auto* schedule = findScheduleFunction();
            if (!schedule) {
                diagnostics_.report(
                    "RS2493",
                    "sequence requires imported RealScript.Game.Schedule",
                    yield.span());
            } else {
                auto call = std::make_unique<BoundCallExpression>();
                call->type = schedule->returnType;
                call->typeName = schedule->returnTypeName;
                call->span = yield.span();
                call->function = *schedule;
                call->arguments.push_back(makeSequenceFieldAccess(
                    input.sequenceTargetField, yield.span()));
                auto callback = std::make_unique<BoundLiteralExpression>();
                callback->type = PrimitiveType::String;
                callback->value = input.sequenceNextCallback->name;
                callback->span = yield.waitTicksToken.span;
                call->arguments.push_back(std::move(callback));
                call->arguments.push_back(convertExpression(
                    bindExpression(*yield.delay), PrimitiveType::Int,
                    yield.delay->span(), "sequence wait_ticks delay"));
                auto timer = std::make_unique<BoundMemberAssignmentExpression>();
                timer->type = input.sequenceTimerField.type;
                timer->span = yield.span();
                const auto* receiver = lookupVariable("this");
                timer->receiver = receiver
                    ? makeVariableAccess(*receiver, yield.span())
                    : makeError(yield.span());
                timer->ownerType = *currentOwnerType_;
                timer->field = input.sequenceTimerField;
                timer->expression = std::move(call);
                auto scheduleStatement =
                    std::make_unique<BoundExpressionStatement>();
                scheduleStatement->span = yield.span();
                scheduleStatement->expression = std::move(timer);
                suspended->statements.push_back(std::move(scheduleStatement));
            }
            auto stop = std::make_unique<BoundReturnStatement>();
            stop->span = yield.span();
            suspended->statements.push_back(std::move(stop));

            auto branch = std::make_unique<BoundIfStatement>();
            branch->span = yield.span();
            branch->condition = makeStateEquals(id, yield.span());
            branch->thenStatement = std::move(resumed);
            branch->elseStatement = std::move(suspended);
            return branch;
        }
        case syntax::SyntaxKind::YieldBreakStatement: {
            auto block = std::make_unique<BoundBlockStatement>();
            block->span = syntaxTree.span();
            block->statements.push_back(makeStateAssignment(0, syntaxTree.span()));
            block->statements.push_back(makeFieldAssignmentStatement(
                input.sequenceCompletedField,
                makeBoolLiteral(true, syntaxTree.span()),
                syntaxTree.span()));
            auto stop = std::make_unique<BoundReturnStatement>();
            stop->span = syntaxTree.span();
            block->statements.push_back(std::move(stop));
            return block;
        }
        case syntax::SyntaxKind::ReturnStatement: {
            const auto& source = static_cast<const
                syntax::ReturnStatementSyntax&>(syntaxTree);
            auto block = std::make_unique<BoundBlockStatement>();
            block->span = source.span();
            const auto hasResult =
                input.sequenceResultField.type != PrimitiveType::Error &&
                input.sequenceResultField.type != PrimitiveType::Void &&
                !input.sequenceResultField.name.empty();
            if (source.expression && hasResult) {
                block->statements.push_back(makeFieldAssignmentStatement(
                    input.sequenceResultField,
                    bindTargetExpression(
                        *source.expression,
                        input.sequenceResultField.type,
                        input.sequenceResultField.typeName,
                        "sequence result"),
                    source.expression->span()));
            } else if (source.expression) {
                diagnostics_.report(
                    "RS8813",
                    "a void sequence cannot return a value",
                    source.expression->span());
            } else if (hasResult) {
                diagnostics_.report(
                    "RS8814",
                    "a result sequence must return a value",
                    source.span());
            }
            block->statements.push_back(makeStateAssignment(
                0, source.span()));
            block->statements.push_back(makeFieldAssignmentStatement(
                input.sequenceCompletedField,
                makeBoolLiteral(true, source.span()),
                source.span()));
            auto stop = std::make_unique<BoundReturnStatement>();
            stop->span = source.span();
            block->statements.push_back(std::move(stop));
            return block;
        }
        case syntax::SyntaxKind::IfStatement: {
            const auto& source = static_cast<const
                syntax::IfStatementSyntax&>(syntaxTree);
            const auto thenIds = descendantIds(*source.thenStatement);
            const auto elseIds = source.elseStatement
                ? descendantIds(*source.elseStatement)
                : std::vector<std::int64_t>{};
            auto ordinary = std::make_unique<BoundIfStatement>();
            ordinary->span = source.span();
            ordinary->condition = convertExpression(
                bindExpression(*source.condition), PrimitiveType::Bool,
                source.condition->span(), "sequence branch condition");
            ordinary->thenStatement = transform(*source.thenStatement);
            if (source.elseStatement) {
                ordinary->elseStatement = transform(*source.elseStatement);
            }
            std::unique_ptr<BoundStatement> dispatch = std::move(ordinary);
            if (!elseIds.empty()) {
                auto branch = std::make_unique<BoundIfStatement>();
                branch->span = source.span();
                branch->condition = makeStateIn(elseIds, source.span());
                branch->thenStatement = transform(*source.elseStatement);
                branch->elseStatement = std::move(dispatch);
                dispatch = std::move(branch);
            }
            if (!thenIds.empty()) {
                auto branch = std::make_unique<BoundIfStatement>();
                branch->span = source.span();
                branch->condition = makeStateIn(thenIds, source.span());
                branch->thenStatement = transform(*source.thenStatement);
                branch->elseStatement = std::move(dispatch);
                dispatch = std::move(branch);
            }
            return dispatch;
        }
        case syntax::SyntaxKind::WhileStatement: {
            const auto& source = static_cast<const
                syntax::WhileStatementSyntax&>(syntaxTree);
            const auto ids = descendantIds(*source.body);
            auto resumed = std::make_unique<BoundDoWhileStatement>();
            resumed->span = source.span();
            ++loopDepth_;
            ++breakableDepth_;
            resumed->body = transform(*source.body);
            --breakableDepth_;
            --loopDepth_;
            resumed->condition = convertExpression(
                bindExpression(*source.condition), PrimitiveType::Bool,
                source.condition->span(), "sequence loop condition");

            auto loop = std::make_unique<BoundWhileStatement>();
            loop->span = source.span();
            loop->condition = convertExpression(
                bindExpression(*source.condition), PrimitiveType::Bool,
                source.condition->span(), "sequence loop condition");
            ++loopDepth_;
            ++breakableDepth_;
            loop->body = transform(*source.body);
            --breakableDepth_;
            --loopDepth_;
            auto dispatch = std::make_unique<BoundIfStatement>();
            dispatch->span = source.span();
            dispatch->condition = makeStateIn(ids, source.span());
            dispatch->thenStatement = std::move(resumed);
            dispatch->elseStatement = std::move(loop);
            return dispatch;
        }
        case syntax::SyntaxKind::ForStatement: {
            const auto& source = static_cast<const
                syntax::ForStatementSyntax&>(syntaxTree);
            const auto ids = descendantIds(*source.body);
            const auto bindCondition = [&]() -> std::unique_ptr<BoundExpression> {
                if (source.condition) {
                    return convertExpression(
                        bindExpression(*source.condition), PrimitiveType::Bool,
                        source.condition->span(), "sequence for condition");
                }
                auto value = std::make_unique<BoundLiteralExpression>();
                value->type = PrimitiveType::Bool;
                value->value = true;
                value->span = source.forKeyword.span;
                return value;
            };
            const auto bindIncrement = [&]() -> std::unique_ptr<BoundExpression> {
                return source.increment
                    ? bindExpression(*source.increment)
                    : nullptr;
            };
            auto resumed = std::make_unique<BoundForStatement>();
            resumed->span = source.span();
            auto resumeCondition = std::make_unique<BoundBinaryExpression>();
            resumeCondition->type = PrimitiveType::Bool;
            resumeCondition->operatorKind = BoundBinaryOperatorKind::LogicalOr;
            resumeCondition->span = source.span();
            resumeCondition->left = makeStateIn(ids, source.span());
            resumeCondition->right = bindCondition();
            resumed->condition = std::move(resumeCondition);
            resumed->increment = bindIncrement();
            ++loopDepth_;
            ++breakableDepth_;
            resumed->body = transform(*source.body);
            --breakableDepth_;
            --loopDepth_;

            auto loop = std::make_unique<BoundForStatement>();
            loop->span = source.span();
            if (source.initializer) {
                loop->initializer = bindStatement(*source.initializer);
            }
            loop->condition = bindCondition();
            loop->increment = bindIncrement();
            ++loopDepth_;
            ++breakableDepth_;
            loop->body = transform(*source.body);
            --breakableDepth_;
            --loopDepth_;

            auto dispatch = std::make_unique<BoundIfStatement>();
            dispatch->span = source.span();
            dispatch->condition = makeStateIn(ids, source.span());
            dispatch->thenStatement = std::move(resumed);
            dispatch->elseStatement = std::move(loop);
            return dispatch;
        }
        case syntax::SyntaxKind::ForeachStatement: {
            const auto& source = static_cast<const
                syntax::ForeachStatementSyntax&>(syntaxTree);
            const auto state = std::find_if(
                input.sequenceForeachFields.begin(),
                input.sequenceForeachFields.end(),
                [&](const FunctionBindingInput::SequenceForeachFields& value) {
                    return value.syntax == &source;
                });
            if (state == input.sequenceForeachFields.end()) {
                diagnostics_.report(
                    "RS8809",
                    "suspending foreach state fields are unavailable",
                    source.span());
                return bindForeachStatement(source);
            }

            auto collection = bindExpression(*source.collection);
            const auto originalCollectionTypeName = collection->typeName;
            const auto findInstanceMethod = [&](const std::string& typeName,
                                                const std::string& name,
                                                std::size_t arity)
                -> const FunctionSymbol* {
                const auto found = visibleTypes_.find(typeName);
                if (found == visibleTypes_.end()) return nullptr;
                for (const auto& method : found->second.methods) {
                    const auto visibleArity = method.parameters.size() -
                        ((!method.staticMethod && method.method) ? 1u : 0u);
                    if (!method.staticMethod && method.name == name &&
                        visibleArity == arity) return &method;
                }
                return nullptr;
            };
            const auto makeInstanceCall = [&](
                const FunctionSymbol& method,
                std::unique_ptr<BoundExpression> receiver,
                std::vector<std::unique_ptr<BoundExpression>> arguments,
                text::TextSpan span) {
                auto call = std::make_unique<BoundCallExpression>();
                call->type = method.returnType;
                call->typeName = method.returnTypeName;
                call->span = span;
                call->function = method;
                call->arguments.push_back(std::move(receiver));
                for (auto& argument : arguments) {
                    call->arguments.push_back(std::move(argument));
                }
                return call;
            };

            if (state->usesEnumerator) {
                const auto* getEnumerator = findInstanceMethod(
                    originalCollectionTypeName, "GetEnumerator", 0);
                if (!getEnumerator) {
                    diagnostics_.report(
                        "RS8808",
                        "suspending foreach GetEnumerator() is unavailable",
                        source.collection->span());
                    collection = makeError(source.collection->span());
                } else {
                    collection = makeInstanceCall(
                        *getEnumerator, std::move(collection), {},
                        source.collection->span());
                }
            }
            collection = convertExpression(
                std::move(collection), state->collection.type,
                source.collection->span(), "sequence foreach collection",
                state->collection.typeName);

            const auto ids = descendantIds(*source.body);
            const auto makeIndexAccess = [&]() {
                return makeSequenceFieldAccess(
                    state->index, source.identifierToken.span);
            };
            const auto makeCollectionAccess = [&]() {
                return makeSequenceFieldAccess(
                    state->collection, source.collection->span());
            };
            const FunctionSymbol* countMethod = nullptr;
            const FunctionSymbol* getMethod = nullptr;
            const FunctionSymbol* moveNextMethod = nullptr;
            const FunctionSymbol* currentMethod = nullptr;
            if (state->collection.type == PrimitiveType::Object) {
                if (state->usesEnumerator) {
                    moveNextMethod = findInstanceMethod(
                        state->collection.typeName, "MoveNext", 0);
                    currentMethod = findInstanceMethod(
                        state->collection.typeName, "Current", 0);
                } else {
                    countMethod = findInstanceMethod(
                        state->collection.typeName, "Count", 0);
                    getMethod = findInstanceMethod(
                        state->collection.typeName, "Get", 1);
                }
            }
            const auto makeCondition = [&]() -> std::unique_ptr<BoundExpression> {
                if (state->collection.type == PrimitiveType::Array) {
                    auto length = std::make_unique<BoundArrayLengthExpression>();
                    length->type = PrimitiveType::Int;
                    length->span = source.collection->span();
                    length->receiver = makeCollectionAccess();
                    auto comparison = std::make_unique<BoundBinaryExpression>();
                    comparison->type = PrimitiveType::Bool;
                    comparison->operatorKind = BoundBinaryOperatorKind::Less;
                    comparison->span = source.span();
                    comparison->left = makeIndexAccess();
                    comparison->right = std::move(length);
                    return comparison;
                }
                if (state->usesEnumerator && moveNextMethod) {
                    return makeInstanceCall(
                        *moveNextMethod, makeCollectionAccess(), {},
                        source.collection->span());
                }
                if (countMethod) {
                    auto count = makeInstanceCall(
                        *countMethod, makeCollectionAccess(), {},
                        source.collection->span());
                    auto comparison = std::make_unique<BoundBinaryExpression>();
                    comparison->type = PrimitiveType::Bool;
                    comparison->operatorKind = BoundBinaryOperatorKind::Less;
                    comparison->span = source.span();
                    comparison->left = makeIndexAccess();
                    comparison->right = std::move(count);
                    return comparison;
                }
                diagnostics_.report(
                    "RS8808",
                    "suspending foreach iteration protocol is incomplete",
                    source.collection->span());
                return makeError(source.collection->span());
            };
            const auto makeElement = [&]() -> std::unique_ptr<BoundExpression> {
                std::unique_ptr<BoundExpression> element;
                if (state->collection.type == PrimitiveType::Array) {
                    auto access =
                        std::make_unique<BoundElementAccessExpression>();
                    PrimitiveType elementType = PrimitiveType::Error;
                    std::string elementTypeName;
                    if (!decodeArrayTypeName(
                            state->collection.typeName,
                            elementType, elementTypeName)) {
                        diagnostics_.report(
                            "RS2210",
                            "foreach array element type is invalid",
                            source.collection->span());
                    }
                    access->type = elementType;
                    access->typeName = elementTypeName;
                    access->elementType = elementType;
                    access->elementTypeName = elementTypeName;
                    access->span = source.identifierToken.span;
                    access->receiver = makeCollectionAccess();
                    access->index = makeIndexAccess();
                    element = std::move(access);
                } else if (state->usesEnumerator && currentMethod) {
                    element = makeInstanceCall(
                        *currentMethod, makeCollectionAccess(), {},
                        source.identifierToken.span);
                } else if (getMethod) {
                    std::vector<std::unique_ptr<BoundExpression>> arguments;
                    arguments.push_back(makeIndexAccess());
                    element = makeInstanceCall(
                        *getMethod, makeCollectionAccess(),
                        std::move(arguments), source.identifierToken.span);
                } else {
                    element = makeError(source.identifierToken.span);
                }
                return convertExpression(
                    std::move(element), state->iteration.type,
                    source.identifierToken.span, "sequence foreach element",
                    state->iteration.typeName);
            };
            const auto makeBody = [&]() {
                auto body = std::make_unique<BoundBlockStatement>();
                body->span = source.body->span();
                auto initializeIteration =
                    std::make_unique<BoundIfStatement>();
                initializeIteration->span = source.identifierToken.span;
                initializeIteration->condition = makeStateEquals(
                    0, source.identifierToken.span);
                initializeIteration->thenStatement =
                    makeFieldAssignmentStatement(
                        state->iteration, makeElement(),
                        source.identifierToken.span);
                body->statements.push_back(std::move(initializeIteration));
                body->statements.push_back(transform(*source.body));
                return body;
            };
            const auto makeLoop = [&](bool resume,
                                      std::unique_ptr<BoundExpression>
                                          initialCollection) {
                auto loop = std::make_unique<BoundForStatement>();
                loop->span = source.span();
                if (!resume) {
                    auto initializer =
                        std::make_unique<BoundBlockStatement>();
                    initializer->span = source.collection->span();
                    initializer->statements.push_back(
                        makeFieldAssignmentStatement(
                            state->collection,
                            std::move(initialCollection),
                            source.collection->span()));
                    initializer->statements.push_back(
                        makeFieldAssignmentStatement(
                            state->index,
                            makeIntLiteral(
                                0, source.identifierToken.span),
                            source.identifierToken.span));
                    loop->initializer = std::move(initializer);
                }
                auto condition = makeCondition();
                if (resume) {
                    auto either =
                        std::make_unique<BoundBinaryExpression>();
                    either->type = PrimitiveType::Bool;
                    either->operatorKind =
                        BoundBinaryOperatorKind::LogicalOr;
                    either->span = source.span();
                    either->left = makeStateIn(ids, source.span());
                    either->right = std::move(condition);
                    condition = std::move(either);
                }
                loop->condition = std::move(condition);
                if (!state->usesEnumerator) {
                    auto addition =
                        std::make_unique<BoundBinaryExpression>();
                    addition->type = PrimitiveType::Int;
                    addition->operatorKind =
                        BoundBinaryOperatorKind::Addition;
                    addition->span = source.identifierToken.span;
                    addition->left = makeIndexAccess();
                    addition->right = makeIntLiteral(
                        1, source.identifierToken.span);
                    loop->increment = makeFieldAssignmentExpression(
                        state->index, std::move(addition),
                        source.identifierToken.span);
                }
                ++loopDepth_;
                ++breakableDepth_;
                loop->body = makeBody();
                --breakableDepth_;
                --loopDepth_;
                return loop;
            };
            auto dispatch = std::make_unique<BoundIfStatement>();
            dispatch->span = source.span();
            dispatch->condition = makeStateIn(ids, source.span());
            dispatch->thenStatement = makeLoop(true, nullptr);
            dispatch->elseStatement =
                makeLoop(false, std::move(collection));
            return dispatch;
        }
        case syntax::SyntaxKind::DoWhileStatement: {
            const auto& source = static_cast<const
                syntax::DoWhileStatementSyntax&>(syntaxTree);
            const auto makeLoop = [&]() {
                auto loop = std::make_unique<BoundDoWhileStatement>();
                loop->span = source.span();
                ++loopDepth_;
                ++breakableDepth_;
                loop->body = transform(*source.body);
                --breakableDepth_;
                --loopDepth_;
                loop->condition = convertExpression(
                    bindExpression(*source.condition), PrimitiveType::Bool,
                    source.condition->span(), "sequence do/while condition");
                return loop;
            };
            auto dispatch = std::make_unique<BoundIfStatement>();
            dispatch->span = source.span();
            dispatch->condition = makeStateIn(
                descendantIds(*source.body), source.span());
            dispatch->thenStatement = makeLoop();
            dispatch->elseStatement = makeLoop();
            return dispatch;
        }
        case syntax::SyntaxKind::SwitchStatement: {
            const auto& source = static_cast<const
                syntax::SwitchStatementSyntax&>(syntaxTree);
            auto switchValue = bindExpression(*source.expression);
            VariableSymbol valueVariable;
            valueVariable.name = "$sequence_switch_value_" +
                std::to_string(nextVariableIndex_);
            valueVariable.type = switchValue->type;
            valueVariable.typeName = switchValue->typeName;
            valueVariable.index = nextVariableIndex_++;
            valueVariable.id = stableTypeId(
                std::to_string(currentFunctionId_) + "::local:" +
                std::to_string(valueVariable.index) + ":" +
                valueVariable.name);
            (void)declareVariable(valueVariable, source.expression->span());

            const auto makeSection = [&](const syntax::SwitchSectionSyntax& section,
                                         bool resume) {
                auto body = std::make_unique<BoundBlockStatement>();
                body->span = section.span();
                ++breakableDepth_;
                for (const auto& statement : section.statements) {
                    body->statements.push_back(wrapForResume(
                        *statement, transform(*statement)));
                }
                --breakableDepth_;
                auto stop = std::make_unique<BoundBreakStatement>();
                stop->span = section.span();
                body->statements.push_back(std::move(stop));
                auto once = std::make_unique<BoundWhileStatement>();
                once->span = section.span();
                if (resume) {
                    std::vector<std::int64_t> ids;
                    for (const auto& statement : section.statements) {
                        const auto nested = descendantIds(*statement);
                        ids.insert(ids.end(), nested.begin(), nested.end());
                    }
                    once->condition = makeStateIn(ids, section.span());
                } else {
                    auto always = std::make_unique<BoundLiteralExpression>();
                    always->type = PrimitiveType::Bool;
                    always->value = true;
                    always->span = section.span();
                    once->condition = std::move(always);
                }
                once->body = std::move(body);
                return once;
            };

            auto normal = std::make_unique<BoundBlockStatement>();
            normal->span = source.span();
            auto declaration =
                std::make_unique<BoundVariableDeclarationStatement>();
            declaration->span = source.expression->span();
            declaration->variable = valueVariable;
            declaration->initializer = std::move(switchValue);
            normal->statements.push_back(std::move(declaration));

            std::unique_ptr<BoundStatement> normalDispatch;
            for (auto section = source.sections.rbegin();
                 section != source.sections.rend(); ++section) {
                auto sectionBody = makeSection(*section, false);
                if (!section->label) {
                    normalDispatch = std::move(sectionBody);
                    continue;
                }
                auto comparison = std::make_unique<BoundBinaryExpression>();
                comparison->type = PrimitiveType::Bool;
                comparison->operatorKind = BoundBinaryOperatorKind::Equals;
                comparison->span = section->span();
                comparison->left = makeVariableAccess(
                    valueVariable, source.expression->span());
                comparison->right = convertExpression(
                    bindExpression(*section->label), valueVariable.type,
                    section->label->span(), "sequence switch case",
                    valueVariable.typeName);
                auto branch = std::make_unique<BoundIfStatement>();
                branch->span = section->span();
                branch->condition = std::move(comparison);
                branch->thenStatement = std::move(sectionBody);
                branch->elseStatement = std::move(normalDispatch);
                normalDispatch = std::move(branch);
            }
            if (normalDispatch) {
                normal->statements.push_back(std::move(normalDispatch));
            }

            std::unique_ptr<BoundStatement> dispatch = std::move(normal);
            for (auto section = source.sections.rbegin();
                 section != source.sections.rend(); ++section) {
                std::vector<std::int64_t> ids;
                for (const auto& statement : section->statements) {
                    const auto nested = descendantIds(*statement);
                    ids.insert(ids.end(), nested.begin(), nested.end());
                }
                if (ids.empty()) continue;
                auto branch = std::make_unique<BoundIfStatement>();
                branch->span = section->span();
                branch->condition = makeStateIn(ids, section->span());
                branch->thenStatement = makeSection(*section, true);
                branch->elseStatement = std::move(dispatch);
                dispatch = std::move(branch);
            }
            return dispatch;
        }
        default:
            return bindStatement(syntaxTree);
        }
    };

    if (input.sequenceSegment == 0) {
        appendSequenceRestartCancellation(*result, input);
        auto zeroTimer = convertExpression(
            makeIntLiteral(
                0, input.sequence->identifierToken.span),
            PrimitiveType::Long,
            input.sequence->identifierToken.span,
            "sequence timer reset");
        result->statements.push_back(makeFieldAssignmentStatement(
            input.sequenceTimerField,
            std::move(zeroTimer),
            input.sequence->identifierToken.span));
        const auto* receiver = lookupVariable("this");
        const auto* target = input.sequence->parameters.empty()
            ? nullptr
            : lookupVariable(
                input.sequence->parameters.front().identifierToken.text);
        if (!receiver || !target) {
            diagnostics_.report(
                "RS2492", "sequence entry parameters are incomplete",
                input.sequence->identifierToken.span);
        } else {
            auto targetAssignment =
                std::make_unique<BoundMemberAssignmentExpression>();
            targetAssignment->type = input.sequenceTargetField.type;
            targetAssignment->span = input.sequence->identifierToken.span;
            targetAssignment->receiver = makeVariableAccess(
                *receiver, input.sequence->identifierToken.span);
            targetAssignment->ownerType = *currentOwnerType_;
            targetAssignment->field = input.sequenceTargetField;
            targetAssignment->expression = makeVariableAccess(
                *target, input.sequence->identifierToken.span);
            auto statement = std::make_unique<BoundExpressionStatement>();
            statement->span = input.sequence->identifierToken.span;
            statement->expression = std::move(targetAssignment);
            result->statements.push_back(std::move(statement));
        }
        result->statements.push_back(makeStateAssignment(
            0, input.sequence->identifierToken.span));
        result->statements.push_back(makeFieldAssignmentStatement(
            input.sequenceCompletedField,
            makeBoolLiteral(
                false, input.sequence->identifierToken.span),
            input.sequence->identifierToken.span));
    }
    auto body = transformBlock(input.sequence->body);
    result->statements.insert(
        result->statements.end(),
        std::make_move_iterator(body->statements.begin()),
        std::make_move_iterator(body->statements.end()));
    result->statements.push_back(makeStateAssignment(
        0, input.sequence->body.closeBraceToken.span));
    result->statements.push_back(makeFieldAssignmentStatement(
        input.sequenceCompletedField,
        makeBoolLiteral(
            true, input.sequence->body.closeBraceToken.span),
        input.sequence->body.closeBraceToken.span));
    return result;
}

std::unique_ptr<BoundBlockStatement> Binder::bindSequenceSegment(
    const FunctionBindingInput& input) {
    if (input.sequenceStateMachine) {
        return bindStateMachineSequence(input);
    }
    if (input.sequenceSingleYieldLoop) {
        return bindSingleYieldLoopSequence(input);
    }
    if (input.sequenceSingleYieldBranch) {
        return bindSingleYieldBranchSequence(input);
    }
    auto result = std::make_unique<BoundBlockStatement>();
    if (!input.sequence) return result;
    result->span = input.sequence->body.span();

    std::vector<std::size_t> yields;
    for (std::size_t index = 0;
         index < input.sequence->body.statements.size();
         ++index) {
        if (input.sequence->body.statements[index]->kind() ==
            syntax::SyntaxKind::YieldWaitStatement) {
            yields.push_back(index);
        }
    }
    const auto segmentCount = yields.size() + 1;
    if (input.sequenceSegment >= segmentCount) {
        diagnostics_.report(
            "RS2492",
            "invalid compiler sequence segment",
            input.sequence->identifierToken.span);
        return result;
    }

    const auto begin = input.sequenceSegment == 0
        ? std::size_t{0}
        : yields[input.sequenceSegment - 1] + 1;
    const auto end = input.sequenceSegment < yields.size()
        ? yields[input.sequenceSegment]
        : input.sequence->body.statements.size();

    if (input.sequenceSegment == 0) {
        appendSequenceRestartCancellation(*result, input);
        const auto* receiver = lookupVariable("this");
        const auto* target = input.sequence->parameters.empty()
            ? nullptr
            : lookupVariable(
                input.sequence->parameters.front().identifierToken.text);
        if (!receiver || !target || !currentOwnerType_) {
            diagnostics_.report(
                "RS2492",
                "sequence entry parameters are incomplete",
                input.sequence->identifierToken.span);
        } else if (currentOwnerType_->kind == TypeKind::Struct) {
            diagnostics_.report(
                "RS2491",
                "sequence methods require a class owner",
                input.sequence->identifierToken.span);
        } else {
            auto assignment =
                std::make_unique<BoundMemberAssignmentExpression>();
            assignment->type = input.sequenceTargetField.type;
            assignment->typeName = input.sequenceTargetField.typeName;
            assignment->span = input.sequence->identifierToken.span;
            assignment->receiver = makeVariableAccess(
                *receiver,
                input.sequence->identifierToken.span);
            assignment->ownerType = *currentOwnerType_;
            assignment->field = input.sequenceTargetField;
            assignment->expression = makeVariableAccess(
                *target,
                input.sequence->parameters.front().identifierToken.span);
            auto statement = std::make_unique<BoundExpressionStatement>();
            statement->span = assignment->span;
            statement->expression = std::move(assignment);
            result->statements.push_back(std::move(statement));
        }
    }

    for (std::size_t index = begin; index < end; ++index) {
        if (input.sequence->body.statements[index]->kind() ==
            syntax::SyntaxKind::YieldBreakStatement) {
            break;
        }
        result->statements.push_back(
            bindStatement(*input.sequence->body.statements[index]));
    }

    if (input.sequenceSegment < yields.size()) {
        const auto& yieldSyntax =
            static_cast<const syntax::YieldWaitStatementSyntax&>(
                *input.sequence->body.statements[yields[input.sequenceSegment]]);
        const auto* schedule = findScheduleFunction();
        if (!schedule || !input.sequenceNextCallback) {
            diagnostics_.report(
                "RS2493",
                "sequence requires imported RealScript.Game.Schedule",
                yieldSyntax.span());
        } else {
            auto call = std::make_unique<BoundCallExpression>();
            call->type = schedule->returnType;
            call->typeName = schedule->returnTypeName;
            call->span = yieldSyntax.span();
            call->function = *schedule;
            call->arguments.push_back(makeSequenceFieldAccess(
                input.sequenceTargetField,
                yieldSyntax.span()));

            auto callback = std::make_unique<BoundLiteralExpression>();
            callback->type = PrimitiveType::String;
            callback->span = yieldSyntax.waitTicksToken.span;
            callback->value = input.sequenceNextCallback->name;
            call->arguments.push_back(std::move(callback));

            call->arguments.push_back(convertExpression(
                bindExpression(*yieldSyntax.delay),
                PrimitiveType::Int,
                yieldSyntax.delay->span(),
                "sequence wait_ticks delay"));

            auto statement = std::make_unique<BoundExpressionStatement>();
            statement->span = yieldSyntax.span();
            const auto* receiver = lookupVariable("this");
            if (receiver && currentOwnerType_) {
                auto assignment =
                    std::make_unique<BoundMemberAssignmentExpression>();
                assignment->type = input.sequenceTimerField.type;
                assignment->typeName = input.sequenceTimerField.typeName;
                assignment->span = yieldSyntax.span();
                assignment->receiver = makeVariableAccess(
                    *receiver, yieldSyntax.span());
                assignment->ownerType = *currentOwnerType_;
                assignment->field = input.sequenceTimerField;
                assignment->expression = std::move(call);
                statement->expression = std::move(assignment);
            } else {
                statement->expression = std::move(call);
            }
            result->statements.push_back(std::move(statement));
        }
    }
    return result;
}

std::unique_ptr<BoundBlockStatement> Binder::bindBlockStatement(
    const syntax::BlockStatementSyntax& syntaxTree,
    bool createScope) {
    if (createScope) {
        pushScope(syntaxTree.span());
    }

    auto result = std::make_unique<BoundBlockStatement>();
    result->span = syntaxTree.span();
    for (const auto& statement : syntaxTree.statements) {
        result->statements.push_back(bindStatement(*statement));
    }

    if (createScope) {
        popScope();
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindEmbeddedStatement(
    const syntax::StatementSyntax& syntaxTree) {
    pushScope(syntaxTree.span());
    auto result = bindStatement(syntaxTree);
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindStatement(
    const syntax::StatementSyntax& syntaxTree) {
    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::BlockStatement:
        return bindBlockStatement(
            static_cast<const syntax::BlockStatementSyntax&>(syntaxTree),
            true);
    case syntax::SyntaxKind::ReturnStatement:
        return bindReturnStatement(
            static_cast<const syntax::ReturnStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::IfStatement:
        return bindIfStatement(
            static_cast<const syntax::IfStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::WhileStatement:
        return bindWhileStatement(
            static_cast<const syntax::WhileStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ForStatement:
        return bindForStatement(
            static_cast<const syntax::ForStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ForeachStatement:
        return bindForeachStatement(
            static_cast<const syntax::ForeachStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::DoWhileStatement:
        return bindDoWhileStatement(
            static_cast<const syntax::DoWhileStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::BreakStatement:
        return bindBreakStatement(
            static_cast<const syntax::BreakStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ContinueStatement:
        return bindContinueStatement(
            static_cast<const syntax::ContinueStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::SwitchStatement:
        return bindSwitchStatement(
            static_cast<const syntax::SwitchStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ThrowStatement:
        return bindThrowStatement(
            static_cast<const syntax::ThrowStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::TryStatement:
        return bindTryStatement(
            static_cast<const syntax::TryStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::YieldWaitStatement: {
        diagnostics_.report(
            "RS2494",
            "yield wait_ticks is valid only at sequence top level",
            syntaxTree.span());
        auto result = std::make_unique<BoundExpressionStatement>();
        result->span = syntaxTree.span();
        result->expression = makeError(syntaxTree.span());
        return result;
    }
    case syntax::SyntaxKind::YieldBreakStatement: {
        diagnostics_.report(
            "RS2494",
            "yield break is valid only inside a sequence",
            syntaxTree.span());
        auto result = std::make_unique<BoundBlockStatement>();
        result->span = syntaxTree.span();
        return result;
    }
    case syntax::SyntaxKind::EventSubscriptionStatement:
        return bindEventSubscriptionStatement(
            static_cast<const syntax::EventSubscriptionStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::VariableDeclarationStatement:
        return bindVariableDeclaration(
            static_cast<const syntax::VariableDeclarationStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ExpressionStatement:
        return bindExpressionStatement(
            static_cast<const syntax::ExpressionStatementSyntax&>(syntaxTree));
    default:
        diagnostics_.report(
            "RS2099",
            "unsupported statement kind",
            syntaxTree.span());
        return std::make_unique<BoundExpressionStatement>();
    }
}

std::unique_ptr<BoundStatement> Binder::bindReturnStatement(
    const syntax::ReturnStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundReturnStatement>();
    result->span = syntaxTree.span();
    if (bindingFinally_) {
        diagnostics_.report(
            "RS8930", "control cannot leave a finally block with return",
            syntaxTree.span());
    }

    if (currentReturnType_ == PrimitiveType::Void) {
        if (syntaxTree.expression) {
            diagnostics_.report(
                "RS2002",
                "void function cannot return a value",
                syntaxTree.expression->span());
            result->expression = bindExpression(*syntaxTree.expression);
        }
        return result;
    }

    if (!syntaxTree.expression) {
        diagnostics_.report(
            "RS2003",
            "function returning '" +
                std::string(primitiveTypeName(currentReturnType_)) +
                "' must return a value",
            syntaxTree.returnKeyword.span);
        return result;
    }

    if (currentReturnModifier_ == ParameterModifier::Ref) {
        if (!syntaxTree.refKeyword ||
            syntaxTree.expression->kind() !=
                syntax::SyntaxKind::NameExpression) {
            diagnostics_.report(
                "RS8827",
                "ref-returning function must return ref to a ref local or parameter",
                syntaxTree.span());
            result->expression = bindExpression(*syntaxTree.expression);
            return result;
        }
        const auto& name = static_cast<const
            syntax::NameExpressionSyntax&>(*syntaxTree.expression);
        const auto* variable = lookupVariable(name.identifierToken.text);
        if (!variable ||
            storageTypeOf(*variable) != currentStorageReturnType_ ||
            storageTypeNameOf(*variable) != currentStorageReturnTypeName_) {
            diagnostics_.report(
                "RS8828",
                "ref return target does not have compatible reference storage",
                syntaxTree.expression->span());
            result->expression = bindExpression(*syntaxTree.expression);
            return result;
        }
        auto storage = std::make_unique<BoundVariableExpression>();
        storage->span = syntaxTree.expression->span();
        storage->type = storageTypeOf(*variable);
        storage->typeName = storageTypeNameOf(*variable);
        storage->variable = *variable;
        storage->variable.type = storage->type;
        storage->variable.typeName = storage->typeName;
        result->expression = std::move(storage);
        return result;
    }
    if (syntaxTree.refKeyword) {
        diagnostics_.report(
            "RS8829", "return ref requires a ref return type",
            syntaxTree.refKeyword->span);
    }

    result->expression = bindTargetExpression(
        *syntaxTree.expression,
        currentReturnType_,
        currentReturnTypeName_,
        "return value");
    if (result->expression &&
        isExactType(currentReturnType_)) {
        result->expression->typeName = currentReturnTypeName_;
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindIfStatement(
    const syntax::IfStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundIfStatement>();
    result->span = syntaxTree.span();
    const bool patternScope = syntaxTree.condition &&
        syntaxTree.condition->kind() == syntax::SyntaxKind::TypeBinaryExpression &&
        static_cast<const syntax::TypeBinaryExpressionSyntax&>(
            *syntaxTree.condition).designationToken.has_value();
    if (patternScope) pushScope(syntaxTree.span());
    result->condition = convertExpression(
        bindExpression(*syntaxTree.condition),
        PrimitiveType::Bool,
        syntaxTree.condition->span(),
        "if condition");
    result->thenStatement = bindEmbeddedStatement(*syntaxTree.thenStatement);
    if (patternScope) popScope();
    if (syntaxTree.elseStatement) {
        result->elseStatement = bindEmbeddedStatement(*syntaxTree.elseStatement);
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindWhileStatement(
    const syntax::WhileStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundWhileStatement>();
    result->span = syntaxTree.span();
    const bool patternScope = syntaxTree.condition &&
        syntaxTree.condition->kind() == syntax::SyntaxKind::TypeBinaryExpression &&
        static_cast<const syntax::TypeBinaryExpressionSyntax&>(
            *syntaxTree.condition).designationToken.has_value();
    if (patternScope) pushScope(syntaxTree.span());
    result->condition = convertExpression(
        bindExpression(*syntaxTree.condition),
        PrimitiveType::Bool,
        syntaxTree.condition->span(),
        "while condition");
    ++loopDepth_;
    ++breakableDepth_;
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    --breakableDepth_;
    --loopDepth_;
    if (patternScope) popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindForStatement(
    const syntax::ForStatementSyntax& syntaxTree) {
    pushScope(syntaxTree.span());
    auto result = std::make_unique<BoundForStatement>();
    result->span = syntaxTree.span();
    if (syntaxTree.initializer) result->initializer = bindStatement(*syntaxTree.initializer);
    if (syntaxTree.condition) {
        result->condition = convertExpression(
            bindExpression(*syntaxTree.condition), PrimitiveType::Bool,
            syntaxTree.condition->span(), "for condition");
    } else {
        auto literal = std::make_unique<BoundLiteralExpression>();
        literal->type = PrimitiveType::Bool;
        literal->value = true;
        literal->span = syntaxTree.forKeyword.span;
        result->condition = std::move(literal);
    }
    if (syntaxTree.increment) result->increment = bindExpression(*syntaxTree.increment);
    ++loopDepth_;
    ++breakableDepth_;
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    --breakableDepth_;
    --loopDepth_;
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindForeachStatement(
    const syntax::ForeachStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundForeachStatement>();
    result->span = syntaxTree.span();
    result->collection = bindExpression(*syntaxTree.collection);

    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    const TypeSymbol* collectionType = nullptr;
    const FunctionSymbol* countMethod = nullptr;
    const FunctionSymbol* getMethod = nullptr;
    const FunctionSymbol* getEnumeratorMethod = nullptr;
    const FunctionSymbol* moveNextMethod = nullptr;
    const FunctionSymbol* currentMethod = nullptr;
    if (result->collection->type == PrimitiveType::Array) {
        if (!decodeArrayTypeName(result->collection->typeName, elementType, elementTypeName)) {
            diagnostics_.report("RS2210", "foreach array element type is invalid", syntaxTree.collection->span());
        }
    } else if (result->collection->type == PrimitiveType::Object) {
        const auto found = visibleTypes_.find(result->collection->typeName);
        if (found != visibleTypes_.end()) collectionType = &found->second;
        if (collectionType) {
            for (const auto& method : collectionType->methods) {
                const auto visibleArity = method.parameters.size() -
                    ((method.method && !method.staticMethod) ? 1u : 0u);
                if (!method.staticMethod && method.name == "Count" && visibleArity == 0 &&
                    method.returnType == PrimitiveType::Int) countMethod = &method;
                if (!method.staticMethod && method.name == "Get" && visibleArity == 1 &&
                    method.parameters.back().type == PrimitiveType::Int) getMethod = &method;
                if (!method.staticMethod && method.name == "GetEnumerator" &&
                    visibleArity == 0 && method.returnType == PrimitiveType::Object) {
                    getEnumeratorMethod = &method;
                }
            }
        }
        if (getEnumeratorMethod) {
            const auto enumeratorType = visibleTypes_.find(
                getEnumeratorMethod->returnTypeName);
            if (enumeratorType != visibleTypes_.end()) {
                for (const auto& method : enumeratorType->second.methods) {
                    const auto visibleArity = method.parameters.size() -
                        ((method.method && !method.staticMethod) ? 1u : 0u);
                    if (!method.staticMethod && method.name == "MoveNext" &&
                        visibleArity == 0 &&
                        method.returnType == PrimitiveType::Bool) {
                        moveNextMethod = &method;
                    }
                    if (!method.staticMethod && method.name == "Current" &&
                        visibleArity == 0 &&
                        method.returnType != PrimitiveType::Void) {
                        currentMethod = &method;
                    }
                }
            }
        }
        if (getEnumeratorMethod && moveNextMethod && currentMethod) {
            auto getEnumerator = std::make_unique<BoundCallExpression>();
            getEnumerator->type = getEnumeratorMethod->returnType;
            getEnumerator->typeName = getEnumeratorMethod->returnTypeName;
            getEnumerator->function = *getEnumeratorMethod;
            getEnumerator->span = syntaxTree.collection->span();
            getEnumerator->arguments.push_back(std::move(result->collection));
            result->collection = std::move(getEnumerator);
            result->usesEnumerator = true;
            elementType = currentMethod->returnType;
            elementTypeName = currentMethod->returnTypeName;
        } else if (countMethod && getMethod) {
            elementType = getMethod->returnType;
            elementTypeName = getMethod->returnTypeName;
        } else {
            diagnostics_.report(
                "RS2211",
                "foreach collection must provide GetEnumerator()/MoveNext()/Current() or Count()/Get(int)",
                syntaxTree.collection->span());
        }
    } else {
        diagnostics_.report("RS2211", "foreach requires an array or indexed collection", syntaxTree.collection->span());
    }

    pushScope(syntaxTree.span());
    result->collectionVariable.name = "$foreach_collection_" + std::to_string(nextVariableIndex_);
    result->collectionVariable.type = result->collection->type;
    result->collectionVariable.typeName = result->collection->typeName;
    result->collectionVariable.index = nextVariableIndex_++;
    result->collectionVariable.declarationSpan =
        syntaxTree.foreachKeyword.span;
    result->collectionVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->collectionVariable.index) + ":" +
        result->collectionVariable.name);
    (void)declareVariable(
        result->collectionVariable,
        syntaxTree.foreachKeyword.span);

    result->indexVariable.name = "$foreach_index_" + std::to_string(nextVariableIndex_);
    result->indexVariable.type = PrimitiveType::Int;
    result->indexVariable.index = nextVariableIndex_++;
    result->indexVariable.declarationSpan = syntaxTree.inKeyword.span;
    result->indexVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->indexVariable.index) + ":" +
        result->indexVariable.name);
    (void)declareVariable(
        result->indexVariable,
        syntaxTree.inKeyword.span);

    std::string declaredTypeName;
    result->iterationVariable.name = syntaxTree.identifierToken.text;
    result->iterationVariable.type = bindType(syntaxTree.type, false, &declaredTypeName);
    result->iterationVariable.typeName = declaredTypeName;
    result->iterationVariable.index = nextVariableIndex_++;
    result->iterationVariable.declarationSpan = syntaxTree.identifierToken.span;
    result->iterationVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->iterationVariable.index) + ":" +
        result->iterationVariable.name);
    (void)declareVariable(result->iterationVariable, syntaxTree.identifierToken.span);

    const auto variableExpression = [](const VariableSymbol& variable, text::TextSpan span) {
        auto value = std::make_unique<BoundVariableExpression>();
        value->type = variable.type;
        value->typeName = variable.typeName;
        value->variable = variable;
        value->span = span;
        return value;
    };

    if (result->collection->type == PrimitiveType::Array) {
        auto count = std::make_unique<BoundArrayLengthExpression>();
        count->type = PrimitiveType::Int;
        count->span = syntaxTree.collection->span();
        count->receiver = variableExpression(result->collectionVariable, syntaxTree.collection->span());
        result->count = std::move(count);

        auto element = std::make_unique<BoundElementAccessExpression>();
        element->type = elementType;
        element->typeName = elementTypeName;
        element->elementType = elementType;
        element->elementTypeName = elementTypeName;
        element->span = syntaxTree.identifierToken.span;
        element->receiver = variableExpression(result->collectionVariable, syntaxTree.collection->span());
        element->index = variableExpression(result->indexVariable, syntaxTree.identifierToken.span);
        result->element = std::move(element);
    } else if (result->usesEnumerator && moveNextMethod && currentMethod) {
        auto moveNext = std::make_unique<BoundCallExpression>();
        moveNext->type = moveNextMethod->returnType;
        moveNext->typeName = moveNextMethod->returnTypeName;
        moveNext->function = *moveNextMethod;
        moveNext->span = syntaxTree.collection->span();
        moveNext->arguments.push_back(variableExpression(
            result->collectionVariable, syntaxTree.collection->span()));
        result->count = std::move(moveNext);

        auto current = std::make_unique<BoundCallExpression>();
        current->type = currentMethod->returnType;
        current->typeName = currentMethod->returnTypeName;
        current->function = *currentMethod;
        current->span = syntaxTree.identifierToken.span;
        current->arguments.push_back(variableExpression(
            result->collectionVariable, syntaxTree.collection->span()));
        result->element = std::move(current);
    } else if (countMethod && getMethod) {
        auto count = std::make_unique<BoundCallExpression>();
        count->type = countMethod->returnType;
        count->typeName = countMethod->returnTypeName;
        count->function = *countMethod;
        count->span = syntaxTree.collection->span();
        count->arguments.push_back(variableExpression(result->collectionVariable, syntaxTree.collection->span()));
        result->count = std::move(count);

        auto element = std::make_unique<BoundCallExpression>();
        element->type = getMethod->returnType;
        element->typeName = getMethod->returnTypeName;
        element->function = *getMethod;
        element->span = syntaxTree.identifierToken.span;
        element->arguments.push_back(variableExpression(result->collectionVariable, syntaxTree.collection->span()));
        element->arguments.push_back(variableExpression(result->indexVariable, syntaxTree.identifierToken.span));
        result->element = std::move(element);
    } else {
        result->count = makeError(syntaxTree.collection->span());
        result->element = makeError(syntaxTree.identifierToken.span);
    }

    result->element = convertExpression(
        std::move(result->element), result->iterationVariable.type,
        syntaxTree.identifierToken.span, "foreach element",
        result->iterationVariable.typeName);

    ++loopDepth_;
    ++breakableDepth_;
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    --breakableDepth_;
    --loopDepth_;
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindDoWhileStatement(
    const syntax::DoWhileStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundDoWhileStatement>();
    result->span = syntaxTree.span();
    ++loopDepth_;
    ++breakableDepth_;
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    --breakableDepth_;
    --loopDepth_;
    result->condition = convertExpression(
        bindExpression(*syntaxTree.condition), PrimitiveType::Bool,
        syntaxTree.condition->span(), "do/while condition");
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindBreakStatement(
    const syntax::BreakStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundBreakStatement>();
    result->span = syntaxTree.span();
    if (breakableDepth_ == 0) {
        diagnostics_.report("RS2212", "break is not inside a loop or switch", syntaxTree.span());
    }
    if (bindingFinally_ && breakableDepth_ <= finallyBreakableDepth_) {
        diagnostics_.report(
            "RS8931", "control cannot leave a finally block with break",
            syntaxTree.span());
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindContinueStatement(
    const syntax::ContinueStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundContinueStatement>();
    result->span = syntaxTree.span();
    if (loopDepth_ == 0) {
        diagnostics_.report("RS2213", "continue is not inside a loop", syntaxTree.span());
    }
    if (bindingFinally_ && loopDepth_ <= finallyLoopDepth_) {
        diagnostics_.report(
            "RS8932", "control cannot leave a finally block with continue",
            syntaxTree.span());
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindSwitchStatement(
    const syntax::SwitchStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundSwitchStatement>();
    result->span = syntaxTree.span();
    result->expression = bindExpression(*syntaxTree.expression);
    pushScope(syntaxTree.span());
    result->valueVariable.name =
        "$switch_value_" + std::to_string(nextVariableIndex_);
    result->valueVariable.type = result->expression->type;
    result->valueVariable.typeName = result->expression->typeName;
    result->valueVariable.index = nextVariableIndex_++;
    result->valueVariable.id = stableTypeId(
        std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->valueVariable.index) + ":" +
        result->valueVariable.name);
    (void)declareVariable(result->valueVariable, syntaxTree.expression->span());
    ++breakableDepth_;
    for (const auto& sourceSection : syntaxTree.sections) {
        BoundSwitchSection section;
        section.span = sourceSection.span();
        pushScope(sourceSection.span());
        if (sourceSection.label) {
            section.label = convertExpression(
                bindExpression(*sourceSection.label), result->expression->type,
                sourceSection.label->span(), "switch case",
                result->expression->typeName);
        }
        if (sourceSection.patternType) {
            section.patternType = bindType(
                *sourceSection.patternType, false,
                &section.patternTypeName);
            section.patternTypeId = isExactType(section.patternType)
                ? stableTypeId(section.patternTypeName)
                : 0;
            if (!isReferenceType(result->expression->type) ||
                !isReferenceType(section.patternType)) {
                diagnostics_.report(
                    "RS8921", "switch type patterns require reference types",
                    sourceSection.span());
            }
            if (sourceSection.patternDesignation) {
                VariableSymbol variable;
                variable.name = sourceSection.patternDesignation->text;
                variable.type = section.patternType;
                variable.typeName = section.patternTypeName;
                variable.storageType = section.patternType;
                variable.storageTypeName = section.patternTypeName;
                variable.index = nextVariableIndex_++;
                if (declareVariable(
                        variable, sourceSection.patternDesignation->span)) {
                    section.patternVariable = *lookupVariable(variable.name);
                }
            }
        }
        if (sourceSection.guard) {
            section.guard = convertExpression(
                bindExpression(*sourceSection.guard), PrimitiveType::Bool,
                sourceSection.guard->span(), "switch case guard");
        }
        for (const auto& sourceStatement : sourceSection.statements) {
            section.statements.push_back(bindStatement(*sourceStatement));
        }
        popScope();
        result->sections.push_back(std::move(section));
    }
    --breakableDepth_;
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindThrowStatement(
    const syntax::ThrowStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundThrowStatement>();
    result->span = syntaxTree.span();
    if (syntaxTree.expression) {
        result->expression = bindExpression(*syntaxTree.expression);
        if (result->expression->type != PrimitiveType::Object) {
            diagnostics_.report(
                "RS8926", "throw expression must be a class object",
                syntaxTree.expression->span());
        }
    } else if (currentExceptionVariable_) {
        result->expression = makeVariableAccess(
            *currentExceptionVariable_, syntaxTree.span());
    } else {
        diagnostics_.report(
            "RS8927", "rethrow is valid only inside a catch clause",
            syntaxTree.span());
        result->expression = makeError(syntaxTree.span());
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindTryStatement(
    const syntax::TryStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundTryStatement>();
    result->span = syntaxTree.span();
    result->body = bindBlockStatement(syntaxTree.body, true);
    bool catchAllSeen = false;
    for (const auto& sourceCatch : syntaxTree.catches) {
        BoundCatchClause clause;
        clause.span = sourceCatch.span();
        if (catchAllSeen) {
            diagnostics_.report(
                "RS8928", "catch clause is unreachable after catch-all",
                sourceCatch.span());
        }
        if (sourceCatch.type) {
            clause.type = bindType(
                *sourceCatch.type, false, &clause.typeName);
            clause.typeId = isExactType(clause.type)
                ? stableTypeId(clause.typeName)
                : 0;
            if (clause.type != PrimitiveType::Object) {
                diagnostics_.report(
                    "RS8929", "catch type must be a class type",
                    sourceCatch.type->span());
            }
        } else {
            catchAllSeen = true;
            clause.type = PrimitiveType::Object;
            clause.typeId = 0;
        }
        pushScope(sourceCatch.span());
        clause.exceptionVariable.name = sourceCatch.identifierToken
            ? sourceCatch.identifierToken->text
            : "$exception_" + std::to_string(nextVariableIndex_);
        clause.exceptionVariable.type = PrimitiveType::Object;
        clause.exceptionVariable.typeName = sourceCatch.type
            ? clause.typeName : std::string{};
        clause.exceptionVariable.storageType = PrimitiveType::Object;
        clause.exceptionVariable.storageTypeName =
            clause.exceptionVariable.typeName;
        clause.exceptionVariable.index = nextVariableIndex_++;
        (void)declareVariable(
            clause.exceptionVariable,
            sourceCatch.identifierToken
                ? sourceCatch.identifierToken->span
                : sourceCatch.catchKeyword.span);
        clause.exceptionVariable =
            *lookupVariable(clause.exceptionVariable.name);
        const auto previousException = currentExceptionVariable_;
        currentExceptionVariable_ = clause.exceptionVariable;
        clause.body = bindBlockStatement(sourceCatch.body, false);
        currentExceptionVariable_ = previousException;
        popScope();
        result->catches.push_back(std::move(clause));
    }
    if (syntaxTree.finallyBody) {
        VariableSymbol pending;
        pending.name = "$finally_exception_" +
            std::to_string(nextVariableIndex_);
        pending.type = PrimitiveType::Object;
        pending.storageType = PrimitiveType::Object;
        pending.index = nextVariableIndex_++;
        (void)declareVariable(pending, syntaxTree.finallyKeyword->span);
        result->finallyExceptionVariable = *lookupVariable(pending.name);
        const auto previousFinally = bindingFinally_;
        const auto previousFinallyBreakableDepth = finallyBreakableDepth_;
        const auto previousFinallyLoopDepth = finallyLoopDepth_;
        bindingFinally_ = true;
        finallyBreakableDepth_ = breakableDepth_;
        finallyLoopDepth_ = loopDepth_;
        result->finallyBody = bindBlockStatement(
            *syntaxTree.finallyBody, true);
        bindingFinally_ = previousFinally;
        finallyBreakableDepth_ = previousFinallyBreakableDepth;
        finallyLoopDepth_ = previousFinallyLoopDepth;
    }
    return result;
}

std::unique_ptr<BoundStatement>
Binder::bindEventSubscriptionStatement(
    const syntax::EventSubscriptionStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundEventSubscriptionStatement>();
    result->span = syntaxTree.span();
    TypeSymbol eventOwner;
    std::unique_ptr<BoundExpression> receiver;
    if (syntaxTree.receiver) {
        receiver = bindExpression(*syntaxTree.receiver);
        if (!receiver || receiver->type != PrimitiveType::Object ||
            receiver->typeName.empty()) {
            diagnostics_.report(
                "RS8308",
                "event subscription receiver must be an object",
                syntaxTree.receiver->span());
            return result;
        }
        const auto owner = visibleTypes_.find(receiver->typeName);
        if (owner == visibleTypes_.end()) {
            diagnostics_.report(
                "RS8308",
                "event subscription receiver type is unavailable",
                syntaxTree.receiver->span());
            return result;
        }
        eventOwner = owner->second;
    } else {
        if (!currentOwnerType_ || currentStaticMethod_) {
            diagnostics_.report(
                "RS8308",
                "unqualified event subscriptions require an instance method",
                syntaxTree.span());
            return result;
        }
        const auto* thisVariable = lookupVariable("this");
        if (!thisVariable) return result;
        eventOwner = *currentOwnerType_;
        receiver = makeVariableAccess(
            *thisVariable, syntaxTree.span());
    }
    for (const auto& event : eventOwner.events) {
        if (event.name != syntaxTree.eventNameToken.text) continue;
        if (!isMemberAccessible(
                event.accessibility,
                event.declaringTypeId,
                eventOwner.moduleName)) {
            diagnostics_.report(
                "RS2534",
                "event '" + event.name + "' is inaccessible",
                syntaxTree.eventNameToken.span);
            return result;
        }
        const auto delegate = visibleTypes_.find(event.delegateName);
        if (delegate == visibleTypes_.end() ||
            !delegate->second.delegateType) {
            diagnostics_.report(
                "RS8309",
                "event delegate type was not resolved",
                syntaxTree.span());
            return result;
        }
        result->ownerType = eventOwner;
        result->receiver = std::move(receiver);
        result->event = event;
        result->delegateType = delegate->second;
        result->handler = bindTargetExpression(
            *syntaxTree.handler,
            PrimitiveType::Object,
            event.delegateName,
            "event handler");
        if (!result->handler ||
            result->handler->type == PrimitiveType::Error) {
            diagnostics_.report(
                "RS8305",
                "event handler does not match delegate",
                syntaxTree.handler->span());
        }
        result->adding = syntaxTree.operatorToken.kind ==
            syntax::SyntaxKind::PlusEqualsToken;
        return result;
    }
    diagnostics_.report(
        "RS8309",
        "event subscription was not resolved",
        syntaxTree.span());
    return result;
}

TypeSymbol Binder::ensureCaptureStorage(
    const VariableSymbol& variable) {
    const auto canonicalName = referenceWrapperTypeName(
        currentModuleName_, variable.type, variable.typeName);
    const auto found = visibleTypes_.find(canonicalName);
    if (found != visibleTypes_.end()) return found->second;

    TypeSymbol wrapper;
    wrapper.kind = TypeKind::Class;
    wrapper.accessibility = Accessibility::Private;
    wrapper.synthetic = true;
    wrapper.sealedType = true;
    wrapper.moduleName = currentModuleName_;
    const auto prefix = currentModuleName_.empty()
        ? std::string{}
        : currentModuleName_ + "::";
    wrapper.name = canonicalName.rfind(prefix, 0) == 0
        ? canonicalName.substr(prefix.size())
        : canonicalName;
    wrapper.id = stableTypeId(wrapper);
    wrapper.sourceName = currentSourceName_;
    wrapper.declarationSpan = variable.declarationSpan;

    FieldSymbol valueField;
    valueField.name = "Value";
    valueField.accessibility = Accessibility::Private;
    valueField.declaringTypeId = wrapper.id;
    valueField.declaringTypeName = canonicalTypeName(wrapper);
    valueField.type = variable.type;
    valueField.typeName = variable.typeName;
    valueField.index = 0;
    valueField.synthetic = true;
    valueField.sourceName = currentSourceName_;
    valueField.declarationSpan = variable.declarationSpan;
    valueField.id = stableTypeId(
        canonicalTypeName(wrapper) + "::field:Value");
    wrapper.fields.push_back(std::move(valueField));

    pendingTypes_.push_back(wrapper);
    visibleTypes_[wrapper.name] = wrapper;
    visibleTypes_[canonicalTypeName(wrapper)] = wrapper;
    return wrapper;
}

std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration(
    const syntax::VariableDeclarationStatementSyntax& syntaxTree) {
    const auto sequenceField = sequenceLocalFields_.find(
        syntaxTree.identifierToken.text);
    if (sequenceField != sequenceLocalFields_.end()) {
        auto block = std::make_unique<BoundBlockStatement>();
        block->span = syntaxTree.span();
        if (!syntaxTree.initializer) return block;
        const auto* receiver = lookupVariable("this");
        if (!receiver || !currentOwnerType_) return block;
        auto assignment = std::make_unique<BoundMemberAssignmentExpression>();
        assignment->type = sequenceField->second.type;
        assignment->typeName = sequenceField->second.typeName;
        assignment->span = syntaxTree.span();
        assignment->receiver = makeVariableAccess(*receiver, syntaxTree.span());
        assignment->ownerType = *currentOwnerType_;
        assignment->field = sequenceField->second;
        assignment->expression = bindTargetExpression(
            *syntaxTree.initializer,
            sequenceField->second.type,
            sequenceField->second.typeName,
            "sequence local initializer");
        auto statement = std::make_unique<BoundExpressionStatement>();
        statement->span = syntaxTree.span();
        statement->expression = std::move(assignment);
        block->statements.push_back(std::move(statement));
        return block;
    }
    if (syntaxTree.refKeyword) {
        auto result = std::make_unique<BoundBlockStatement>();
        result->span = syntaxTree.span();
        std::string declaredTypeName;
        const auto declaredType = bindType(
            syntaxTree.type, false, &declaredTypeName);
        if (!syntaxTree.equalsToken ||
            !syntaxTree.initializerRefKeyword ||
            !syntaxTree.initializer) {
            diagnostics_.report(
                "RS8822",
                "ref local requires '= ref' followed by a reference target",
                syntaxTree.span());
            return result;
        }
        if (syntaxTree.initializer->kind() ==
                syntax::SyntaxKind::CallExpression ||
            syntaxTree.initializer->kind() ==
                syntax::SyntaxKind::MemberCallExpression) {
            auto initializer = bindExpression(*syntaxTree.initializer);
            const auto expectedStorage = referenceWrapperTypeName(
                currentModuleName_, declaredType, declaredTypeName);
            if (!initializer ||
                initializer->type != PrimitiveType::Object ||
                initializer->typeName != expectedStorage) {
                diagnostics_.report(
                    "RS8830",
                    "ref local initializer is not a compatible ref return",
                    syntaxTree.initializer->span());
                return result;
            }
            auto declaration =
                std::make_unique<BoundVariableDeclarationStatement>();
            declaration->span = syntaxTree.span();
            declaration->variable.name = syntaxTree.identifierToken.text;
            declaration->variable.type = declaredType;
            declaration->variable.typeName = declaredTypeName;
            declaration->variable.modifier = ParameterModifier::Ref;
            declaration->variable.storageType = PrimitiveType::Object;
            declaration->variable.storageTypeName = expectedStorage;
            declaration->variable.index = nextVariableIndex_++;
            declaration->variable.declarationSpan =
                syntaxTree.identifierToken.span;
            declaration->variable.id = stableTypeId(
                std::to_string(currentFunctionId_) + "::local:" +
                std::to_string(declaration->variable.index) + ":" +
                declaration->variable.name);
            if (!declareVariable(
                    declaration->variable,
                    syntaxTree.identifierToken.span)) {
                return result;
            }
            declaration->initializer = std::move(initializer);
            return declaration;
        }
        if (syntaxTree.initializer->kind() !=
            syntax::SyntaxKind::NameExpression) {
            diagnostics_.report(
                "RS8822",
                "ref local target must be a variable or ref-returning call",
                syntaxTree.initializer->span());
            return result;
        }
        const auto& targetName = static_cast<const
            syntax::NameExpressionSyntax&>(*syntaxTree.initializer)
                .identifierToken.text;
        const auto* target = lookupVariable(targetName);
        if (!target) {
            diagnostics_.report(
                "RS8823", "ref local target is not a variable",
                syntaxTree.initializer->span());
            return result;
        }
        if (target->modifier == ParameterModifier::In) {
            diagnostics_.report(
                "RS8824", "an in parameter cannot be aliased by a ref local",
                syntaxTree.initializer->span());
            return result;
        }
        if (target->type != declaredType ||
            ((isExactType(declaredType) ||
              declaredType == PrimitiveType::Object) &&
             target->typeName != declaredTypeName)) {
            diagnostics_.report(
                "RS8825", "ref local type does not match its target",
                syntaxTree.type.span());
            return result;
        }
        auto& aliases = referenceAliasScopes_.back();
        if (aliases.find(syntaxTree.identifierToken.text) != aliases.end() ||
            scopes_.back().find(syntaxTree.identifierToken.text) !=
                scopes_.back().end()) {
            diagnostics_.report(
                "RS2202",
                "name '" + syntaxTree.identifierToken.text +
                    "' is already declared in this scope",
                syntaxTree.identifierToken.span);
            return result;
        }
        aliases.emplace(syntaxTree.identifierToken.text, *target);
        SymbolOccurrence occurrence;
        occurrence.id = target->id;
        occurrence.kind = SymbolKind::Local;
        occurrence.name = syntaxTree.identifierToken.text;
        occurrence.detail = "ref " + std::string(
            isExactType(declaredType) && !declaredTypeName.empty()
                ? declaredTypeName
                : primitiveTypeName(declaredType));
        occurrence.sourceName = currentSourceName_;
        occurrence.span = syntaxTree.identifierToken.span;
        occurrence.definition = true;
        occurrences_.push_back(std::move(occurrence));
        return result;
    }

    auto result = std::make_unique<BoundVariableDeclarationStatement>();
    result->span = syntaxTree.span();
    std::string declaredTypeName;
    result->variable.name = syntaxTree.identifierToken.text;
    const bool inferredType = syntaxTree.type.name.text == "var" &&
        !syntaxTree.type.isGeneric() && !syntaxTree.type.isNullable() &&
        !syntaxTree.type.isArray();
    std::unique_ptr<BoundExpression> inferredInitializer;
    if (inferredType) {
        if (!syntaxTree.initializer) {
            diagnostics_.report(
                "RS8901", "implicitly typed local requires an initializer",
                syntaxTree.span());
            result->variable.type = PrimitiveType::Error;
        } else if (syntaxTree.initializer->kind() ==
                   syntax::SyntaxKind::LiteralExpression &&
                   static_cast<const syntax::LiteralExpressionSyntax&>(
                       *syntaxTree.initializer).literalToken.kind ==
                       syntax::SyntaxKind::NullKeyword) {
            diagnostics_.report(
                "RS8902", "cannot infer an implicitly typed local from null",
                syntaxTree.initializer->span());
            result->variable.type = PrimitiveType::Error;
        } else {
            inferredInitializer = bindExpression(*syntaxTree.initializer);
            result->variable.type = inferredInitializer->type;
            result->variable.typeName = inferredInitializer->typeName;
            if (result->variable.type == PrimitiveType::Void ||
                result->variable.type == PrimitiveType::Error) {
                diagnostics_.report(
                    "RS8903", "initializer does not have an inferable value type",
                    syntaxTree.initializer->span());
                result->variable.type = PrimitiveType::Error;
                result->variable.typeName.clear();
            }
        }
    } else {
        result->variable.type = bindType(
            syntaxTree.type, false, &declaredTypeName);
        result->variable.typeName = declaredTypeName;
    }
    result->variable.index = nextVariableIndex_++;
    result->variable.parameter = false;
    result->variable.declarationSpan = syntaxTree.identifierToken.span;
    result->variable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->variable.index) + ":" +
        result->variable.name);

    if (capturedVariableNames_.find(result->variable.name) !=
        capturedVariableNames_.end()) {
        const auto storage = ensureCaptureStorage(result->variable);
        result->variable.storageType = PrimitiveType::Object;
        result->variable.storageTypeName = canonicalTypeName(storage);
    }

    (void)declareVariable(result->variable, syntaxTree.identifierToken.span);
    if (inferredInitializer) {
        result->initializer = std::move(inferredInitializer);
    } else if (syntaxTree.initializer && !inferredType) {
        result->initializer = bindTargetExpression(
            *syntaxTree.initializer,
            result->variable.type,
            result->variable.typeName,
            "initializer");
        if (result->initializer &&
            isExactType(result->variable.type)) {
            if (!result->initializer->typeName.empty() &&
                result->initializer->typeName != result->variable.typeName) {
                diagnostics_.report(
                    "RS2410",
                    "cannot initialize '" + result->variable.typeName +
                        "' with '" + result->initializer->typeName + "'",
                    syntaxTree.initializer->span());
            }
            result->initializer->typeName = result->variable.typeName;
        }
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindExpressionStatement(
    const syntax::ExpressionStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundExpressionStatement>();
    result->span = syntaxTree.span();
    result->expression = bindExpression(*syntaxTree.expression);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindExpression(
    const syntax::ExpressionSyntax& syntaxTree) {
    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::LiteralExpression:
        return bindLiteralExpression(
            static_cast<const syntax::LiteralExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::NameExpression:
        return bindNameExpression(
            static_cast<const syntax::NameExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ThisExpression:
        return bindThisExpression(
            static_cast<const syntax::ThisExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::BaseExpression:
        return bindBaseExpression(
            static_cast<const syntax::BaseExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::UnaryExpression:
        return bindUnaryExpression(
            static_cast<const syntax::UnaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::BinaryExpression:
        return bindBinaryExpression(
            static_cast<const syntax::BinaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::TypeBinaryExpression:
        return bindTypeBinaryExpression(
            static_cast<const syntax::TypeBinaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::TypeOfExpression:
        return bindTypeOfExpression(
            static_cast<const syntax::TypeOfExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::SwitchExpression:
        return bindSwitchExpression(
            static_cast<const syntax::SwitchExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ConditionalExpression:
        return bindConditionalExpression(
            static_cast<const syntax::ConditionalExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::AssignmentExpression:
        return bindAssignmentExpression(
            static_cast<const syntax::AssignmentExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ParenthesizedExpression:
        return bindExpression(
            *static_cast<const syntax::ParenthesizedExpressionSyntax&>(
                syntaxTree).expression);
    case syntax::SyntaxKind::CastExpression:
        return bindCastExpression(
            static_cast<const syntax::CastExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::CallExpression:
        return bindCallExpression(
            static_cast<const syntax::CallExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::MemberCallExpression:
        return bindMemberCallExpression(
            static_cast<const syntax::MemberCallExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::NewObjectExpression:
        return bindNewObjectExpression(
            static_cast<const syntax::NewObjectExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::NewArrayExpression:
        return bindNewArrayExpression(
            static_cast<const syntax::NewArrayExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ElementAccessExpression:
        return bindElementAccessExpression(
            static_cast<const syntax::ElementAccessExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::MemberAccessExpression:
        return bindMemberAccessExpression(
            static_cast<const syntax::MemberAccessExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::MemberAssignmentExpression:
        return bindMemberAssignmentExpression(
            static_cast<const syntax::MemberAssignmentExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ElementAssignmentExpression:
        return bindElementAssignmentExpression(
            static_cast<const syntax::ElementAssignmentExpressionSyntax&>(syntaxTree));
    default:
        diagnostics_.report(
            "RS2199",
            "unsupported expression kind",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
}

PrimitiveType Binder::bindType(
    const syntax::TypeSyntax& syntaxTree,
    bool allowVoid,
    std::string* typeName) {
    if (typeName) typeName->clear();
    PrimitiveType baseType = resolvePrimitiveType(syntaxTree.name.text);
    std::string baseTypeName;
    if (baseType == PrimitiveType::Error) {
        const auto found = visibleTypes_.find(syntaxTree.name.text);
        if (found != visibleTypes_.end()) {
            baseType = found->second.kind == TypeKind::Class
                ? PrimitiveType::Object
                : found->second.kind == TypeKind::Struct
                    ? PrimitiveType::Struct
                    : PrimitiveType::Enum;
            baseTypeName = canonicalTypeName(found->second);
        } else {
            diagnostics_.report(
                "RS2200",
                "unknown type '" + syntaxTree.name.text + "'",
                syntaxTree.span());
            return PrimitiveType::Error;
        }
    }

    if (syntaxTree.isArray()) {
        if (baseType == PrimitiveType::Void ||
            baseType == PrimitiveType::Array) {
            diagnostics_.report(
                "RS2201",
                "invalid array element type",
                syntaxTree.span());
            return PrimitiveType::Error;
        }
        if (typeName) *typeName = arrayTypeName(baseType, baseTypeName);
        return PrimitiveType::Array;
    }

    if (baseType == PrimitiveType::Void && !allowVoid) {
        diagnostics_.report(
            "RS2201",
            "void is not valid in this type position",
            syntaxTree.span());
        return PrimitiveType::Error;
    }
    if (typeName &&
        (baseType == PrimitiveType::Object ||
         baseType == PrimitiveType::Struct ||
         baseType == PrimitiveType::Enum ||
         baseType == PrimitiveType::Handle)) {
        *typeName = baseTypeName;
    }
    return baseType;
}

const VariableSymbol* Binder::lookupVariable(
    const std::string& name) const noexcept {
    for (std::size_t depth = scopes_.size(); depth > 0; --depth) {
        const auto alias = referenceAliasScopes_[depth - 1].find(name);
        if (alias != referenceAliasScopes_[depth - 1].end()) {
            return &alias->second;
        }
        const auto found = scopes_[depth - 1].find(name);
        if (found != scopes_[depth - 1].end()) {
            return &found->second;
        }
    }
    return nullptr;
}

bool Binder::declareVariable(VariableSymbol variable, text::TextSpan span) {
    auto& scope = scopes_.back();
    variable.declarationSpan = span;
    if (variable.scopeSpan.empty() && !scopeSpans_.empty()) {
        variable.scopeSpan = scopeSpans_.back();
    }
    if (variable.id == 0) {
        variable.id = stableTypeId(std::to_string(currentFunctionId_) +
            "::local:" + std::to_string(variable.index) + ":" + variable.name);
    }
    if (scope.find(variable.name) != scope.end()) {
        diagnostics_.report(
            "RS2202",
            "name '" + variable.name + "' is already declared in this scope",
            span);
        return false;
    }
    allVariables_.push_back(variable);
    SymbolOccurrence occurrence;
    occurrence.id = variable.id;
    occurrence.kind = variable.parameter ? SymbolKind::Parameter : SymbolKind::Local;
    occurrence.name = variable.name;
    occurrence.detail = isExactType(variable.type) && !variable.typeName.empty()
        ? variable.typeName
        : primitiveTypeName(variable.type);
    occurrence.sourceName = currentSourceName_;
    occurrence.span = span;
    occurrence.definition = true;
    occurrences_.push_back(std::move(occurrence));
    scope.emplace(variable.name, std::move(variable));
    return true;
}

void Binder::pushScope(text::TextSpan span) {
    scopes_.emplace_back();
    referenceAliasScopes_.emplace_back();
    scopeSpans_.push_back(span);
}

void Binder::popScope() {
    scopes_.pop_back();
    referenceAliasScopes_.pop_back();
    scopeSpans_.pop_back();
}

std::unique_ptr<BoundErrorExpression> Binder::makeError(
    text::TextSpan span) const {
    auto result = std::make_unique<BoundErrorExpression>();
    result->type = PrimitiveType::Error;
    result->span = span;
    return result;
}

} // namespace realscript::semantic
