#include "realscript/mir/Mir.h"

#include <stdexcept>

namespace realscript::mir {
namespace {

bool isLiteralTrue(const semantic::BoundExpression& expression) {
    if (expression.kind() != semantic::BoundNodeKind::LiteralExpression ||
        expression.type != semantic::PrimitiveType::Bool) {
        return false;
    }
    const auto& literal =
        static_cast<const semantic::BoundLiteralExpression&>(expression);
    return std::holds_alternative<bool>(literal.value) &&
        std::get<bool>(literal.value);
}

} // namespace

Module Lowerer::lower(const semantic::SemanticModel& model) {
    Module result;
    result.name = model.moduleName;
    result.types = model.types;
    for (const auto& function : model.functions) {
        result.functions.push_back(lowerFunction(function));
    }
    return result;
}

Function Lowerer::lowerFunction(const semantic::BoundFunction& function) {
    Function result;
    result.symbolId = function.symbol.id;
    result.moduleName = function.symbol.moduleName;
    result.name = function.symbol.ownerTypeName.empty()
        ? function.symbol.name
        : function.symbol.ownerTypeName + "." + function.symbol.name;
    result.returnType = function.symbol.returnType;
    result.debugInfo.sourceName = function.symbol.sourceName;
    result.debugInfo.declaration.span = function.symbol.declarationSpan;
    result.debugInfo.body.span = function.body ? function.body->span : function.symbol.bodySpan;
    result.returnTypeId =
        semantic::isExactType(function.symbol.returnType)
        ? semantic::stableTypeId(function.symbol.returnTypeName)
        : 0;
    result.localTypes.assign(
        function.variableCount,
        semantic::PrimitiveType::Error);
    result.localTypeIds.assign(function.variableCount, 0);

    for (const auto& variable : function.variables) {
        debug::LocalVariableInfo local;
        local.name = variable.name;
        local.slot = static_cast<std::uint32_t>(variable.index);
        local.type = variable.type;
        local.typeId = semantic::isExactType(variable.type)
            ? semantic::stableTypeId(variable.typeName)
            : 0;
        local.parameter = variable.parameter;
        local.declaration.span = variable.declarationSpan;
        local.scope.span = variable.scopeSpan.empty()
            ? (function.body ? function.body->span : function.symbol.bodySpan)
            : variable.scopeSpan;
        result.debugInfo.locals.push_back(std::move(local));
    }

    for (const auto& parameter : function.symbol.parameters) {
        result.parameterTypes.push_back(parameter.type);
        result.parameterTypeIds.push_back(
            semantic::isExactType(parameter.type)
                ? semantic::stableTypeId(parameter.typeName)
                : 0);
        result.localTypes.at(parameter.index) = parameter.type;
        result.localTypeIds.at(parameter.index) =
            semantic::isExactType(parameter.type)
                ? semantic::stableTypeId(parameter.typeName)
                : 0;
    }

    currentFunction_ = &result;
    nextValueId_ = 0;
    collectLocalTypes(*function.body);

    const auto entry = createBlock();
    setCurrentBlock(entry);

    for (std::size_t i = 0; i < function.symbol.parameters.size(); ++i) {
        const auto& parameter = function.symbol.parameters[i];
        const auto value = emitValue(
            Opcode::Parameter,
            parameter.type,
            {},
            {});
        auto& parameterInstruction =
            block(*currentBlockId_).instructions.back();
        parameterInstruction.integerImmediate = static_cast<std::int64_t>(i);
        parameterInstruction.resultTypeId =
            semantic::isExactType(parameter.type)
                ? semantic::stableTypeId(parameter.typeName)
                : 0;
        emitStoreLocal(parameter.index, value, {});
    }

    lowerStatement(*function.body);
    if (hasCurrentBlock() && !currentBlockTerminated() &&
        function.symbol.returnType == semantic::PrimitiveType::Void) {
        emitReturn(std::nullopt, function.body->span);
    }

    for (const auto& basicBlock : result.blocks) {
        for (std::size_t instructionIndex = 0;
             instructionIndex < basicBlock.instructions.size();
             ++instructionIndex) {
            const auto& instruction = basicBlock.instructions[instructionIndex];
            if (instruction.sourceSpan.empty()) continue;
            debug::SequencePoint point;
            point.blockId = basicBlock.id;
            point.instructionIndex = static_cast<std::uint32_t>(instructionIndex);
            point.range.span = instruction.sourceSpan;
            const auto duplicate = !result.debugInfo.sequencePoints.empty() &&
                result.debugInfo.sequencePoints.back().range.span.start == point.range.span.start &&
                result.debugInfo.sequencePoints.back().range.span.length == point.range.span.length;
            if (!duplicate) result.debugInfo.sequencePoints.push_back(std::move(point));
        }
        if (!basicBlock.terminator.sourceSpan.empty()) {
            debug::SequencePoint point;
            point.blockId = basicBlock.id;
            point.instructionIndex = static_cast<std::uint32_t>(basicBlock.instructions.size());
            point.terminator = true;
            point.range.span = basicBlock.terminator.sourceSpan;
            const auto duplicate = !result.debugInfo.sequencePoints.empty() &&
                result.debugInfo.sequencePoints.back().range.span.start == point.range.span.start &&
                result.debugInfo.sequencePoints.back().range.span.length == point.range.span.length;
            if (!duplicate) result.debugInfo.sequencePoints.push_back(std::move(point));
        }
    }

    currentFunction_ = nullptr;
    clearCurrentBlock();
    return result;
}

void Lowerer::collectLocalTypes(const semantic::BoundStatement& statement) {
    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement: {
        const auto& blockStatement =
            static_cast<const semantic::BoundBlockStatement&>(statement);
        for (const auto& child : blockStatement.statements) {
            collectLocalTypes(*child);
        }
        return;
    }
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& declaration =
            static_cast<const semantic::BoundVariableDeclarationStatement&>(
                statement);
        currentFunction_->localTypes.at(declaration.variable.index) =
            declaration.variable.type;
        currentFunction_->localTypeIds.at(declaration.variable.index) =
            semantic::isExactType(declaration.variable.type)
                ? semantic::stableTypeId(declaration.variable.typeName)
                : 0;
        return;
    }
    case semantic::BoundNodeKind::IfStatement: {
        const auto& ifStatement =
            static_cast<const semantic::BoundIfStatement&>(statement);
        collectLocalTypes(*ifStatement.thenStatement);
        if (ifStatement.elseStatement) {
            collectLocalTypes(*ifStatement.elseStatement);
        }
        return;
    }
    case semantic::BoundNodeKind::WhileStatement:
        collectLocalTypes(
            *static_cast<const semantic::BoundWhileStatement&>(statement).body);
        return;
    default:
        return;
    }
}

void Lowerer::lowerStatement(const semantic::BoundStatement& statement) {
    if (!hasCurrentBlock() || currentBlockTerminated()) {
        return;
    }

    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement: {
        const auto& blockStatement =
            static_cast<const semantic::BoundBlockStatement&>(statement);
        for (const auto& child : blockStatement.statements) {
            lowerStatement(*child);
            if (!hasCurrentBlock() || currentBlockTerminated()) {
                break;
            }
        }
        return;
    }
    case semantic::BoundNodeKind::ReturnStatement: {
        const auto& returnStatement =
            static_cast<const semantic::BoundReturnStatement&>(statement);
        if (returnStatement.expression) {
            emitReturn(
                lowerExpression(*returnStatement.expression),
                statement.span);
        } else {
            emitReturn(std::nullopt, statement.span);
        }
        return;
    }
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& declaration =
            static_cast<const semantic::BoundVariableDeclarationStatement&>(
                statement);
        if (declaration.initializer) {
            emitStoreLocal(
                declaration.variable.index,
                lowerExpression(*declaration.initializer),
                statement.span);
        }
        return;
    }
    case semantic::BoundNodeKind::ExpressionStatement: {
        const auto& expressionStatement =
            static_cast<const semantic::BoundExpressionStatement&>(statement);
        (void)lowerExpression(*expressionStatement.expression);
        return;
    }
    case semantic::BoundNodeKind::IfStatement: {
        const auto& ifStatement =
            static_cast<const semantic::BoundIfStatement&>(statement);
        const auto condition = lowerExpression(*ifStatement.condition);
        const auto thenBlock = createBlock();

        if (!ifStatement.elseStatement) {
            const auto mergeBlock = createBlock();
            emitBranch(condition, thenBlock, mergeBlock, {}, {}, statement.span);

            setCurrentBlock(thenBlock);
            lowerStatement(*ifStatement.thenStatement);
            if (hasCurrentBlock() && !currentBlockTerminated()) {
                emitJump(mergeBlock, {}, ifStatement.thenStatement->span);
            }
            setCurrentBlock(mergeBlock);
            return;
        }

        const auto elseBlock = createBlock();
        emitBranch(condition, thenBlock, elseBlock, {}, {}, statement.span);

        setCurrentBlock(thenBlock);
        lowerStatement(*ifStatement.thenStatement);
        std::optional<BlockId> thenEnd;
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            thenEnd = currentBlockId_;
        }

        setCurrentBlock(elseBlock);
        lowerStatement(*ifStatement.elseStatement);
        std::optional<BlockId> elseEnd;
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            elseEnd = currentBlockId_;
        }

        if (!thenEnd && !elseEnd) {
            clearCurrentBlock();
            return;
        }

        const auto mergeBlock = createBlock();
        if (thenEnd) {
            setCurrentBlock(*thenEnd);
            emitJump(mergeBlock, {}, ifStatement.thenStatement->span);
        }
        if (elseEnd) {
            setCurrentBlock(*elseEnd);
            emitJump(mergeBlock, {}, ifStatement.elseStatement->span);
        }
        setCurrentBlock(mergeBlock);
        return;
    }
    case semantic::BoundNodeKind::WhileStatement: {
        const auto& whileStatement =
            static_cast<const semantic::BoundWhileStatement&>(statement);
        const auto conditionBlock = createBlock();
        const auto bodyBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span);

        setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*whileStatement.condition);
        if (isLiteralTrue(*whileStatement.condition)) {
            emitJump(bodyBlock, {}, whileStatement.condition->span);
            setCurrentBlock(bodyBlock);
            lowerStatement(*whileStatement.body);
            if (hasCurrentBlock() && !currentBlockTerminated()) {
                emitJump(conditionBlock, {}, whileStatement.body->span);
            }
            clearCurrentBlock();
            return;
        }

        const auto exitBlock = createBlock();
        emitBranch(
            condition,
            bodyBlock,
            exitBlock,
            {},
            {},
            whileStatement.condition->span);
        setCurrentBlock(bodyBlock);
        lowerStatement(*whileStatement.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            emitJump(conditionBlock, {}, whileStatement.body->span);
        }
        setCurrentBlock(exitBlock);
        return;
    }
    default:
        throw std::logic_error(
            "unsupported bound statement in Phase 1C MIR lowerer");
    }
}

} // namespace realscript::mir
