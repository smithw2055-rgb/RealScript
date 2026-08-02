#include "realscript/semantic/Semantic.h"
#include "FlowAnalysis.h"

#include <unordered_set>
#include <utility>

namespace realscript::semantic {

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

    SemanticModel result;
    result.moduleName = input.moduleName;
    result.types = input.types;

    if (!input.functionBindings.empty()) {
        for (const auto& binding : input.functionBindings) {
            result.functions.push_back(bindFunction(binding));
        }
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
    scopeSpans_.clear();
    allVariables_.clear();
    pushScope(input.body ? input.body->span() : input.symbol.bodySpan);
    currentSourceName_ = input.sourceName.empty()
        ? input.symbol.sourceName
        : input.sourceName;
    currentFunctionId_ = input.symbol.id;
    loopDepth_ = 0;
    breakableDepth_ = 0;
    result.symbol.sourceName = currentSourceName_;
    currentReturnType_ = input.symbol.returnType;
    currentReturnTypeName_ = input.symbol.returnTypeName;
    nextVariableIndex_ = input.symbol.parameters.size();
    currentStaticMethod_ = input.symbol.staticMethod;
    currentConstructor_ = input.symbol.constructor;
    currentOwnerType_.reset();
    if (!input.symbol.ownerTypeName.empty()) {
        const auto found = visibleTypes_.find(input.symbol.ownerTypeName);
        if (found != visibleTypes_.end()) currentOwnerType_ = found->second;
    }

    for (std::size_t i = 0; i < input.symbol.parameters.size(); ++i) {
        auto parameter = input.symbol.parameters[i];
        if (i < input.parameterNames.size()) parameter.name = input.parameterNames[i];
        const auto span = i < input.parameterSpans.size() ? input.parameterSpans[i] : text::TextSpan{};
        parameter.declarationSpan = span;
        if (parameter.id == 0) {
            parameter.id = stableTypeId(std::to_string(currentFunctionId_) +
                "::local:" + std::to_string(parameter.index) + ":" + parameter.name);
        }
        (void)declareVariable(parameter, span);
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
    } else if (input.sequence) {
        result.body = bindSequenceSegment(input);
    } else if (input.body) {
        result.body = bindBlockStatement(*input.body, false);
    } else {
        result.body = std::make_unique<BoundBlockStatement>();
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
    currentSourceName_.clear();
    currentFunctionId_ = 0;
    allVariables_.clear();
    return result;
}

std::unique_ptr<BoundExpression> Binder::makeVariableAccess(
    const VariableSymbol& variable,
    text::TextSpan span) {
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

std::unique_ptr<BoundBlockStatement> Binder::bindSequenceSegment(
    const FunctionBindingInput& input) {
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
            statement->expression = std::move(call);
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
    case syntax::SyntaxKind::YieldWaitStatement:
        diagnostics_.report(
            "RS2494",
            "yield wait_ticks is valid only at sequence top level",
            syntaxTree.span());
        return std::make_unique<BoundExpressionStatement>();
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

    result->expression = convertExpression(
        bindExpression(*syntaxTree.expression),
        currentReturnType_,
        syntaxTree.expression->span(),
        "return value",
        currentReturnTypeName_);
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
    result->condition = convertExpression(
        bindExpression(*syntaxTree.condition),
        PrimitiveType::Bool,
        syntaxTree.condition->span(),
        "if condition");
    result->thenStatement = bindEmbeddedStatement(*syntaxTree.thenStatement);
    if (syntaxTree.elseStatement) {
        result->elseStatement = bindEmbeddedStatement(*syntaxTree.elseStatement);
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindWhileStatement(
    const syntax::WhileStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundWhileStatement>();
    result->span = syntaxTree.span();
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
            }
        }
        if (!countMethod || !getMethod) {
            diagnostics_.report("RS2211", "foreach collection must provide Count() and Get(int)", syntaxTree.collection->span());
        } else {
            elementType = getMethod->returnType;
            elementTypeName = getMethod->returnTypeName;
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
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindContinueStatement(
    const syntax::ContinueStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundContinueStatement>();
    result->span = syntaxTree.span();
    if (loopDepth_ == 0) {
        diagnostics_.report("RS2213", "continue is not inside a loop", syntaxTree.span());
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
        if (sourceSection.label) {
            section.label = convertExpression(
                bindExpression(*sourceSection.label), result->expression->type,
                sourceSection.label->span(), "switch case",
                result->expression->typeName);
        }
        pushScope(sourceSection.span());
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

std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration(
    const syntax::VariableDeclarationStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundVariableDeclarationStatement>();
    result->span = syntaxTree.span();
    std::string declaredTypeName;
    result->variable.name = syntaxTree.identifierToken.text;
    result->variable.type = bindType(syntaxTree.type, false, &declaredTypeName);
    result->variable.typeName = declaredTypeName;
    result->variable.index = nextVariableIndex_++;
    result->variable.parameter = false;
    result->variable.declarationSpan = syntaxTree.identifierToken.span;
    result->variable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->variable.index) + ":" +
        result->variable.name);

    (void)declareVariable(result->variable, syntaxTree.identifierToken.span);
    if (syntaxTree.initializer) {
        result->initializer = convertExpression(
            bindExpression(*syntaxTree.initializer),
            result->variable.type,
            syntaxTree.initializer->span(),
            "initializer",
            result->variable.typeName);
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
    case syntax::SyntaxKind::UnaryExpression:
        return bindUnaryExpression(
            static_cast<const syntax::UnaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::BinaryExpression:
        return bindBinaryExpression(
            static_cast<const syntax::BinaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::AssignmentExpression:
        return bindAssignmentExpression(
            static_cast<const syntax::AssignmentExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ParenthesizedExpression:
        return bindExpression(
            *static_cast<const syntax::ParenthesizedExpressionSyntax&>(
                syntaxTree).expression);
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
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        const auto found = it->find(name);
        if (found != it->end()) {
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
    scopeSpans_.push_back(span);
}

void Binder::popScope() {
    scopes_.pop_back();
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
