#include "realscript/mir/Mir.h"

#include <algorithm>
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
    case semantic::BoundNodeKind::TryStatement: {
        const auto& value =
            static_cast<const semantic::BoundTryStatement&>(statement);
        if (value.body && containsBreakForCurrentTarget(*value.body)) {
            return true;
        }
        for (const auto& clause : value.catches) {
            if (clause.body && containsBreakForCurrentTarget(*clause.body)) {
                return true;
            }
        }
        if (value.finallyBody &&
            containsBreakForCurrentTarget(*value.finallyBody)) {
            return true;
        }
        return false;
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
    result.returnType = semantic::storageReturnTypeOf(function.symbol);
    result.debugInfo.sourceName = function.symbol.sourceName;
    result.debugInfo.declaration.span = function.symbol.declarationSpan;
    result.debugInfo.body.span = function.body ? function.body->span : function.symbol.bodySpan;
    result.returnTypeId = semantic::isExactType(result.returnType)
        ? semantic::stableTypeId(
            semantic::storageReturnTypeNameOf(function.symbol)) : 0;
    result.localTypes.assign(function.variableCount, semantic::PrimitiveType::Error);
    result.localTypeIds.assign(function.variableCount, 0);

    for (const auto& variable : function.variables) {
        const auto storageType = semantic::storageTypeOf(variable);
        const auto& storageTypeName = semantic::storageTypeNameOf(variable);
        if (variable.index < result.localTypes.size()) {
            result.localTypes[variable.index] = storageType;
            result.localTypeIds[variable.index] =
                semantic::isExactType(storageType)
                    ? semantic::stableTypeId(storageTypeName)
                    : 0;
        }
        debug::LocalVariableInfo local;
        local.name = variable.name; local.slot = static_cast<std::uint32_t>(variable.index);
        const auto debugType = storageType;
        const auto& debugTypeName = storageTypeName;
        local.type = debugType;
        local.typeId = semantic::isExactType(debugType)
            ? semantic::stableTypeId(debugTypeName)
            : 0;
        local.parameter = variable.parameter; local.declaration.span = variable.declarationSpan;
        local.scope.span = variable.scopeSpan.empty() ? (function.body ? function.body->span : function.symbol.bodySpan)
                                                      : variable.scopeSpan;
        result.debugInfo.locals.push_back(std::move(local));
    }
    for (const auto& parameter : function.symbol.parameters) {
        const auto storageType = semantic::storageTypeOf(parameter);
        const auto& storageTypeName =
            semantic::storageTypeNameOf(parameter);
        result.parameterTypes.push_back(storageType);
        result.parameterTypeIds.push_back(
            semantic::isExactType(storageType)
                ? semantic::stableTypeId(storageTypeName)
                : 0);
        result.localTypes.at(parameter.index) = storageType;
        result.localTypeIds.at(parameter.index) =
            semantic::isExactType(storageType)
                ? semantic::stableTypeId(storageTypeName)
                : 0;
    }

    currentFunction_ = &result; nextValueId_ = 0;
    currentAsyncFunction_ = function.symbol.asyncFunction;
    currentAsyncReturnTypeName_ = semantic::storageReturnTypeNameOf(function.symbol);
    breakTargets_.clear(); continueTargets_.clear();
    breakFinalizerDepths_.clear(); continueFinalizerDepths_.clear();
    finalizers_.clear();
    ecsWriteBacks_.clear();
    breakEcsWriteBackDepths_.clear();
    exceptionCleanupBlocks_.clear();
    finalizerCleanupBlocks_.clear();
    collectLocalTypes(*function.body);
    const auto entry = createBlock(); setCurrentBlock(entry);
    if (currentAsyncFunction_) {
        Instruction enterInst;
        enterInst.result = -1;
        enterInst.resultType = semantic::PrimitiveType::Void;
        enterInst.opcode = Opcode::AsyncEnter;
        enterInst.typeId = result.returnTypeId;
        enterInst.symbolName = currentAsyncReturnTypeName_;
        enterInst.sourceSpan = function.symbol.declarationSpan;
        block(*currentBlockId_).instructions.push_back(std::move(enterInst));
    }
    if (function.symbol.constructor && function.symbol.staticMethod) {
        const auto guardId = semantic::stableTypeId(
            function.symbol.declaringTypeName + "::cctor_guard");
        const auto hasName = "$intrinsic::static_has::" + std::to_string(guardId);
        Instruction hasCall;
        hasCall.result = nextValueId_++;
        hasCall.resultType = semantic::PrimitiveType::Bool;
        hasCall.opcode = Opcode::Call;
        hasCall.symbolId = semantic::stableTypeId(hasName);
        hasCall.symbolName = hasName;
        hasCall.sourceSpan = function.symbol.declarationSpan;
        const auto hasVal = hasCall.result;
        block(*currentBlockId_).instructions.push_back(std::move(hasCall));

        const auto returnBlock = createBlock();
        const auto runBlock = createBlock();
        emitBranch(hasVal, returnBlock, runBlock, {}, {}, function.symbol.declarationSpan);

        setCurrentBlock(returnBlock);
        emitReturn(std::nullopt, function.symbol.declarationSpan);

        setCurrentBlock(runBlock);
        const auto trueVal = emitValue(
            Opcode::ConstantBool, semantic::PrimitiveType::Bool, {}, function.symbol.declarationSpan);
        block(*currentBlockId_).instructions.back().boolImmediate = true;
        const auto setName = "$intrinsic::static_set::" + std::to_string(guardId);
        Instruction setCall;
        setCall.resultType = semantic::PrimitiveType::Void;
        setCall.opcode = Opcode::Call;
        setCall.operands = {trueVal};
        setCall.parameterTypes = {semantic::PrimitiveType::Bool};
        setCall.parameterTypeIds = {0};
        setCall.symbolId = semantic::stableTypeId(setName);
        setCall.symbolName = setName;
        setCall.sourceSpan = function.symbol.declarationSpan;
        block(*currentBlockId_).instructions.push_back(std::move(setCall));
    }
    for (std::size_t i = 0; i < function.symbol.parameters.size(); ++i) {
        const auto& parameter = function.symbol.parameters[i];
        const auto storageType = semantic::storageTypeOf(parameter);
        const auto& storageTypeName =
            semantic::storageTypeNameOf(parameter);
        const auto value = emitValue(
            Opcode::Parameter, storageType, {}, {});
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.integerImmediate = static_cast<std::int64_t>(i);
        instruction.resultTypeId = semantic::isExactType(storageType)
            ? semantic::stableTypeId(storageTypeName) : 0;
        emitStoreLocal(parameter.index, value, {});
    }
    lowerStatement(*function.body);
    const bool isVoidAsync = currentAsyncFunction_ &&
        result.returnTypeId == semantic::stableTypeId("RealScript.Async::Awaitable");
    if (hasCurrentBlock() && !currentBlockTerminated() &&
        (function.symbol.returnType == semantic::PrimitiveType::Void || isVoidAsync))
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
    currentAsyncFunction_ = false; currentAsyncReturnTypeName_.clear();
    currentFunction_ = nullptr; clearCurrentBlock(); return result;
}

void Lowerer::collectLocalTypes(const semantic::BoundStatement& statement) {
    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement:
        for (const auto& child : static_cast<const semantic::BoundBlockStatement&>(statement).statements)
            collectLocalTypes(*child);
        return;
    case semantic::BoundNodeKind::EventSubscriptionStatement:
        return;
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& value = static_cast<const semantic::BoundVariableDeclarationStatement&>(statement);
        const auto storageType = semantic::storageTypeOf(value.variable);
        const auto& storageTypeName =
            semantic::storageTypeNameOf(value.variable);
        currentFunction_->localTypes.at(value.variable.index) = storageType;
        currentFunction_->localTypeIds.at(value.variable.index) =
            semantic::isExactType(storageType)
                ? semantic::stableTypeId(storageTypeName) : 0;
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
        if (value.isEcsQuery) {
            currentFunction_->localTypes.at(value.indexVariable.index) = value.indexVariable.type;
            currentFunction_->localTypeIds.at(value.indexVariable.index) = 0;
            if (value.hasEcsEntityBinding) {
                for (const auto* variable : {&value.ecsEntityCollectionVariable, &value.ecsEntityVariable}) {
                    currentFunction_->localTypes.at(variable->index) = variable->type;
                    currentFunction_->localTypeIds.at(variable->index) = semantic::isExactType(variable->type)
                        ? semantic::stableTypeId(variable->typeName) : 0;
                }
            }
            for (const auto& col : value.ecsColumns) {
                for (const auto* variable : {&col.collectionVariable, &col.iterationVariable}) {
                    currentFunction_->localTypes.at(variable->index) = variable->type;
                    currentFunction_->localTypeIds.at(variable->index) = semantic::isExactType(variable->type)
                        ? semantic::stableTypeId(variable->typeName) : 0;
                }
            }
            collectLocalTypes(*value.body);
            return;
        }
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
    case semantic::BoundNodeKind::TryStatement: {
        const auto& value = static_cast<const
            semantic::BoundTryStatement&>(statement);
        collectLocalTypes(*value.body);
        for (const auto& clause : value.catches) {
            currentFunction_->localTypes.at(clause.exceptionVariable.index) =
                semantic::PrimitiveType::Object;
            currentFunction_->localTypeIds.at(clause.exceptionVariable.index) =
                clause.typeId;
            collectLocalTypes(*clause.body);
        }
        if (value.finallyBody) collectLocalTypes(*value.finallyBody);
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
    const auto emitEcsWriteBack = [&](const semantic::BoundForeachStatement& ecsLoop, text::TextSpan span) {
        if (!hasCurrentBlock() || currentBlockTerminated()) return;
        for (std::size_t colIdx = 0; colIdx < ecsLoop.ecsColumns.size(); ++colIdx) {
            const auto& col = ecsLoop.ecsColumns[colIdx];
            if (!col.isWrite) continue;
            const auto elemTypeId = semantic::isExactType(col.elementType)
                ? semantic::stableTypeId(col.elementTypeName)
                : 0;
            const auto colArr = emitLoadLocal(col.collectionVariable, span);
            const auto curIdx = emitLoadLocal(ecsLoop.indexVariable, span);
            const auto updatedElem = emitLoadLocal(col.iterationVariable, span);
            Instruction store;
            store.resultType = semantic::PrimitiveType::Void;
            store.opcode = Opcode::StoreElement;
            store.operands = {colArr, curIdx, updatedElem};
            store.elementType = col.elementType;
            store.elementTypeId = elemTypeId;
            store.sourceSpan = span;
            block(*currentBlockId_).instructions.push_back(std::move(store));

            const std::string wbName =
                "$intrinsic::ecs_writeback::" + std::to_string(colIdx) + "::" + ecsLoop.ecsQuerySpec;
            Instruction wbCall;
            wbCall.resultType = semantic::PrimitiveType::Void;
            wbCall.opcode = Opcode::Call;
            wbCall.operands = {curIdx, updatedElem};
            wbCall.parameterTypes = {semantic::PrimitiveType::Int, col.elementType};
            wbCall.parameterTypeIds = {0, elemTypeId};
            wbCall.symbolId = semantic::stableTypeId(wbName);
            wbCall.symbolName = wbName;
            wbCall.sourceSpan = span;
            block(*currentBlockId_).instructions.push_back(std::move(wbCall));
        }
    };
    const auto emitEcsWriteBacksUntil = [&](std::size_t depth) {
        for (std::size_t index = ecsWriteBacks_.size(); index > depth; --index) {
            if (!hasCurrentBlock() || currentBlockTerminated()) break;
            emitEcsWriteBack(*ecsWriteBacks_[index - 1], statement.span);
        }
    };
    const auto emitFinalizersUntil = [&](std::size_t depth) {
        for (std::size_t index = finalizers_.size(); index > depth; --index) {
            if (!hasCurrentBlock() || currentBlockTerminated()) break;
            const auto cleanup = createBlock();
            emitJump(cleanup, {}, statement.span);
            const auto cleanupStartIdx = currentFunction_->blocks.size() - 1;
            setCurrentBlock(cleanup);
            lowerStatement(*finalizers_[index - 1]);
            if (index - 1 < finalizerCleanupBlocks_.size()) {
                for (std::size_t bIdx = cleanupStartIdx;
                     bIdx < currentFunction_->blocks.size(); ++bIdx) {
                    finalizerCleanupBlocks_[index - 1].push_back(
                        currentFunction_->blocks[bIdx].id);
                }
            }
        }
    };

    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement:
        for (const auto& child : static_cast<const semantic::BoundBlockStatement&>(statement).statements) {
            lowerStatement(*child); if (!hasCurrentBlock() || currentBlockTerminated()) break;
        }
        return;
    case semantic::BoundNodeKind::ReturnStatement: {
        const auto& value = static_cast<const semantic::BoundReturnStatement&>(statement);
        auto returned = value.expression
            ? std::optional<ValueId>{lowerExpression(*value.expression)}
            : std::nullopt;
        std::optional<std::size_t> savedReturnSlot;
        semantic::PrimitiveType savedReturnType = semantic::PrimitiveType::Void;
        semantic::SymbolId savedReturnTypeId = 0;
        if (returned.has_value() && !finalizers_.empty() &&
            hasCurrentBlock() && !currentBlockTerminated()) {
            bool foundDef = false;
            for (auto bIt = currentFunction_->blocks.rbegin();
                 bIt != currentFunction_->blocks.rend() && !foundDef;
                 ++bIt) {
                for (auto iIt = bIt->instructions.rbegin();
                     iIt != bIt->instructions.rend();
                     ++iIt) {
                    if (iIt->result == *returned) {
                        savedReturnType = iIt->resultType;
                        savedReturnTypeId = iIt->resultTypeId;
                        foundDef = true;
                        break;
                    }
                }
                if (!foundDef) {
                    for (const auto& param : bIt->parameters) {
                        if (param.value == *returned) {
                            savedReturnType = param.type;
                            savedReturnTypeId = param.typeId;
                            foundDef = true;
                            break;
                        }
                    }
                }
            }
            if (!foundDef && value.expression) {
                savedReturnType = value.expression->type;
                savedReturnTypeId = semantic::isExactType(savedReturnType)
                    ? semantic::stableTypeId(value.expression->typeName)
                    : 0;
            }
            const std::size_t tmpSlot = currentFunction_->localTypes.size();
            currentFunction_->localTypes.push_back(savedReturnType);
            currentFunction_->localTypeIds.push_back(savedReturnTypeId);
            Instruction storeInst;
            storeInst.opcode = Opcode::StoreLocal;
            storeInst.localIndex = tmpSlot;
            storeInst.operands = {*returned};
            storeInst.sourceSpan = statement.span;
            block(*currentBlockId_).instructions.push_back(std::move(storeInst));
            savedReturnSlot = tmpSlot;
        }
        emitEcsWriteBacksUntil(0);
        emitFinalizersUntil(0);
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            if (savedReturnSlot.has_value()) {
                const auto loaded = emitValue(
                    Opcode::LoadLocal, savedReturnType, {}, statement.span);
                auto& loadInst = block(*currentBlockId_).instructions.back();
                loadInst.localIndex = *savedReturnSlot;
                loadInst.resultTypeId = savedReturnTypeId;
                returned = loaded;
            }
            emitReturn(returned, statement.span);
        }
        return;
    }
    case semantic::BoundNodeKind::BreakStatement:
        if (breakTargets_.empty()) throw std::logic_error("unbound break reached MIR lowering");
        emitEcsWriteBacksUntil(breakEcsWriteBackDepths_.back());
        emitFinalizersUntil(breakFinalizerDepths_.back());
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            emitJump(breakTargets_.back(), {}, statement.span);
        }
        return;
    case semantic::BoundNodeKind::ContinueStatement:
        if (continueTargets_.empty()) throw std::logic_error("unbound continue reached MIR lowering");
        emitFinalizersUntil(continueFinalizerDepths_.back());
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            emitJump(continueTargets_.back(), {}, statement.span);
        }
        return;
    case semantic::BoundNodeKind::ThrowStatement: {
        const auto& value = static_cast<const
            semantic::BoundThrowStatement&>(statement);
        emitThrow(lowerExpression(*value.expression), statement.span);
        return;
    }
    case semantic::BoundNodeKind::TryStatement: {
        const auto& value = static_cast<const
            semantic::BoundTryStatement&>(statement);
        const auto tryEntry = createBlock();
        emitJump(tryEntry, {}, statement.span);
        const auto tryStart = currentFunction_->blocks.size() - 1;
        setCurrentBlock(tryEntry);
        if (value.finallyBody) {
            finalizers_.push_back(value.finallyBody.get());
            finalizerCleanupBlocks_.emplace_back();
        }
        lowerStatement(*value.body);
        std::vector<BlockId> ownTryCleanup;
        if (value.finallyBody) {
            ownTryCleanup = std::move(finalizerCleanupBlocks_.back());
            finalizerCleanupBlocks_.pop_back();
            finalizers_.pop_back();
        }
        std::vector<BlockId> tryBlocks;
        for (std::size_t index = tryStart;
             index < currentFunction_->blocks.size(); ++index) {
            const auto id = currentFunction_->blocks[index].id;
            if (std::find(ownTryCleanup.begin(), ownTryCleanup.end(), id) ==
                ownTryCleanup.end()) {
                tryBlocks.push_back(id);
            }
        }
        std::optional<BlockId> afterBlock;
        const auto ensureAfterBlock = [&]() -> BlockId {
            if (!afterBlock.has_value()) {
                afterBlock = createBlock();
            }
            return *afterBlock;
        };
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            if (value.finallyBody) lowerStatement(*value.finallyBody);
            if (hasCurrentBlock() && !currentBlockTerminated()) {
                emitJump(ensureAfterBlock(), {}, statement.span);
            }
        }

        std::vector<std::vector<BlockId>> catchBlocks;
        for (const auto& clause : value.catches) {
            const auto handlerBlock = createBlock();
            currentFunction_->exceptionHandlers.push_back(ExceptionHandler{
                tryBlocks, handlerBlock, clause.typeId,
                clause.exceptionVariable.index});
            const auto catchStart = currentFunction_->blocks.size() - 1;
            setCurrentBlock(handlerBlock);
            if (value.finallyBody) {
                finalizers_.push_back(value.finallyBody.get());
                finalizerCleanupBlocks_.emplace_back();
            }
            lowerStatement(*clause.body);
            std::vector<BlockId> ownCatchCleanup;
            if (value.finallyBody) {
                ownCatchCleanup = std::move(finalizerCleanupBlocks_.back());
                finalizerCleanupBlocks_.pop_back();
                finalizers_.pop_back();
            }
            std::vector<BlockId> protectedCatch;
            for (std::size_t index = catchStart;
                 index < currentFunction_->blocks.size(); ++index) {
                const auto id = currentFunction_->blocks[index].id;
                if (std::find(ownCatchCleanup.begin(), ownCatchCleanup.end(), id) ==
                    ownCatchCleanup.end()) {
                    protectedCatch.push_back(id);
                }
            }
            catchBlocks.push_back(std::move(protectedCatch));
            if (hasCurrentBlock() && !currentBlockTerminated()) {
                if (value.finallyBody) lowerStatement(*value.finallyBody);
                if (hasCurrentBlock() && !currentBlockTerminated()) {
                    emitJump(ensureAfterBlock(), {}, clause.span);
                }
            }
        }
        if (value.finallyBody && value.finallyExceptionVariable) {
            const auto exceptionalFinally = createBlock();
            currentFunction_->exceptionHandlers.push_back(ExceptionHandler{
                tryBlocks, exceptionalFinally, 0,
                value.finallyExceptionVariable->index});
            for (const auto& protectedCatch : catchBlocks) {
                currentFunction_->exceptionHandlers.push_back(ExceptionHandler{
                    protectedCatch, exceptionalFinally, 0,
                    value.finallyExceptionVariable->index});
            }
            setCurrentBlock(exceptionalFinally);
            lowerStatement(*value.finallyBody);
            if (hasCurrentBlock() && !currentBlockTerminated()) {
                emitThrow(
                    emitLoadLocal(
                        *value.finallyExceptionVariable, statement.span),
                    statement.span);
            }
        }
        if (afterBlock.has_value()) {
            setCurrentBlock(*afterBlock);
        } else {
            clearCurrentBlock();
        }
        return;
    }
    case semantic::BoundNodeKind::EventSubscriptionStatement: {
        const auto& subscription = static_cast<const
            semantic::BoundEventSubscriptionStatement&>(statement);
        auto receiver = lowerExpression(*subscription.receiver);
        receiver = emitValue(
            Opcode::CheckNotNull,
            semantic::PrimitiveType::Object,
            {receiver},
            statement.span);
        auto& check = block(*currentBlockId_).instructions.back();
        check.typeId = subscription.ownerType.id;
        check.resultTypeId = subscription.ownerType.id;
        check.symbolName = semantic::canonicalTypeName(
            subscription.ownerType);
        const auto current = emitValue(
            Opcode::LoadField,
            semantic::PrimitiveType::Object,
            {receiver}, statement.span);
        auto& load = block(*currentBlockId_).instructions.back();
        load.typeId = subscription.ownerType.id;
        load.fieldIndex = subscription.event.storageField.index;
        load.resultTypeId = subscription.delegateType.id;
        load.symbolName = semantic::canonicalTypeName(
            subscription.ownerType);
        const auto handler = lowerExpression(*subscription.handler);
        const auto combined = emitValue(
            subscription.adding
                ? Opcode::CombineDelegate
                : Opcode::RemoveDelegate,
            semantic::PrimitiveType::Object,
            {current, handler}, statement.span);
        auto& combine = block(*currentBlockId_).instructions.back();
        combine.typeId = subscription.delegateType.id;
        combine.resultTypeId = subscription.delegateType.id;
        combine.symbolName = semantic::canonicalTypeName(
            subscription.delegateType);
        Instruction store;
        store.resultType = semantic::PrimitiveType::Void;
        store.opcode = Opcode::StoreField;
        store.operands = {receiver, combined};
        store.typeId = subscription.ownerType.id;
        store.fieldIndex = subscription.event.storageField.index;
        store.symbolName = semantic::canonicalTypeName(
            subscription.ownerType);
        store.sourceSpan = statement.span;
        block(*currentBlockId_).instructions.push_back(
            std::move(store));
        return;
    }
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& value = static_cast<const semantic::BoundVariableDeclarationStatement&>(statement);
        if (value.initializer) {
            const auto initializer = lowerExpression(*value.initializer);
            if (semantic::storageTypeOf(value.variable) !=
                    value.variable.type &&
                value.variable.modifier ==
                    semantic::ParameterModifier::None) {
                const auto cell = emitValue(
                    Opcode::NewObject,
                    semantic::PrimitiveType::Object,
                    {}, statement.span);
                auto& allocation =
                    block(*currentBlockId_).instructions.back();
                allocation.typeId = semantic::stableTypeId(
                    semantic::storageTypeNameOf(value.variable));
                allocation.resultTypeId = allocation.typeId;
                allocation.symbolName =
                    semantic::storageTypeNameOf(value.variable);
                Instruction storeValue;
                storeValue.resultType = semantic::PrimitiveType::Void;
                storeValue.opcode = Opcode::StoreField;
                storeValue.operands = {cell, initializer};
                storeValue.typeId = allocation.typeId;
                storeValue.fieldIndex = 0;
                storeValue.symbolName = allocation.symbolName;
                storeValue.sourceSpan = statement.span;
                block(*currentBlockId_).instructions.push_back(
                    std::move(storeValue));
                emitStoreLocal(
                    value.variable.index, cell, statement.span);
            } else {
                emitStoreLocal(
                    value.variable.index, initializer, statement.span);
            }
        } else if (value.variable.modifier == semantic::ParameterModifier::None) {
            if (semantic::storageTypeOf(value.variable) != value.variable.type) {
                const auto cell = emitValue(
                    Opcode::NewObject,
                    semantic::PrimitiveType::Object,
                    {}, statement.span);
                auto& allocation =
                    block(*currentBlockId_).instructions.back();
                allocation.typeId = semantic::stableTypeId(
                    semantic::storageTypeNameOf(value.variable));
                allocation.resultTypeId = allocation.typeId;
                allocation.symbolName =
                    semantic::storageTypeNameOf(value.variable);
                const auto cellTypeId = allocation.typeId;
                const auto cellSymbolName = allocation.symbolName;
                if (value.variable.type == semantic::PrimitiveType::Struct &&
                    !value.variable.typeName.empty()) {
                    const auto defaultStruct = emitValue(
                        Opcode::NewStruct,
                        semantic::PrimitiveType::Struct,
                        {}, statement.span);
                    auto& structInst = block(*currentBlockId_).instructions.back();
                    structInst.typeId = semantic::stableTypeId(value.variable.typeName);
                    structInst.resultTypeId = structInst.typeId;
                    structInst.symbolName = value.variable.typeName;
                    Instruction storeValue;
                    storeValue.resultType = semantic::PrimitiveType::Void;
                    storeValue.opcode = Opcode::StoreField;
                    storeValue.operands = {cell, defaultStruct};
                    storeValue.typeId = cellTypeId;
                    storeValue.fieldIndex = 0;
                    storeValue.symbolName = cellSymbolName;
                    storeValue.sourceSpan = statement.span;
                    block(*currentBlockId_).instructions.push_back(
                        std::move(storeValue));
                }
                emitStoreLocal(value.variable.index, cell, statement.span);
            } else if (value.variable.type == semantic::PrimitiveType::Struct &&
                       !value.variable.typeName.empty()) {
                const auto defaultStruct = emitValue(
                    Opcode::NewStruct,
                    semantic::PrimitiveType::Struct,
                    {}, statement.span);
                auto& structInst = block(*currentBlockId_).instructions.back();
                structInst.typeId = semantic::stableTypeId(value.variable.typeName);
                structInst.resultTypeId = structInst.typeId;
                structInst.symbolName = value.variable.typeName;
                emitStoreLocal(value.variable.index, defaultStruct, statement.span);
            }
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
        std::optional<BlockId> thenEnd;
        if (currentBlockId_ && !currentBlockTerminated()) {
            thenEnd = currentBlockId_;
        }
        setCurrentBlock(elseBlock); lowerStatement(*value.elseStatement);
        std::optional<BlockId> elseEnd;
        if (currentBlockId_ && !currentBlockTerminated()) {
            elseEnd = currentBlockId_;
        }
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
        if (exitBlock) { breakTargets_.push_back(*exitBlock); breakFinalizerDepths_.push_back(finalizers_.size()); breakEcsWriteBackDepths_.push_back(ecsWriteBacks_.size()); }
        continueTargets_.push_back(conditionBlock); continueFinalizerDepths_.push_back(finalizers_.size());
        setCurrentBlock(bodyBlock);
        lowerStatement(*value.body);
        continueTargets_.pop_back(); continueFinalizerDepths_.pop_back();
        if (exitBlock) { breakTargets_.pop_back(); breakFinalizerDepths_.pop_back(); breakEcsWriteBackDepths_.pop_back(); }
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
        if (exitBlock) { breakTargets_.push_back(*exitBlock); breakFinalizerDepths_.push_back(finalizers_.size()); breakEcsWriteBackDepths_.push_back(ecsWriteBacks_.size()); }
        continueTargets_.push_back(incrementBlock); continueFinalizerDepths_.push_back(finalizers_.size());
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
        continueTargets_.pop_back(); continueFinalizerDepths_.pop_back();
        if (exitBlock) { breakTargets_.pop_back(); breakFinalizerDepths_.pop_back(); breakEcsWriteBackDepths_.pop_back(); }
        if (exitBlock) setCurrentBlock(*exitBlock);
        else clearCurrentBlock();
        return;
    }
    case semantic::BoundNodeKind::ForeachStatement: {
        const auto& value = static_cast<const semantic::BoundForeachStatement&>(statement);
        if (value.isEcsQuery) {
            if (value.ecsColumns.empty()) return;
            if (value.hasEcsEntityBinding) {
                const auto entArrayTypeId = semantic::stableTypeId(value.ecsEntityCollectionVariable.typeName);
                const std::string entGetName = "$intrinsic::static_get::ecs_ent::" + value.ecsQuerySpec;
                Instruction entGetCall;
                entGetCall.result = nextValueId_++;
                entGetCall.resultType = semantic::PrimitiveType::Array;
                entGetCall.resultTypeId = entArrayTypeId;
                entGetCall.elementType = semantic::PrimitiveType::Int;
                entGetCall.elementTypeId = 0;
                entGetCall.opcode = Opcode::Call;
                entGetCall.symbolId = semantic::stableTypeId(entGetName);
                entGetCall.symbolName = entGetName;
                entGetCall.sourceSpan = statement.span;
                const auto entColResult = entGetCall.result;
                block(*currentBlockId_).instructions.push_back(std::move(entGetCall));
                emitStoreLocal(value.ecsEntityCollectionVariable.index, entColResult, statement.span);
            }
            for (std::size_t colIdx = 0; colIdx < value.ecsColumns.size(); ++colIdx) {
                const auto& col = value.ecsColumns[colIdx];
                const auto arrayTypeId = semantic::stableTypeId(col.collectionVariable.typeName);
                const auto elemTypeId = semantic::isExactType(col.elementType)
                    ? semantic::stableTypeId(col.elementTypeName)
                    : 0;
                const std::string getName =
                    "$intrinsic::static_get::ecs_col::" + std::to_string(colIdx) + "::" + value.ecsQuerySpec;
                Instruction getCall;
                getCall.result = nextValueId_++;
                getCall.resultType = semantic::PrimitiveType::Array;
                getCall.resultTypeId = arrayTypeId;
                getCall.elementType = col.elementType;
                getCall.elementTypeId = elemTypeId;
                getCall.opcode = Opcode::Call;
                getCall.symbolId = semantic::stableTypeId(getName);
                getCall.symbolName = getName;
                getCall.sourceSpan = statement.span;
                const auto colResult = getCall.result;
                block(*currentBlockId_).instructions.push_back(std::move(getCall));
                emitStoreLocal(col.collectionVariable.index, colResult, statement.span);
            }
            emitStoreLocal(value.indexVariable.index, emitInt(0, statement.span), statement.span);
            const auto conditionBlock = createBlock(), bodyBlock = createBlock(), incrementBlock = createBlock(), exitBlock = createBlock();
            emitJump(conditionBlock, {}, statement.span);
            setCurrentBlock(conditionBlock);
            const auto index = emitLoadLocal(value.indexVariable, statement.span);
            for (std::size_t colIdx = 0; colIdx < value.ecsColumns.size(); ++colIdx) {
                const auto colArr = emitLoadLocal(
                    value.ecsColumns[colIdx].collectionVariable, statement.span);
                const auto count = emitValue(
                    Opcode::ArrayLength, semantic::PrimitiveType::Int, {colArr}, statement.span);
                const auto condition = emitValue(
                    Opcode::LessInt, semantic::PrimitiveType::Bool, {index, count}, statement.span);
                if (colIdx + 1 == value.ecsColumns.size()) {
                    emitBranch(condition, bodyBlock, exitBlock, {}, {}, statement.span);
                } else {
                    const auto nextColCheckBlock = createBlock();
                    emitBranch(condition, nextColCheckBlock, exitBlock, {}, {}, statement.span);
                    setCurrentBlock(nextColCheckBlock);
                }
            }

            breakTargets_.push_back(exitBlock);
            breakFinalizerDepths_.push_back(finalizers_.size());
            breakEcsWriteBackDepths_.push_back(ecsWriteBacks_.size());
            ecsWriteBacks_.push_back(&value);
            continueTargets_.push_back(incrementBlock);
            continueFinalizerDepths_.push_back(finalizers_.size());

            setCurrentBlock(bodyBlock);
            if (value.hasEcsEntityBinding) {
                const auto entArr = emitLoadLocal(value.ecsEntityCollectionVariable, statement.span);
                const auto curIdx = emitLoadLocal(value.indexVariable, statement.span);
                const auto entVal = emitValue(
                    Opcode::LoadElement, semantic::PrimitiveType::Int, {entArr, curIdx}, statement.span);
                auto& loadInst = block(*currentBlockId_).instructions.back();
                loadInst.resultTypeId = 0;
                loadInst.elementType = semantic::PrimitiveType::Int;
                loadInst.elementTypeId = 0;
                emitStoreLocal(value.ecsEntityVariable.index, entVal, value.ecsEntityVariable.declarationSpan);
            }
            for (const auto& col : value.ecsColumns) {
                const auto elemTypeId = semantic::isExactType(col.elementType)
                    ? semantic::stableTypeId(col.elementTypeName)
                    : 0;
                const auto colArr = emitLoadLocal(col.collectionVariable, statement.span);
                const auto curIdx = emitLoadLocal(value.indexVariable, statement.span);
                const auto elemVal = emitValue(
                    Opcode::LoadElement, col.elementType, {colArr, curIdx}, statement.span);
                auto& loadInst = block(*currentBlockId_).instructions.back();
                loadInst.resultTypeId = elemTypeId;
                loadInst.elementType = col.elementType;
                loadInst.elementTypeId = elemTypeId;
                emitStoreLocal(col.iterationVariable.index, elemVal, col.iterationVariable.declarationSpan);
            }
            lowerStatement(*value.body);
            if (hasCurrentBlock() && !currentBlockTerminated()) {
                emitJump(incrementBlock, {}, value.body->span);
            }

            setCurrentBlock(incrementBlock);
            emitEcsWriteBack(value, statement.span);
            const auto currentIndex = emitLoadLocal(value.indexVariable, statement.span);
            const auto nextIndex = emitValue(
                Opcode::AddInt, semantic::PrimitiveType::Int,
                {currentIndex, emitInt(1, statement.span)}, statement.span);
            emitStoreLocal(value.indexVariable.index, nextIndex, statement.span);
            emitJump(conditionBlock, {}, statement.span);

            ecsWriteBacks_.pop_back();
            continueTargets_.pop_back();
            continueFinalizerDepths_.pop_back();
            breakTargets_.pop_back();
            breakFinalizerDepths_.pop_back();
            breakEcsWriteBackDepths_.pop_back();
            setCurrentBlock(exitBlock);
            return;
        }
        emitStoreLocal(value.collectionVariable.index, lowerExpression(*value.collection), value.collection->span);
        emitStoreLocal(value.indexVariable.index, emitInt(0, statement.span), statement.span);
        const auto conditionBlock = createBlock(), bodyBlock = createBlock(), incrementBlock = createBlock(), exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span); setCurrentBlock(conditionBlock);
        ValueId condition = 0;
        if (value.usesEnumerator) {
            condition = lowerExpression(*value.count);
        } else {
            const auto index = emitLoadLocal(value.indexVariable, statement.span);
            const auto count = lowerExpression(*value.count);
            condition = emitValue(Opcode::LessInt, semantic::PrimitiveType::Bool,
                {index, count}, statement.span);
        }
        emitBranch(condition, bodyBlock, exitBlock, {}, {}, statement.span);
        breakTargets_.push_back(exitBlock); breakFinalizerDepths_.push_back(finalizers_.size()); breakEcsWriteBackDepths_.push_back(ecsWriteBacks_.size());
        continueTargets_.push_back(incrementBlock); continueFinalizerDepths_.push_back(finalizers_.size());
        setCurrentBlock(bodyBlock);
        emitStoreLocal(value.iterationVariable.index, lowerExpression(*value.element), value.iterationVariable.declarationSpan);
        lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(incrementBlock, {}, value.body->span);
        setCurrentBlock(incrementBlock);
        if (!value.usesEnumerator) {
            const auto currentIndex = emitLoadLocal(value.indexVariable, statement.span);
            const auto nextIndex = emitValue(Opcode::AddInt, semantic::PrimitiveType::Int,
                {currentIndex, emitInt(1, statement.span)}, statement.span);
            emitStoreLocal(value.indexVariable.index, nextIndex, statement.span);
        }
        emitJump(conditionBlock, {}, statement.span);
        continueTargets_.pop_back(); continueFinalizerDepths_.pop_back();
        breakTargets_.pop_back(); breakFinalizerDepths_.pop_back(); breakEcsWriteBackDepths_.pop_back(); setCurrentBlock(exitBlock); return;
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
        if (exitBlock) { breakTargets_.push_back(*exitBlock); breakFinalizerDepths_.push_back(finalizers_.size()); breakEcsWriteBackDepths_.push_back(ecsWriteBacks_.size()); }
        continueTargets_.push_back(conditionBlock); continueFinalizerDepths_.push_back(finalizers_.size());
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
        continueTargets_.pop_back(); continueFinalizerDepths_.pop_back();
        if (exitBlock) { breakTargets_.pop_back(); breakFinalizerDepths_.pop_back(); breakEcsWriteBackDepths_.pop_back(); }
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
        bool exitReachable = false;
        std::vector<BlockId> sectionBlocks; sectionBlocks.reserve(value.sections.size());
        std::optional<std::size_t> defaultIndex;
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            sectionBlocks.push_back(createBlock());
            if (!value.sections[i].pattern &&
                !value.sections[i].label &&
                value.sections[i].patternType ==
                    semantic::PrimitiveType::Error) {
                defaultIndex = i;
            }
        }
        bool checksExhausted = false;
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            const auto& section = value.sections[i];
            if (!section.pattern &&
                !section.label &&
                section.patternType == semantic::PrimitiveType::Error) continue;
            if (!section.guard && section.pattern &&
                (section.pattern->kind == semantic::BoundPatternKind::Discard ||
                 section.pattern->kind == semantic::BoundPatternKind::Var)) {
                if (section.pattern->kind == semantic::BoundPatternKind::Var &&
                    section.pattern->variable) {
                    const auto switchValue =
                        emitLoadLocal(value.valueVariable, value.expression->span);
                    emitStoreLocal(
                        section.pattern->variable->index, switchValue, section.span);
                }
                emitJump(sectionBlocks[i], {}, section.span);
                checksExhausted = true;
                break;
            }
            const auto nextCheck = createBlock();
            ValueId matched = -1;
            if (section.pattern) {
                semantic::BoundTypeBinaryExpression synthTest;
                synthTest.span = section.span;
                synthTest.type = semantic::PrimitiveType::Bool;
                synthTest.safeCast = false;
                synthTest.patternInputVariable = value.valueVariable;
                synthTest.inputAlreadyInVariable = true;
                synthTest.borrowedPattern = section.pattern.get();
                matched = lowerExpression(synthTest);
            } else if (section.patternType != semantic::PrimitiveType::Error) {
                const auto switchValue =
                    emitLoadLocal(value.valueVariable, value.expression->span);
                matched = emitValue(
                    Opcode::IsType, semantic::PrimitiveType::Bool,
                    {switchValue}, section.span);
                auto& test = block(*currentBlockId_).instructions.back();
                test.elementType = section.patternType;
                test.elementTypeId = section.patternTypeId;
                test.parameterTypes = {value.expression->type};
                test.symbolName = section.patternTypeName;
                if (section.patternVariable) {
                    const auto cast = emitValue(
                        Opcode::AsType, section.patternType,
                        {switchValue}, section.span);
                    auto& asType = block(*currentBlockId_).instructions.back();
                    asType.elementType = section.patternType;
                    asType.elementTypeId = section.patternTypeId;
                    asType.parameterTypes = {value.expression->type};
                    asType.resultTypeId = section.patternTypeId;
                    asType.symbolName = section.patternTypeName;
                    emitStoreLocal(
                        section.patternVariable->index, cast, section.span);
                }
            } else {
                const auto switchValue =
                    emitLoadLocal(value.valueVariable, value.expression->span);
                const auto caseValue = lowerExpression(*section.label);
                matched = emitValue(
                    Opcode::Equal, semantic::PrimitiveType::Bool,
                    {switchValue, caseValue}, section.span);
            }
            if (section.guard) {
                const auto guardBlock = createBlock();
                emitBranch(
                    matched, guardBlock, nextCheck, {}, {}, section.span);
                setCurrentBlock(guardBlock);
                const auto guard = lowerExpression(*section.guard);
                emitBranch(
                    guard, sectionBlocks[i], nextCheck,
                    {}, {}, section.span);
            } else {
                emitBranch(
                    matched, sectionBlocks[i], nextCheck,
                    {}, {}, section.span);
            }
            setCurrentBlock(nextCheck);
        }
        if (!checksExhausted) {
            if (defaultIndex && value.sections[*defaultIndex].guard) {
                const auto& fallback = value.sections[*defaultIndex];
                const auto guard = lowerExpression(*fallback.guard);
                emitBranch(
                    guard, sectionBlocks[*defaultIndex], exitBlock,
                    {}, {}, fallback.span);
                exitReachable = true;
            } else {
                emitJump(defaultIndex ? sectionBlocks[*defaultIndex] : exitBlock, {}, statement.span);
                exitReachable = !defaultIndex.has_value();
            }
        }
        breakTargets_.push_back(exitBlock); breakFinalizerDepths_.push_back(finalizers_.size()); breakEcsWriteBackDepths_.push_back(ecsWriteBacks_.size());
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            setCurrentBlock(sectionBlocks[i]);
            for (const auto& child : value.sections[i].statements) {
                exitReachable = exitReachable ||
                    containsBreakForCurrentTarget(*child);
                lowerStatement(*child); if (!hasCurrentBlock() || currentBlockTerminated()) break;
            }
            if (hasCurrentBlock() && !currentBlockTerminated()) {
                if (value.sections[i].statements.empty() && i + 1 < value.sections.size()) {
                    emitJump(sectionBlocks[i + 1], {}, value.sections[i].span);
                } else {
                    emitJump(exitBlock, {}, value.sections[i].span);
                    exitReachable = true;
                }
            }
        }
        breakTargets_.pop_back(); breakFinalizerDepths_.pop_back(); breakEcsWriteBackDepths_.pop_back();
        if (exitReachable) setCurrentBlock(exitBlock);
        else {
            clearCurrentBlock();
            currentFunction_->blocks.erase(
                std::remove_if(
                    currentFunction_->blocks.begin(),
                    currentFunction_->blocks.end(),
                    [&](const auto& candidate) {
                        return candidate.id == exitBlock;
                    }),
                currentFunction_->blocks.end());
        }
        return;
    }
    default: throw std::logic_error("unsupported bound statement in MIR lowerer");
    }
}

} // namespace realscript::mir
