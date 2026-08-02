#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]


def patch(path: str, old: str, new: str) -> None:
    file = root / path
    text = file.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"anchor missing in {path}: {old[:100]!r}")
    file.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


patch(
    "src/semantic/SemanticBinding.cpp",
    '''    result->condition = convertExpression(
        bindExpression(*syntaxTree.condition),
        PrimitiveType::Bool,
        syntaxTree.condition->span(),
        "while condition");
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    return result;
}
''',
    '''    result->condition = convertExpression(
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
''')

patch(
    "src/mir/MirLowerer.cpp",
    '''namespace realscript::mir {

Module Lowerer::lower''',
    '''namespace realscript::mir {
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

bool hasIncomingEdge(const Function& function, BlockId target) {
    for (const auto& block : function.blocks) {
        const auto& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::Jump &&
            terminator.target == target) {
            return true;
        }
        if (terminator.kind == TerminatorKind::Branch &&
            (terminator.target == target || terminator.falseTarget == target)) {
            return true;
        }
    }
    return false;
}

} // namespace

Module Lowerer::lower''')

patch(
    "src/mir/MirLowerer.cpp",
    '''        const auto conditionBlock = createBlock(), bodyBlock = createBlock(), exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span); setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*value.condition);
        emitBranch(condition, bodyBlock, exitBlock, {}, {}, value.condition->span);
        breakTargets_.push_back(exitBlock); continueTargets_.push_back(conditionBlock);
        setCurrentBlock(bodyBlock); lowerStatement(*value.body);
        continueTargets_.pop_back(); breakTargets_.pop_back();
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(conditionBlock, {}, value.body->span);
        setCurrentBlock(exitBlock); return;
''',
    '''        const auto conditionBlock = createBlock();
        const auto bodyBlock = createBlock();
        const auto exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span);
        setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*value.condition);
        const auto constantTrue = isLiteralTrue(*value.condition);
        if (constantTrue) {
            emitJump(bodyBlock, {}, value.condition->span);
        } else {
            emitBranch(
                condition,
                bodyBlock,
                exitBlock,
                {},
                {},
                value.condition->span);
        }
        breakTargets_.push_back(exitBlock);
        continueTargets_.push_back(conditionBlock);
        setCurrentBlock(bodyBlock);
        lowerStatement(*value.body);
        continueTargets_.pop_back();
        breakTargets_.pop_back();
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            emitJump(conditionBlock, {}, value.body->span);
        }
        if (constantTrue && !hasIncomingEdge(*currentFunction_, exitBlock)) {
            setCurrentBlock(exitBlock);
            emitJump(exitBlock, {}, statement.span);
            clearCurrentBlock();
        } else {
            setCurrentBlock(exitBlock);
        }
        return;
''')

patch(
    "src/mir/MirLowerer.cpp",
    '''        const auto conditionBlock = createBlock(), bodyBlock = createBlock(), incrementBlock = createBlock(), exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span); setCurrentBlock(conditionBlock);
        emitBranch(lowerExpression(*value.condition), bodyBlock, exitBlock, {}, {}, value.condition->span);
        breakTargets_.push_back(exitBlock); continueTargets_.push_back(incrementBlock);
        setCurrentBlock(bodyBlock); lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(incrementBlock, {}, value.body->span);
        setCurrentBlock(incrementBlock); if (value.increment) (void)lowerExpression(*value.increment);
        if (!currentBlockTerminated()) emitJump(conditionBlock, {}, value.increment ? value.increment->span : statement.span);
        continueTargets_.pop_back(); breakTargets_.pop_back(); setCurrentBlock(exitBlock); return;
''',
    '''        const auto conditionBlock = createBlock();
        const auto bodyBlock = createBlock();
        const auto incrementBlock = createBlock();
        const auto exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span);
        setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*value.condition);
        const auto constantTrue = isLiteralTrue(*value.condition);
        if (constantTrue) {
            emitJump(bodyBlock, {}, value.condition->span);
        } else {
            emitBranch(
                condition,
                bodyBlock,
                exitBlock,
                {},
                {},
                value.condition->span);
        }
        breakTargets_.push_back(exitBlock);
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
        breakTargets_.pop_back();
        if (constantTrue && !hasIncomingEdge(*currentFunction_, exitBlock)) {
            setCurrentBlock(exitBlock);
            emitJump(exitBlock, {}, statement.span);
            clearCurrentBlock();
        } else {
            setCurrentBlock(exitBlock);
        }
        return;
''')

patch(
    "src/mir/MirLowerer.cpp",
    '''        const auto bodyBlock = createBlock(), conditionBlock = createBlock(), exitBlock = createBlock();
        emitJump(bodyBlock, {}, statement.span);
        breakTargets_.push_back(exitBlock); continueTargets_.push_back(conditionBlock);
        setCurrentBlock(bodyBlock); lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(conditionBlock, {}, value.body->span);
        setCurrentBlock(conditionBlock);
        emitBranch(lowerExpression(*value.condition), bodyBlock, exitBlock, {}, {}, value.condition->span);
        continueTargets_.pop_back(); breakTargets_.pop_back(); setCurrentBlock(exitBlock); return;
''',
    '''        const auto bodyBlock = createBlock();
        const auto conditionBlock = createBlock();
        const auto exitBlock = createBlock();
        emitJump(bodyBlock, {}, statement.span);
        breakTargets_.push_back(exitBlock);
        continueTargets_.push_back(conditionBlock);
        setCurrentBlock(bodyBlock);
        lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) {
            emitJump(conditionBlock, {}, value.body->span);
        }
        setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*value.condition);
        const auto constantTrue = isLiteralTrue(*value.condition);
        if (constantTrue) {
            emitJump(bodyBlock, {}, value.condition->span);
        } else {
            emitBranch(
                condition,
                bodyBlock,
                exitBlock,
                {},
                {},
                value.condition->span);
        }
        continueTargets_.pop_back();
        breakTargets_.pop_back();
        if (constantTrue && !hasIncomingEdge(*currentFunction_, exitBlock)) {
            setCurrentBlock(exitBlock);
            emitJump(exitBlock, {}, statement.span);
            clearCurrentBlock();
        } else {
            setCurrentBlock(exitBlock);
        }
        return;
''')

print("Phase 18A semantic fixes applied")
