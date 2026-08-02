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

bool containsBreakForCurrentTarget(
    const semantic::BoundStatement& statement) {
    switch (statement.kind()) {
    case semantic::BoundNodeKind::BreakStatement:
        return true;
    case semantic::BoundNodeKind::BlockStatement:
        for (const auto& child :
             static_cast<const semantic::BoundBlockStatement&>(statement)
                 .statements) {
            if (containsBreakForCurrentTarget(*child)) return true;
        }
        return false;
    case semantic::BoundNodeKind::IfStatement: {
        const auto& value =
            static_cast<const semantic::BoundIfStatement&>(statement);
        return containsBreakForCurrentTarget(*value.thenStatement) ||
            (value.elseStatement &&
             containsBreakForCurrentTarget(*value.elseStatement));
    }
    case semantic::BoundNodeKind::WhileStatement:
    case semantic::BoundNodeKind::ForStatement:
    case semantic::BoundNodeKind::ForeachStatement:
    case semantic::BoundNodeKind::DoWhileStatement:
    case semantic::BoundNodeKind::SwitchStatement:
        return false;
    default:
        return false;
    }
}

} // namespace

Module Lowerer::lower(const semantic::SemanticModel& model) {
    Module result; result.name = model.moduleName; result.types = model.types;
    for (const auto& function : model.functions) result.functions.push_back(lowerFunction(function));
    return result;
}

Function Lowerer::lowerFunction(const semantic::BoundFunction& function) {
    Function result;
    result.symbolId = function.symbol.id;
    result.moduleName = function.symbol.moduleName;
    result.name = function.symbol.ownerTypeName.empty() ? function.symbol.name
        : function.symbol.ownerTypeName + "." + function.symbol.name;
    result.returnType = function.symbol.returnType;
    result.debugInfo.sourceName = function.symbol.sourceName;
    result.debugInfo.declaration.span = function.symbol.declarationSpan;
    result.debugInfo.body.span = function.body ? function.body->span : function.symbol.bodySpan;
    result.returnTypeId = semantic::isExactType(function.symbol.returnType)
        ? semantic::stableTypeId(function.symbol.returnTypeName) : 0;
    result.localTypes.assign(function.variableCount, semantic::PrimitiveType::Error);
    result.localTypeIds.assign(function.variableCount, 0);

    for (const auto& variable : function.variables) {
        debug::LocalVariableInfo local;
        local.name = variable.name; local.slot = static_cast<std::uint32_t>(variable.index);
        local.type = variable.type;
        local.typeId = semantic::isExactType(variable.type) ? semantic::stableTypeId(variable.typeName) : 0;
        local.parameter = variable.parameter; local.declaration.span = variable.declarationSpan;
        local.scope.span = variable.scopeSpan.empty() ? (function.body ? function.body->span : function.symbol.bodySpan)
                                                      : variable.scopeSpan;
        result.debugInfo.locals.push_back(std::move(local));
    }
    for (const auto& parameter : function.symbol.parameters) {
        result.parameterTypes.push_back(parameter.type);
        result.parameterTypeIds.push_back(semantic::isExactType(parameter.type)
            ? semantic::stableTypeId(parameter.typeName) : 0);
        result.localTypes.at(parameter.index) = parameter.type;
        result.localTypeIds.at(parameter.index) = semantic::isExactType(parameter.type)
            ? semantic::stableTypeId(parameter.typeName) : 0;
    }

    currentFunction_ = &result; nextValueId_ = 0; breakTargets_.clear(); continueTargets_.clear();
    collectLocalTypes(*function.body);
    const auto entry = createBlock(); setCurrentBlock(entry);
    for (std::size_t i = 0; i < function.symbol.parameters.size(); ++i) {
        const auto& parameter = function.symbol.parameters[i];
        const auto value = emitValue(Opcode::Parameter, parameter.type, {}, {});
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.integerImmediate = static_cast<std::int64_t>(i);
        instruction.resultTypeId = semantic::isExactType(parameter.type)
            ? semantic::stableTypeId(parameter.typeName) : 0;
        emitStoreLocal(parameter.index, value, {});
    }
    lowerStatement(*function.body);
    if (hasCurrentBlock() && !currentBlockTerminated() && function.symbol.returnType == semantic::PrimitiveType::Void)
        emitReturn(std::nullopt, function.body->span);

    for (const auto& basicBlock : result.blocks) {
        for (std::size_t i = 0; i < basicBlock.instructions.size(); ++i) {
            const auto& instruction = basicBlock.instructions[i];
            if (instruction.sourceSpan.empty()) continue;
            debug::SequencePoint point; point.blockId = basicBlock.id;
            point.instructionIndex = static_cast<std::uint32_t>(i); point.range.span = instruction.sourceSpan;
            const auto duplicate = !result.debugInfo.sequencePoints.empty() &&
                result.debugInfo.sequencePoints.back().range.span.start == point.range.span.start &&
                result.debugInfo.sequencePoints.back().range.span.length == point.range.span.length;
            if (!duplicate) result.debugInfo.sequencePoints.push_back(std::move(point));
        }
        if (!basicBlock.terminator.sourceSpan.empty()) {
            debug::SequencePoint point; point.blockId = basicBlock.id;
            point.instructionIndex = static_cast<std::uint32_t>(basicBlock.instructions.size());
            point.terminator = true; point.range.span = basicBlock.terminator.sourceSpan;
            const auto duplicate = !result.debugInfo.sequencePoints.empty() &&
                result.debugInfo.sequencePoints.back().range.span.start == point.range.span.start &&
                result.debugInfo.sequencePoints.back().range.span.length == point.range.span.length;
            if (!duplicate) result.debugInfo.sequencePoints.push_back(std::move(point));
        }
    }
    currentFunction_ = nullptr; clearCurrentBlock(); return result;
}

void Lowerer::collectLocalTypes(const semantic::BoundStatement& statement) {
    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement:
        for (const auto& child : static_cast<const semantic::BoundBlockStatement&>(statement).statements)
            collectLocalTypes(*child);
        return;
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& value = static_cast<const semantic::BoundVariableDeclarationStatement&>(statement);
        currentFunction_->localTypes.at(value.variable.index) = value.variable.type;
        currentFunction_->localTypeIds.at(value.variable.index) = semantic::isExactType(value.variable.type)
            ? semantic::stableTypeId(value.variable.typeName) : 0;
        return;
    }
    case semantic::BoundNodeKind::IfStatement: {
        const auto& value = static_cast<const semantic::BoundIfStatement&>(statement);
        collectLocalTypes(*value.thenStatement); if (value.elseStatement) collectLocalTypes(*value.elseStatement); return;
    }
    case semantic::BoundNodeKind::WhileStatement:
        collectLocalTypes(*static_cast<const semantic::BoundWhileStatement&>(statement).body); return;
    case semantic::BoundNodeKind::ForStatement: {
        const auto& value =
            static_cast<const semantic::BoundForStatement&>(statement);
        if (value.initializer) {
            collectLocalTypes(*value.initializer);
        }
        collectLocalTypes(*value.body);
        return;
    }
    case semantic::BoundNodeKind::ForeachStatement: {
        const auto& value = static_cast<const semantic::BoundForeachStatement&>(statement);
        for (const auto* variable : {&value.collectionVariable, &value.indexVariable, &value.iterationVariable}) {
            currentFunction_->localTypes.at(variable->index) = variable->type;
            currentFunction_->localTypeIds.at(variable->index) = semantic::isExactType(variable->type)
                ? semantic::stableTypeId(variable->typeName) : 0;
        }
        collectLocalTypes(*value.body); return;
    }
    case semantic::BoundNodeKind::DoWhileStatement:
        collectLocalTypes(*static_cast<const semantic::BoundDoWhileStatement&>(statement).body); return;
    case semantic::BoundNodeKind::SwitchStatement: {
        const auto& value =
            static_cast<const semantic::BoundSwitchStatement&>(statement);
        currentFunction_->localTypes.at(value.valueVariable.index) =
            value.valueVariable.type;
        currentFunction_->localTypeIds.at(value.valueVariable.index) =
            semantic::isExactType(value.valueVariable.type)
                ? semantic::stableTypeId(value.valueVariable.typeName)
                : 0;
        for (const auto& section : value.sections) {
            for (const auto& child : section.statements) {
                collectLocalTypes(*child);
            }
        }
        return;
    }
    default: return;
    }
}

void Lowerer::lowerStatement(const semantic::BoundStatement& statement) {
    if (!hasCurrentBlock() || currentBlockTerminated()) return;
    const auto emitLoadLocal = [&](const semantic::VariableSymbol& variable, text::TextSpan span) {
        const auto value = emitValue(Opcode::LoadLocal, variable.type, {}, span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.localIndex = variable.index;
        instruction.resultTypeId = semantic::isExactType(variable.type)
            ? semantic::stableTypeId(variable.typeName) : 0;
        return value;
    };
    const auto emitInt = [&](std::int64_t immediate, text::TextSpan span) {
        const auto value = emitValue(Opcode::ConstantInt, semantic::PrimitiveType::Int, {}, span);
        block(*currentBlockId_).instructions.back().integerImmediate = immediate; return value;
    };

    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement:
        for (const auto& child : static_cast<const semantic::BoundBlockStatement&>(statement).statements) {
            lowerStatement(*child); if (!hasCurrentBlock() || currentBlockTerminated()) break;
        }
        return;
    case semantic::BoundNodeKind::ReturnStatement: {
        const auto& value = static_cast<const semantic::BoundReturnStatement&>(statement);
        emitReturn(value.expression ? std::optional<ValueId>{lowerExpression(*value.expression)} : std::nullopt, statement.span); return;
    }
    case semantic::BoundNodeKind::BreakStatement:
        if (breakTargets_.empty()) throw std::logic_error("unbound break reached MIR lowering");
        emitJump(breakTargets_.back(), {}, statement.span); return;
    case semantic::BoundNodeKind::ContinueStatement:
        if (continueTargets_.empty()) throw std::logic_error("unbound continue reached MIR lowering");
        emitJump(continueTargets_.back(), {}, statement.span); return;
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& value = static_cast<const semantic::BoundVariableDeclarationStatement&>(statement);
        if (value.initializer) {
            emitStoreLocal(
                value.variable.index,
                lowerExpression(*value.initializer),
                statement.span);
        }
        return;
    }
    case semantic::BoundNodeKind::ExpressionStatement:
        (void)lowerExpression(*static_cast<const semantic::BoundExpressionStatement&>(statement).expression); return;
    case semantic::BoundNodeKind::IfStatement: {
        const auto& value = static_cast<const semantic::BoundIfStatement&>(statement);
        const auto condition = lowerExpression(*value.condition); const auto thenBlock = createBlock();
        if (!value.elseStatement) {
            const auto merge = createBlock(); emitBranch(condition, thenBlock, merge, {}, {}, statement.span);
            setCurrentBlock(thenBlock); lowerStatement(*value.thenStatement);
            if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(merge, {}, value.thenStatement->span);
            setCurrentBlock(merge); return;
        }
        const auto elseBlock = createBlock(); emitBranch(condition, thenBlock, elseBlock, {}, {}, statement.span);
        setCurrentBlock(thenBlock); lowerStatement(*value.thenStatement);
        std::optional<BlockId> thenEnd = hasCurrentBlock() && !currentBlockTerminated() ? currentBlockId_ : std::nullopt;
        setCurrentBlock(elseBlock); lowerStatement(*value.elseStatement);
        std::optional<BlockId> elseEnd = hasCurrentBlock() && !currentBlockTerminated() ? currentBlockId_ : std::nullopt;
        if (!thenEnd && !elseEnd) { clearCurrentBlock(); return; }
        const auto merge = createBlock();
        if (thenEnd) { setCurrentBlock(*thenEnd); emitJump(merge, {}, value.thenStatement->span); }
        if (elseEnd) { setCurrentBlock(*elseEnd); emitJump(merge, {}, value.elseStatement->span); }
        setCurrentBlock(merge); return;
    }
    case semantic::BoundNodeKind::WhileStatement: {
        const auto& value =
            static_cast<const semantic::BoundWhileStatement&>(statement);
        const auto constantTrue = isLiteralTrue(*value.condition);
        const auto needsExit =
            !constantTrue || containsBreakForCurrentTarget(*value.body);
        const auto conditionBlock = createBlock();
        const auto bodyBlock = createBlock();
        const auto exitBlock = needsExit
            ? std::optional<BlockId>{createBlock()}
            : std::nullopt;
        emitJump(conditionBlock, {}, statement.span);
        setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*value.condition);
        if (constantTrue) {
            emitJump(bodyBlock, {}, value.condition->span);
        } else {
            emitBranch(
                condition,
                bodyBlock,
                *exitBlock,
                {},
                {},
                value.condition->span);
        }
        if (exitBlock) breakTargets_.push_back(*exitBlock);
        continueTargets_.push_back(conditionBlock);
        setCurrentBlock(bodyBlock);
        lowerStatement(*value.body);
        continueTargets_.pop_back();
        if (exitBlock) breakTargets_.pop_back();
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            emitJump(conditionBlock, {}, value.body->span);
        }
        if (exitBlock) setCurrentBlock(*exitBlock);
        else clearCurrentBlock();
        return;
    }
    case semantic::BoundNodeKind::ForStatement: {
        const auto& value =
            static_cast<const semantic::BoundForStatement&>(statement);
        if (value.initializer) lowerStatement(*value.initializer);
        if (!hasCurrentBlock() || currentBlockTerminated()) return;
        const auto constantTrue = isLiteralTrue(*value.condition);
        const auto needsExit =
            !constantTrue || containsBreakForCurrentTarget(*value.body);
        const auto conditionBlock = createBlock();
        const auto bodyBlock = createBlock();
        const auto incrementBlock = createBlock();
        const auto exitBlock = needsExit
            ? std::optional<BlockId>{createBlock()}
            : std::nullopt;
        emitJump(conditionBlock, {}, statement.span);
        setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*value.condition);
        if (constantTrue) {
            emitJump(bodyBlock, {}, value.condition->span);
        } else {
            emitBranch(
                condition,
                bodyBlock,
                *exitBlock,
                {},
                {},
                value.condition->span);
        }
        if (exitBlock) breakTargets_.push_back(*exitBlock);
        continueTargets_.push_back(incrementBlock);
        setCurrentBlock(bodyBlock);
        lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            emitJump(incrementBlock, {}, value.body->span);
        }
        setCurrentBlock(incrementBlock);
        if (value.increment) {
            (void)lowerExpression(*value.increment);
        }
        if (!currentBlockTerminated()) {
            emitJump(
                conditionBlock,
                {},
                value.increment ? value.increment->span : statement.span);
        }
        continueTargets_.pop_back();
        if (exitBlock) breakTargets_.pop_back();
        if (exitBlock) setCurrentBlock(*exitBlock);
        else clearCurrentBlock();
        return;
    }
    case semantic::BoundNodeKind::ForeachStatement: {
        const auto& value = static_cast<const semantic::BoundForeachStatement&>(statement);
        emitStoreLocal(value.collectionVariable.index, lowerExpression(*value.collection), value.collection->span);
        emitStoreLocal(value.indexVariable.index, emitInt(0, statement.span), statement.span);
        const auto conditionBlock = createBlock(), bodyBlock = createBlock(), incrementBlock = createBlock(), exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span); setCurrentBlock(conditionBlock);
        const auto index = emitLoadLocal(value.indexVariable, statement.span);
        const auto count = lowerExpression(*value.count);
        const auto condition = emitValue(Opcode::LessInt, semantic::PrimitiveType::Bool, {index, count}, statement.span);
        emitBranch(condition, bodyBlock, exitBlock, {}, {}, statement.span);
        breakTargets_.push_back(exitBlock); continueTargets_.push_back(incrementBlock);
        setCurrentBlock(bodyBlock);
        emitStoreLocal(value.iterationVariable.index, lowerExpression(*value.element), value.iterationVariable.declarationSpan);
        lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(incrementBlock, {}, value.body->span);
        setCurrentBlock(incrementBlock);
        const auto currentIndex = emitLoadLocal(value.indexVariable, statement.span);
        const auto nextIndex = emitValue(Opcode::AddInt, semantic::PrimitiveType::Int,
            {currentIndex, emitInt(1, statement.span)}, statement.span);
        emitStoreLocal(value.indexVariable.index, nextIndex, statement.span);
        emitJump(conditionBlock, {}, statement.span);
        continueTargets_.pop_back(); breakTargets_.pop_back(); setCurrentBlock(exitBlock); return;
    }
    case semantic::BoundNodeKind::DoWhileStatement: {
        const auto& value =
            static_cast<const semantic::BoundDoWhileStatement&>(statement);
        const auto constantTrue = isLiteralTrue(*value.condition);
        const auto needsExit =
            !constantTrue || containsBreakForCurrentTarget(*value.body);
        const auto bodyBlock = createBlock();
        const auto conditionBlock = createBlock();
        const auto exitBlock = needsExit
            ? std::optional<BlockId>{createBlock()}
            : std::nullopt;
        emitJump(bodyBlock, {}, statement.span);
        if (exitBlock) breakTargets_.push_back(*exitBlock);
        continueTargets_.push_back(conditionBlock);
        setCurrentBlock(bodyBlock);
        lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            emitJump(conditionBlock, {}, value.body->span);
        }
        setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*value.condition);
        if (constantTrue) {
            emitJump(bodyBlock, {}, value.condition->span);
        } else {
            emitBranch(
                condition,
                bodyBlock,
                *exitBlock,
                {},
                {},
                value.condition->span);
        }
        continueTargets_.pop_back();
        if (exitBlock) breakTargets_.pop_back();
        if (exitBlock) setCurrentBlock(*exitBlock);
        else clearCurrentBlock();
        return;
    }
    case semantic::BoundNodeKind::SwitchStatement: {
        const auto& value = static_cast<const semantic::BoundSwitchStatement&>(statement);
        emitStoreLocal(
            value.valueVariable.index,
            lowerExpression(*value.expression),
            value.expression->span);
        const auto exitBlock = createBlock();
        std::vector<BlockId> sectionBlocks; sectionBlocks.reserve(value.sections.size());
        std::optional<std::size_t> defaultIndex;
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            sectionBlocks.push_back(createBlock()); if (!value.sections[i].label) defaultIndex = i;
        }
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            if (!value.sections[i].label) continue;
            const auto nextCheck = createBlock();
            const auto switchValue =
                emitLoadLocal(value.valueVariable, value.expression->span);
            const auto caseValue = lowerExpression(*value.sections[i].label);
            const auto equal = emitValue(
                Opcode::Equal,
                semantic::PrimitiveType::Bool,
                {switchValue, caseValue},
                value.sections[i].span);
            emitBranch(equal, sectionBlocks[i], nextCheck, {}, {}, value.sections[i].span);
            setCurrentBlock(nextCheck);
        }
        emitJump(defaultIndex ? sectionBlocks[*defaultIndex] : exitBlock, {}, statement.span);
        breakTargets_.push_back(exitBlock);
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            setCurrentBlock(sectionBlocks[i]);
            for (const auto& child : value.sections[i].statements) {
                lowerStatement(*child); if (!hasCurrentBlock() || currentBlockTerminated()) break;
            }
            if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(exitBlock, {}, value.sections[i].span);
        }
        breakTargets_.pop_back(); setCurrentBlock(exitBlock); return;
    }
    default: throw std::logic_error("unsupported bound statement in MIR lowerer");
    }
}

} // namespace realscript::mir
