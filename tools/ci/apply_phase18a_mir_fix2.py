#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]


def patch(path: str, old: str, new: str) -> None:
    file = root / path
    text = file.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"anchor missing in {path}: {old[:120]!r}")
    file.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


# Switch discriminants must live in a local because MIR values are block-local.
patch(
    "include/realscript/semantic/Semantic.h",
    '''struct BoundSwitchStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> expression;
    std::vector<BoundSwitchSection> sections;
''',
    '''struct BoundSwitchStatement final : BoundStatement {
    VariableSymbol valueVariable;
    std::unique_ptr<BoundExpression> expression;
    std::vector<BoundSwitchSection> sections;
''')

patch(
    "src/semantic/SemanticBinding.cpp",
    '''    result->span = syntaxTree.span();
    result->expression = bindExpression(*syntaxTree.expression);
    ++breakableDepth_;
''',
    '''    result->span = syntaxTree.span();
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
''')
patch(
    "src/semantic/SemanticBinding.cpp",
    '''    --breakableDepth_;
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration''',
    '''    --breakableDepth_;
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration''')

# Replace the unreachable-exit helper with direct-break analysis.
patch(
    "src/mir/MirLowerer.cpp",
    '''bool hasIncomingEdge(const Function& function, BlockId target) {
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
''',
    '''bool containsBreakForCurrentTarget(
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
''')

# Collect the synthetic switch local.
patch(
    "src/mir/MirLowerer.cpp",
    '''    case semantic::BoundNodeKind::SwitchStatement:
        for (const auto& section : static_cast<const semantic::BoundSwitchStatement&>(statement).sections)
            for (const auto& child : section.statements) collectLocalTypes(*child);
        return;
''',
    '''    case semantic::BoundNodeKind::SwitchStatement: {
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
''')

# Replace while lowering with optional exit allocation.
start = '''    case semantic::BoundNodeKind::WhileStatement: {
'''
end = '''    case semantic::BoundNodeKind::ForStatement: {
'''
file = root / "src/mir/MirLowerer.cpp"
text = file.read_text(encoding="utf-8")
begin = text.index(start)
finish = text.index(end, begin)
while_code = '''    case semantic::BoundNodeKind::WhileStatement: {
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
'''
text = text[:begin] + while_code + text[finish:]
file.write_text(text, encoding="utf-8", newline="\n")

# Replace for lowering.
text = file.read_text(encoding="utf-8")
begin = text.index(end)
finish = text.index('''    case semantic::BoundNodeKind::ForeachStatement: {
''', begin)
for_code = '''    case semantic::BoundNodeKind::ForStatement: {
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
        if (value.increment) (void)lowerExpression(*value.increment);
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
'''
text = text[:begin] + for_code + text[finish:]
file.write_text(text, encoding="utf-8", newline="\n")

# Replace do/while lowering.
text = file.read_text(encoding="utf-8")
begin = text.index('''    case semantic::BoundNodeKind::DoWhileStatement: {
''')
finish = text.index('''    case semantic::BoundNodeKind::SwitchStatement: {
''', begin)
do_code = '''    case semantic::BoundNodeKind::DoWhileStatement: {
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
'''
text = text[:begin] + do_code + text[finish:]
file.write_text(text, encoding="utf-8", newline="\n")

# Switch discriminant is stored and reloaded in each check block.
patch(
    "src/mir/MirLowerer.cpp",
    '''        const auto switchValue = lowerExpression(*value.expression); const auto exitBlock = createBlock();
''',
    '''        emitStoreLocal(
            value.valueVariable.index,
            lowerExpression(*value.expression),
            value.expression->span);
        const auto exitBlock = createBlock();
''')
patch(
    "src/mir/MirLowerer.cpp",
    '''            const auto caseValue = lowerExpression(*value.sections[i].label);
            const auto equal = emitValue(Opcode::Equal, semantic::PrimitiveType::Bool,
                {switchValue, caseValue}, value.sections[i].span);
''',
    '''            const auto switchValue =
                emitLoadLocal(value.valueVariable, value.expression->span);
            const auto caseValue = lowerExpression(*value.sections[i].label);
            const auto equal = emitValue(
                Opcode::Equal,
                semantic::PrimitiveType::Bool,
                {switchValue, caseValue},
                value.sections[i].span);
''')

print("Phase 18A MIR fixes applied")
