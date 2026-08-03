#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "src/mir/MirLowerer.cpp"
text = path.read_text(encoding="utf-8")
wrong = '''    case semantic::BoundNodeKind::EventSubscriptionStatement: {
        const auto& subscription = static_cast<const
            semantic::BoundEventSubscriptionStatement&>(statement);
        const auto receiver = lowerExpression(*subscription.receiver);
        const auto checked = emitValue(
            Opcode::CheckNotNull,
            semantic::PrimitiveType::Object,
            {receiver},
            statement.span);
        auto& check = block(*currentBlockId_).instructions.back();
        check.typeId = subscription.ownerType.id;
        check.resultTypeId = subscription.ownerType.id;
        check.symbolName = semantic::canonicalTypeName(
            subscription.ownerType);
        const auto enabled = emitValue(
            Opcode::ConstantBool,
            semantic::PrimitiveType::Bool,
            {},
            statement.span);
        block(*currentBlockId_).instructions.back().boolImmediate =
            subscription.enabled;
        Instruction store;
        store.resultType = semantic::PrimitiveType::Void;
        store.opcode = Opcode::StoreField;
        store.operands = {checked, enabled};
        store.typeId = subscription.ownerType.id;
        store.fieldIndex = subscription.enabledField.index;
        store.symbolName = semantic::canonicalTypeName(
            subscription.ownerType);
        store.sourceSpan = statement.span;
        block(*currentBlockId_).instructions.push_back(
            std::move(store));
        return;
    }
'''
if wrong not in text:
    raise RuntimeError("misplaced event subscription MIR block not found")
text = text.replace(wrong, '''    case semantic::BoundNodeKind::EventSubscriptionStatement:
        return;
''', 1)
anchor = '''    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& value = static_cast<const semantic::BoundVariableDeclarationStatement&>(statement);
        if (value.initializer) {
'''
correct = '''    case semantic::BoundNodeKind::EventSubscriptionStatement: {
        const auto& subscription = static_cast<const
            semantic::BoundEventSubscriptionStatement&>(statement);
        const auto receiver = lowerExpression(*subscription.receiver);
        const auto checked = emitValue(
            Opcode::CheckNotNull,
            semantic::PrimitiveType::Object,
            {receiver},
            statement.span);
        auto& check = block(*currentBlockId_).instructions.back();
        check.typeId = subscription.ownerType.id;
        check.resultTypeId = subscription.ownerType.id;
        check.symbolName = semantic::canonicalTypeName(
            subscription.ownerType);
        const auto enabled = emitValue(
            Opcode::ConstantBool,
            semantic::PrimitiveType::Bool,
            {},
            statement.span);
        block(*currentBlockId_).instructions.back().boolImmediate =
            subscription.enabled;
        Instruction store;
        store.resultType = semantic::PrimitiveType::Void;
        store.opcode = Opcode::StoreField;
        store.operands = {checked, enabled};
        store.typeId = subscription.ownerType.id;
        store.fieldIndex = subscription.enabledField.index;
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
'''
if correct not in text:
    if anchor not in text:
        raise RuntimeError("lowerStatement variable declaration anchor not found")
    text = text.replace(anchor, correct, 1)
path.write_text(text, encoding="utf-8")
