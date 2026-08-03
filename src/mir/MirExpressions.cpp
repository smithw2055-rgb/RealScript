#include "realscript/mir/Mir.h"

#include <stdexcept>
#include <utility>

namespace realscript::mir {

ValueId Lowerer::lowerExpression(const semantic::BoundExpression& expression) {
    auto emitCallInstruction = [&](const semantic::FunctionSymbol& function,
                                   semantic::PrimitiveType resultType,
                                   const std::string& resultTypeName,
                                   std::vector<ValueId> arguments,
                                   text::TextSpan span,
                                   bool virtualDispatch,
                                   std::uint32_t virtualSlot,
                                   bool interfaceDispatch,
                                   semantic::SymbolId interfaceTypeId,
                                   std::uint32_t interfaceSlot) -> ValueId {
        Instruction instruction;
        instruction.resultType = resultType;
        instruction.opcode = Opcode::Call;
        instruction.operands = std::move(arguments);
        instruction.symbolId = function.id;
        instruction.virtualDispatch = virtualDispatch;
        instruction.virtualSlot = virtualSlot;
        instruction.interfaceDispatch = interfaceDispatch;
        instruction.interfaceTypeId = interfaceTypeId;
        instruction.interfaceSlot = interfaceSlot;
        instruction.symbolName = function.moduleName + "::" +
            (function.ownerTypeName.empty()
                ? function.name
                : function.ownerTypeName + "." + function.name);
        instruction.resultTypeId = semantic::isExactType(resultType)
            ? semantic::stableTypeId(resultTypeName)
            : 0;
        for (const auto& parameter : function.parameters) {
            const auto storageType =
                semantic::storageTypeOf(parameter);
            const auto& storageTypeName =
                semantic::storageTypeNameOf(parameter);
            instruction.parameterTypes.push_back(storageType);
            instruction.parameterTypeIds.push_back(
                semantic::isExactType(storageType)
                    ? semantic::stableTypeId(storageTypeName)
                    : 0);
        }
        instruction.sourceSpan = span;
        if (resultType != semantic::PrimitiveType::Void) {
            instruction.result = nextValueId_++;
        }
        block(*currentBlockId_).instructions.push_back(std::move(instruction));
        return block(*currentBlockId_).instructions.back().result;
    };

    switch (expression.kind()) {
    case semantic::BoundNodeKind::LiteralExpression: {
        const auto& literal = static_cast<const semantic::BoundLiteralExpression&>(expression);
        if (literal.type == semantic::PrimitiveType::Int ||
            literal.type == semantic::PrimitiveType::Long ||
            literal.type == semantic::PrimitiveType::Enum) {
            const auto value = emitValue(Opcode::ConstantInt, literal.type, {}, expression.span);
            auto& instruction = block(*currentBlockId_).instructions.back();
            instruction.integerImmediate = std::get<std::int64_t>(literal.value);
            instruction.resultTypeId = literal.type == semantic::PrimitiveType::Enum
                ? semantic::stableTypeId(literal.typeName)
                : 0;
            return value;
        }
        if (literal.type == semantic::PrimitiveType::Double) {
            const auto value = emitValue(Opcode::ConstantDouble, literal.type, {}, expression.span);
            block(*currentBlockId_).instructions.back().doubleImmediate =
                std::get<double>(literal.value);
            return value;
        }
        if (literal.type == semantic::PrimitiveType::Bool) {
            const auto value = emitValue(Opcode::ConstantBool, literal.type, {}, expression.span);
            block(*currentBlockId_).instructions.back().boolImmediate = std::get<bool>(literal.value);
            return value;
        }
        if (literal.type == semantic::PrimitiveType::String) {
            const auto value = emitValue(Opcode::ConstantString, literal.type, {}, expression.span);
            block(*currentBlockId_).instructions.back().stringImmediate = std::get<std::string>(literal.value);
            return value;
        }
        if (literal.type == semantic::PrimitiveType::Null) {
            return emitValue(Opcode::ConstantNull, literal.type, {}, expression.span);
        }
        throw std::logic_error("unsupported literal type in MIR lowerer");
    }
    case semantic::BoundNodeKind::VariableExpression: {
        const auto& variable = static_cast<const semantic::BoundVariableExpression&>(expression);
        const auto value = emitValue(Opcode::LoadLocal, variable.type, {}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.localIndex = variable.variable.index;
        instruction.resultTypeId = semantic::isExactType(variable.type)
            ? semantic::stableTypeId(variable.variable.typeName)
            : 0;
        return value;
    }
    case semantic::BoundNodeKind::AssignmentExpression: {
        const auto& assignment = static_cast<const semantic::BoundAssignmentExpression&>(expression);
        const auto value = lowerExpression(*assignment.expression);
        emitStoreLocal(assignment.variable.index, value, expression.span);
        return value;
    }
    case semantic::BoundNodeKind::ConversionExpression: {
        const auto& conversion = static_cast<const semantic::BoundConversionExpression&>(expression);
        const auto operand = lowerExpression(*conversion.expression);
        switch (conversion.conversion) {
        case semantic::ConversionKind::Identity:
            return operand;
        case semantic::ConversionKind::NullToString:
            return emitValue(Opcode::ConvertNullToString, conversion.type, {operand}, expression.span);
        case semantic::ConversionKind::NullToObject: {
            const auto value = emitValue(Opcode::ConvertNullToObject, conversion.type, {operand}, expression.span);
            block(*currentBlockId_).instructions.back().resultTypeId = semantic::stableTypeId(conversion.typeName);
            return value;
        }
        case semantic::ConversionKind::NullToArray: {
            const auto value = emitValue(Opcode::ConvertNullToArray, conversion.type, {operand}, expression.span);
            block(*currentBlockId_).instructions.back().resultTypeId = semantic::stableTypeId(conversion.typeName);
            return value;
        }
        case semantic::ConversionKind::IntToLong:
            return emitValue(Opcode::ConvertIntToLong, conversion.type, {operand}, expression.span);
        case semantic::ConversionKind::IntToDouble:
            return emitValue(Opcode::ConvertIntToDouble, conversion.type, {operand}, expression.span);
        case semantic::ConversionKind::LongToDouble:
            return emitValue(Opcode::ConvertLongToDouble, conversion.type, {operand}, expression.span);
        case semantic::ConversionKind::None:
            break;
        }
        throw std::logic_error("invalid bound conversion reached MIR lowering");
    }
    case semantic::BoundNodeKind::NewArrayExpression: {
        const auto& allocation = static_cast<const semantic::BoundNewArrayExpression&>(expression);
        const auto length = lowerExpression(*allocation.length);
        const auto value = emitValue(Opcode::NewArray, semantic::PrimitiveType::Array, {length}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.resultTypeId = semantic::stableTypeId(allocation.typeName);
        instruction.elementType = allocation.elementType;
        instruction.elementTypeId = semantic::isExactType(allocation.elementType)
            ? semantic::stableTypeId(allocation.elementTypeName)
            : 0;
        instruction.symbolName = allocation.typeName;
        return value;
    }
    case semantic::BoundNodeKind::ArrayLengthExpression: {
        const auto& length = static_cast<const semantic::BoundArrayLengthExpression&>(expression);
        return emitValue(Opcode::ArrayLength, semantic::PrimitiveType::Int,
            {lowerExpression(*length.receiver)}, expression.span);
    }
    case semantic::BoundNodeKind::ElementAccessExpression: {
        const auto& access = static_cast<const semantic::BoundElementAccessExpression&>(expression);
        const auto value = emitValue(Opcode::LoadElement, access.elementType,
            {lowerExpression(*access.receiver), lowerExpression(*access.index)}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.resultTypeId = semantic::isExactType(access.elementType)
            ? semantic::stableTypeId(access.elementTypeName)
            : 0;
        instruction.elementType = access.elementType;
        instruction.elementTypeId = instruction.resultTypeId;
        return value;
    }
    case semantic::BoundNodeKind::ElementAssignmentExpression: {
        const auto& assignment = static_cast<const semantic::BoundElementAssignmentExpression&>(expression);
        const auto receiver = lowerExpression(*assignment.receiver);
        const auto index = lowerExpression(*assignment.index);
        const auto value = lowerExpression(*assignment.expression);
        Instruction store;
        store.resultType = semantic::PrimitiveType::Void;
        store.opcode = Opcode::StoreElement;
        store.operands = {receiver, index, value};
        store.elementType = assignment.elementType;
        store.elementTypeId = semantic::isExactType(assignment.elementType)
            ? semantic::stableTypeId(assignment.elementTypeName)
            : 0;
        store.sourceSpan = expression.span;
        block(*currentBlockId_).instructions.push_back(std::move(store));
        return value;
    }
    case semantic::BoundNodeKind::NewObjectExpression: {
        const auto& allocation = static_cast<const semantic::BoundNewObjectExpression&>(expression);
        const auto value = emitValue(Opcode::NewObject, semantic::PrimitiveType::Object, {}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.typeId = allocation.objectType.id;
        instruction.resultTypeId = allocation.objectType.id;
        instruction.symbolName = semantic::canonicalTypeName(allocation.objectType);
        if (allocation.constructor) {
            std::vector<ValueId> arguments{value};
            for (const auto& argument : allocation.arguments) arguments.push_back(lowerExpression(*argument));
            (void)emitCallInstruction(
                *allocation.constructor,
                semantic::PrimitiveType::Void,
                {},
                std::move(arguments),
                expression.span,
                false,
                std::numeric_limits<std::uint32_t>::max(),
                false,
                0,
                std::numeric_limits<std::uint32_t>::max());
        }
        return value;
    }
    case semantic::BoundNodeKind::NewStructExpression: {
        const auto& allocation = static_cast<const semantic::BoundNewStructExpression&>(expression);
        auto value = emitValue(Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.typeId = allocation.structType.id;
        instruction.resultTypeId = allocation.structType.id;
        instruction.symbolName = semantic::canonicalTypeName(allocation.structType);
        if (allocation.constructor) {
            std::vector<ValueId> arguments{value};
            for (const auto& argument : allocation.arguments) arguments.push_back(lowerExpression(*argument));
            value = emitCallInstruction(
                *allocation.constructor,
                semantic::PrimitiveType::Struct,
                semantic::canonicalTypeName(allocation.structType),
                std::move(arguments),
                expression.span,
                false,
                std::numeric_limits<std::uint32_t>::max(),
                false,
                0,
                std::numeric_limits<std::uint32_t>::max());
        }
        return value;
    }
    case semantic::BoundNodeKind::MemberAccessExpression: {
        const auto& member = static_cast<const semantic::BoundMemberAccessExpression&>(expression);
        const auto receiver = lowerExpression(*member.receiver);
        const auto checked = emitValue(Opcode::CheckNotNull, semantic::PrimitiveType::Object, {receiver}, member.receiver->span);
        auto& check = block(*currentBlockId_).instructions.back();
        check.typeId = member.ownerType.id;
        check.resultTypeId = member.ownerType.id;
        check.symbolName = semantic::canonicalTypeName(member.ownerType);
        const auto value = emitValue(Opcode::LoadField, member.field.type, {checked}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.typeId = member.ownerType.id;
        instruction.resultTypeId = semantic::isExactType(member.field.type)
            ? semantic::stableTypeId(member.field.typeName)
            : 0;
        instruction.fieldIndex = member.field.index;
        instruction.symbolName = semantic::canonicalTypeName(member.ownerType);
        return value;
    }
    case semantic::BoundNodeKind::MemberAssignmentExpression: {
        const auto& assignment = static_cast<const semantic::BoundMemberAssignmentExpression&>(expression);
        const auto receiver = lowerExpression(*assignment.receiver);
        const auto checked = emitValue(Opcode::CheckNotNull, semantic::PrimitiveType::Object, {receiver}, assignment.receiver->span);
        auto& check = block(*currentBlockId_).instructions.back();
        check.typeId = assignment.ownerType.id;
        check.resultTypeId = assignment.ownerType.id;
        check.symbolName = semantic::canonicalTypeName(assignment.ownerType);
        const auto value = lowerExpression(*assignment.expression);
        Instruction store;
        store.resultType = semantic::PrimitiveType::Void;
        store.opcode = Opcode::StoreField;
        store.operands = {checked, value};
        store.typeId = assignment.ownerType.id;
        store.fieldIndex = assignment.field.index;
        store.symbolName = semantic::canonicalTypeName(assignment.ownerType);
        store.sourceSpan = expression.span;
        block(*currentBlockId_).instructions.push_back(std::move(store));
        return value;
    }
    case semantic::BoundNodeKind::StructFieldAccessExpression: {
        const auto& member = static_cast<const semantic::BoundStructFieldAccessExpression&>(expression);
        const auto value = emitValue(Opcode::LoadStructField, member.field.type,
            {lowerExpression(*member.receiver)}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.typeId = member.ownerType.id;
        instruction.fieldIndex = member.field.index;
        instruction.resultTypeId = semantic::isExactType(member.field.type)
            ? semantic::stableTypeId(member.field.typeName)
            : 0;
        instruction.symbolName = semantic::canonicalTypeName(member.ownerType);
        return value;
    }
    case semantic::BoundNodeKind::StructFieldAssignmentExpression: {
        const auto& assignment = static_cast<const semantic::BoundStructFieldAssignmentExpression&>(expression);
        const auto current = emitValue(Opcode::LoadLocal, semantic::PrimitiveType::Struct, {}, expression.span);
        auto& load = block(*currentBlockId_).instructions.back();
        load.localIndex = assignment.variable.index;
        load.resultTypeId = assignment.ownerType.id;
        const auto assigned = lowerExpression(*assignment.expression);
        const auto updated = emitValue(Opcode::StoreStructField, semantic::PrimitiveType::Struct,
            {current, assigned}, expression.span);
        auto& store = block(*currentBlockId_).instructions.back();
        store.typeId = assignment.ownerType.id;
        store.resultTypeId = assignment.ownerType.id;
        store.fieldIndex = assignment.field.index;
        store.symbolName = semantic::canonicalTypeName(assignment.ownerType);
        emitStoreLocal(assignment.variable.index, updated, expression.span);
        return assigned;
    }
    case semantic::BoundNodeKind::PropertyAssignmentExpression: {
        const auto& assignment = static_cast<const semantic::BoundPropertyAssignmentExpression&>(expression);
        std::vector<ValueId> arguments;
        for (std::size_t index = 0; index < assignment.arguments.size(); ++index) {
            auto value = lowerExpression(*assignment.arguments[index]);
            if (index == 0 && assignment.setter.method &&
                !assignment.setter.staticMethod &&
                assignment.arguments[index]->type == semantic::PrimitiveType::Object) {
                value = emitValue(
                    Opcode::CheckNotNull,
                    semantic::PrimitiveType::Object,
                    {value},
                    assignment.arguments[index]->span);
                auto& check = block(*currentBlockId_).instructions.back();
                check.typeId = assignment.setter.ownerTypeId;
                check.resultTypeId = assignment.setter.ownerTypeId;
                check.symbolName = assignment.setter.moduleName + "::" +
                    assignment.setter.ownerTypeName;
            }
            arguments.push_back(value);
        }
        const auto assigned = lowerExpression(*assignment.assignedValue);
        arguments.push_back(assigned);
        (void)emitCallInstruction(
            assignment.setter,
            semantic::PrimitiveType::Void,
            {},
            std::move(arguments),
            expression.span,
            false,
            std::numeric_limits<std::uint32_t>::max(),
            false,
            0,
            std::numeric_limits<std::uint32_t>::max());
        return assigned;
    }
    case semantic::BoundNodeKind::EventInvocationExpression: {
        const auto& invocation = static_cast<const
            semantic::BoundEventInvocationExpression&>(expression);
        auto receiver = lowerExpression(*invocation.receiver);
        receiver = emitValue(
            Opcode::CheckNotNull,
            semantic::PrimitiveType::Object,
            {receiver},
            invocation.receiver->span);
        auto& receiverCheck =
            block(*currentBlockId_).instructions.back();
        receiverCheck.typeId = invocation.ownerType.id;
        receiverCheck.resultTypeId = invocation.ownerType.id;
        receiverCheck.symbolName = semantic::canonicalTypeName(
            invocation.ownerType);
        std::vector<ValueId> values;
        for (const auto& argument : invocation.arguments) {
            values.push_back(lowerExpression(*argument));
        }
        for (const auto& handler : invocation.event.handlers) {
            const auto enabled = emitValue(
                Opcode::LoadField,
                semantic::PrimitiveType::Bool,
                {receiver},
                expression.span);
            auto& load = block(*currentBlockId_).instructions.back();
            load.typeId = invocation.ownerType.id;
            load.fieldIndex = handler.enabledField.index;
            load.symbolName = semantic::canonicalTypeName(
                invocation.ownerType);
            const auto callBlock = createBlock();
            const auto nextBlock = createBlock();
            emitBranch(
                enabled,
                callBlock,
                nextBlock,
                {},
                {},
                expression.span);
            setCurrentBlock(callBlock);
            std::vector<ValueId> arguments{receiver};
            arguments.insert(
                arguments.end(), values.begin(), values.end());
            (void)emitCallInstruction(
                handler.function,
                semantic::PrimitiveType::Void,
                {},
                std::move(arguments),
                expression.span,
                false,
                std::numeric_limits<std::uint32_t>::max(),
                false,
                0,
                std::numeric_limits<std::uint32_t>::max());
            emitJump(nextBlock, {}, expression.span);
            setCurrentBlock(nextBlock);
        }
        return -1;
    }
    case semantic::BoundNodeKind::ReferenceCallExpression: {
        const auto& call = static_cast<const
            semantic::BoundReferenceCallExpression&>(expression);
        std::vector<ValueId> arguments;
        struct Writeback {
            semantic::VariableSymbol variable;
            semantic::TypeSymbol wrapperType;
            semantic::FieldSymbol field;
            ValueId wrapper = -1;
        };
        std::vector<Writeback> writebacks;
        for (const auto& argument : call.arguments) {
            if (argument.modifier ==
                    semantic::ParameterModifier::None ||
                argument.modifier ==
                    semantic::ParameterModifier::In ||
                argument.forwarded) {
                arguments.push_back(
                    lowerExpression(*argument.value));
                continue;
            }

            const auto wrapper = emitValue(
                Opcode::NewObject,
                semantic::PrimitiveType::Object,
                {},
                expression.span);
            auto& allocation =
                block(*currentBlockId_).instructions.back();
            allocation.typeId = argument.wrapperType.id;
            allocation.resultTypeId = argument.wrapperType.id;
            allocation.symbolName =
                semantic::canonicalTypeName(argument.wrapperType);
            if (argument.modifier ==
                    semantic::ParameterModifier::Ref &&
                argument.value) {
                const auto initial =
                    lowerExpression(*argument.value);
                Instruction store;
                store.resultType =
                    semantic::PrimitiveType::Void;
                store.opcode = Opcode::StoreField;
                store.operands = {wrapper, initial};
                store.typeId = argument.wrapperType.id;
                store.fieldIndex = argument.valueField.index;
                store.symbolName = semantic::canonicalTypeName(
                    argument.wrapperType);
                store.sourceSpan = expression.span;
                block(*currentBlockId_).instructions.push_back(
                    std::move(store));
            }
            arguments.push_back(wrapper);
            writebacks.push_back(Writeback{
                argument.variable,
                argument.wrapperType,
                argument.valueField,
                wrapper});
        }

        const auto callResult = emitCallInstruction(
            call.function,
            call.type,
            call.function.returnTypeName,
            std::move(arguments),
            expression.span,
            call.virtualDispatch,
            call.virtualSlot,
            call.interfaceDispatch,
            call.interfaceTypeId,
            call.interfaceSlot);
        for (const auto& writeback : writebacks) {
            const auto value = emitValue(
                Opcode::LoadField,
                writeback.field.type,
                {writeback.wrapper},
                expression.span);
            auto& load = block(
                *currentBlockId_).instructions.back();
            load.typeId = writeback.wrapperType.id;
            load.resultTypeId = semantic::isExactType(
                    writeback.field.type)
                ? semantic::stableTypeId(
                    writeback.field.typeName)
                : 0;
            load.fieldIndex = writeback.field.index;
            load.symbolName = semantic::canonicalTypeName(
                writeback.wrapperType);
            emitStoreLocal(
                writeback.variable.index, value, expression.span);
        }
        return callResult;
    }
    case semantic::BoundNodeKind::CallExpression: {
        const auto& call = static_cast<const semantic::BoundCallExpression&>(expression);
        std::vector<ValueId> arguments;
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            auto value = lowerExpression(*call.arguments[index]);
            if (index == 0 && call.function.method &&
                !call.function.staticMethod &&
                call.arguments[index]->type == semantic::PrimitiveType::Object) {
                value = emitValue(
                    Opcode::CheckNotNull,
                    semantic::PrimitiveType::Object,
                    {value},
                    call.arguments[index]->span);
                auto& check = block(*currentBlockId_).instructions.back();
                check.typeId = call.function.ownerTypeId;
                check.resultTypeId = call.function.ownerTypeId;
                check.symbolName = call.function.moduleName + "::" +
                    call.function.ownerTypeName;
            }
            arguments.push_back(value);
        }
        return emitCallInstruction(
            call.function,
            call.type,
            call.function.returnTypeName,
            std::move(arguments),
            expression.span,
            call.virtualDispatch,
            call.virtualSlot,
            call.interfaceDispatch,
            call.interfaceTypeId,
            call.interfaceSlot);
    }
    case semantic::BoundNodeKind::UnaryExpression: {
        const auto& unary = static_cast<const semantic::BoundUnaryExpression&>(expression);
        const auto operand = lowerExpression(*unary.operand);
        switch (unary.operatorKind) {
        case semantic::BoundUnaryOperatorKind::Identity:
            return operand;
        case semantic::BoundUnaryOperatorKind::Negation:
            return emitValue(
                unary.type == semantic::PrimitiveType::Int ? Opcode::NegateInt :
                unary.type == semantic::PrimitiveType::Long ? Opcode::NegateLong : Opcode::NegateDouble,
                unary.type, {operand}, expression.span);
        case semantic::BoundUnaryOperatorKind::LogicalNegation:
            return emitValue(Opcode::LogicalNot, unary.type, {operand}, expression.span);
        }
        break;
    }
    case semantic::BoundNodeKind::BinaryExpression: {
        const auto& binary = static_cast<const semantic::BoundBinaryExpression&>(expression);
        if (binary.operatorKind == semantic::BoundBinaryOperatorKind::LogicalAnd ||
            binary.operatorKind == semantic::BoundBinaryOperatorKind::LogicalOr) {
            return lowerShortCircuit(binary);
        }
        const auto left = lowerExpression(*binary.left);
        const auto right = lowerExpression(*binary.right);
        const auto numericType = binary.left->type;
        Opcode opcode = Opcode::Equal;
        const bool isLong = numericType == semantic::PrimitiveType::Long;
        const bool isDouble = numericType == semantic::PrimitiveType::Double;
        switch (binary.operatorKind) {
        case semantic::BoundBinaryOperatorKind::Addition:
            opcode = isDouble ? Opcode::AddDouble : isLong ? Opcode::AddLong : Opcode::AddInt; break;
        case semantic::BoundBinaryOperatorKind::Subtraction:
            opcode = isDouble ? Opcode::SubtractDouble : isLong ? Opcode::SubtractLong : Opcode::SubtractInt; break;
        case semantic::BoundBinaryOperatorKind::Multiplication:
            opcode = isDouble ? Opcode::MultiplyDouble : isLong ? Opcode::MultiplyLong : Opcode::MultiplyInt; break;
        case semantic::BoundBinaryOperatorKind::Division:
            opcode = isDouble ? Opcode::DivideDouble : isLong ? Opcode::DivideLong : Opcode::DivideInt; break;
        case semantic::BoundBinaryOperatorKind::Remainder:
            opcode = isLong ? Opcode::RemainderLong : Opcode::RemainderInt; break;
        case semantic::BoundBinaryOperatorKind::Equals: opcode = Opcode::Equal; break;
        case semantic::BoundBinaryOperatorKind::NotEquals: opcode = Opcode::NotEqual; break;
        case semantic::BoundBinaryOperatorKind::Less:
            opcode = isDouble ? Opcode::LessDouble : isLong ? Opcode::LessLong : Opcode::LessInt; break;
        case semantic::BoundBinaryOperatorKind::LessOrEquals:
            opcode = isDouble ? Opcode::LessOrEqualDouble : isLong ? Opcode::LessOrEqualLong : Opcode::LessOrEqualInt; break;
        case semantic::BoundBinaryOperatorKind::Greater:
            opcode = isDouble ? Opcode::GreaterDouble : isLong ? Opcode::GreaterLong : Opcode::GreaterInt; break;
        case semantic::BoundBinaryOperatorKind::GreaterOrEquals:
            opcode = isDouble ? Opcode::GreaterOrEqualDouble : isLong ? Opcode::GreaterOrEqualLong : Opcode::GreaterOrEqualInt; break;
        case semantic::BoundBinaryOperatorKind::LogicalAnd:
        case semantic::BoundBinaryOperatorKind::LogicalOr:
            throw std::logic_error("short-circuit operator reached eager MIR lowering");
        }
        return emitValue(opcode, binary.type, {left, right}, expression.span);
    }
    default:
        break;
    }
    throw std::logic_error("unsupported bound expression in MIR lowerer");
}

ValueId Lowerer::lowerShortCircuit(
    const semantic::BoundBinaryExpression& expression) {
    const auto left = lowerExpression(*expression.left);
    const auto rhsBlock = createBlock();
    const auto mergeBlock = createBlock();
    const auto mergeValue = addBlockParameter(
        mergeBlock,
        semantic::PrimitiveType::Bool);

    const bool isAnd =
        expression.operatorKind == semantic::BoundBinaryOperatorKind::LogicalAnd;
    const auto shortCircuitValue = emitValue(
        Opcode::ConstantBool,
        semantic::PrimitiveType::Bool,
        {},
        expression.span);
    block(*currentBlockId_).instructions.back().boolImmediate = !isAnd;

    if (isAnd) {
        emitBranch(
            left,
            rhsBlock,
            mergeBlock,
            {},
            {shortCircuitValue},
            expression.span);
    } else {
        emitBranch(
            left,
            mergeBlock,
            rhsBlock,
            {shortCircuitValue},
            {},
            expression.span);
    }

    setCurrentBlock(rhsBlock);
    const auto right = lowerExpression(*expression.right);
    emitJump(mergeBlock, {right}, expression.right->span);
    setCurrentBlock(mergeBlock);
    return mergeValue;
}

BlockId Lowerer::createBlock() {
    const auto id = static_cast<BlockId>(currentFunction_->blocks.size());
    currentFunction_->blocks.push_back({id, {}, {}, {}});
    return id;
}

ValueId Lowerer::addBlockParameter(
    BlockId blockId,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) {
    const auto value = nextValueId_++;
    block(blockId).parameters.push_back({value, type, typeId});
    return value;
}

BasicBlock& Lowerer::block(BlockId id) {
    if (id >= currentFunction_->blocks.size() ||
        currentFunction_->blocks[id].id != id) {
        throw std::logic_error("invalid MIR block id");
    }
    return currentFunction_->blocks[id];
}

const BasicBlock& Lowerer::block(BlockId id) const {
    if (id >= currentFunction_->blocks.size() ||
        currentFunction_->blocks[id].id != id) {
        throw std::logic_error("invalid MIR block id");
    }
    return currentFunction_->blocks[id];
}

bool Lowerer::hasCurrentBlock() const noexcept {
    return currentBlockId_.has_value();
}

bool Lowerer::currentBlockTerminated() const {
    return !hasCurrentBlock() ||
        block(*currentBlockId_).terminator.kind != TerminatorKind::None;
}

void Lowerer::setCurrentBlock(BlockId id) {
    (void)block(id);
    currentBlockId_ = id;
}

void Lowerer::clearCurrentBlock() noexcept {
    currentBlockId_.reset();
}

ValueId Lowerer::emitValue(
    Opcode opcode,
    semantic::PrimitiveType type,
    std::vector<ValueId> operands,
    text::TextSpan sourceSpan) {
    if (!hasCurrentBlock() || currentBlockTerminated()) {
        const auto functionName = currentFunction_
            ? currentFunction_->name
            : std::string{"<none>"};
        const auto blockState = !hasCurrentBlock()
            ? std::string{"none"}
            : std::string{"bb"} + std::to_string(*currentBlockId_) +
                ":" + terminatorName(
                    block(*currentBlockId_).terminator.kind);
        throw std::logic_error(
            "cannot emit MIR value '" +
            std::string{opcodeName(opcode)} +
            "' at span " + std::to_string(sourceSpan.start) +
            ":" + std::to_string(sourceSpan.length) +
            " with block " + blockState +
            " in function '" + functionName + "'");
    }

    Instruction instruction;
    instruction.result = nextValueId_++;
    instruction.resultType = type;
    instruction.opcode = opcode;
    instruction.operands = std::move(operands);
    instruction.sourceSpan = sourceSpan;
    block(*currentBlockId_).instructions.push_back(std::move(instruction));
    return block(*currentBlockId_).instructions.back().result;
}

void Lowerer::emitStoreLocal(
    std::size_t localIndex,
    ValueId value,
    text::TextSpan sourceSpan) {
    if (!hasCurrentBlock() || currentBlockTerminated()) {
        throw std::logic_error(
            "cannot emit a local store without an open MIR block");
    }

    Instruction instruction;
    instruction.result = -1;
    instruction.resultType = semantic::PrimitiveType::Void;
    instruction.opcode = Opcode::StoreLocal;
    instruction.operands = {value};
    instruction.localIndex = localIndex;
    instruction.sourceSpan = sourceSpan;
    block(*currentBlockId_).instructions.push_back(std::move(instruction));
}

void Lowerer::emitJump(
    BlockId target,
    std::vector<ValueId> arguments,
    text::TextSpan sourceSpan) {
    if (!hasCurrentBlock() || currentBlockTerminated()) {
        throw std::logic_error("cannot emit a jump into a closed MIR block");
    }
    auto& terminator = block(*currentBlockId_).terminator;
    terminator.kind = TerminatorKind::Jump;
    terminator.target = target;
    terminator.arguments = std::move(arguments);
    terminator.sourceSpan = sourceSpan;
}

void Lowerer::emitBranch(
    ValueId condition,
    BlockId trueTarget,
    BlockId falseTarget,
    std::vector<ValueId> trueArguments,
    std::vector<ValueId> falseArguments,
    text::TextSpan sourceSpan) {
    if (!hasCurrentBlock() || currentBlockTerminated()) {
        throw std::logic_error("cannot emit a branch into a closed MIR block");
    }
    auto& terminator = block(*currentBlockId_).terminator;
    terminator.kind = TerminatorKind::Branch;
    terminator.condition = condition;
    terminator.target = trueTarget;
    terminator.falseTarget = falseTarget;
    terminator.arguments = std::move(trueArguments);
    terminator.falseArguments = std::move(falseArguments);
    terminator.sourceSpan = sourceSpan;
}

void Lowerer::emitReturn(
    std::optional<ValueId> value,
    text::TextSpan sourceSpan) {
    if (!hasCurrentBlock() || currentBlockTerminated()) {
        throw std::logic_error("cannot emit a return into a closed MIR block");
    }
    auto& terminator = block(*currentBlockId_).terminator;
    terminator.kind = value
        ? TerminatorKind::ReturnValue
        : TerminatorKind::ReturnVoid;
    terminator.value = value.value_or(-1);
    terminator.sourceSpan = sourceSpan;
}

} // namespace realscript::mir
