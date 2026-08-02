#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
path = root / "src/mir/MirLowerer.cpp"
text = path.read_text(encoding="utf-8")

collect_start = text.index(
    "    case semantic::BoundNodeKind::ForStatement: {\n",
    text.index("void Lowerer::collectLocalTypes"))
collect_end = text.index(
    "    case semantic::BoundNodeKind::ForeachStatement: {\n",
    collect_start)
collect_code = '''    case semantic::BoundNodeKind::ForStatement: {
        const auto& value =
            static_cast<const semantic::BoundForStatement&>(statement);
        if (value.initializer) {
            collectLocalTypes(*value.initializer);
        }
        collectLocalTypes(*value.body);
        return;
    }
'''
text = text[:collect_start] + collect_code + text[collect_end:]

lower_section = text.index("void Lowerer::lowerStatement")
lower_start = text.index(
    "    case semantic::BoundNodeKind::ForStatement: {\n",
    lower_section)
lower_end = text.index(
    "    case semantic::BoundNodeKind::ForeachStatement: {\n",
    lower_start)
lower_code = '''    case semantic::BoundNodeKind::ForStatement: {
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
'''
text = text[:lower_start] + lower_code + text[lower_end:]
path.write_text(text, encoding="utf-8", newline="\n")
print("Phase 18A for-loop repair applied")
