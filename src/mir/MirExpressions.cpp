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
    auto emitTypedNull = [&](semantic::PrimitiveType type,
                             const std::string& typeName,
                             text::TextSpan span) -> ValueId {
        const auto raw = emitValue(
            Opcode::ConstantNull, semantic::PrimitiveType::Null, {}, span);
        Opcode opcode = Opcode::ConvertNullToObject;
        if (type == semantic::PrimitiveType::String) {
            opcode = Opcode::ConvertNullToString;
        } else if (type == semantic::PrimitiveType::Array) {
            opcode = Opcode::ConvertNullToArray;
        }
        const auto value = emitValue(opcode, type, {raw}, span);
        if (semantic::isExactType(type)) {
            block(*currentBlockId_).instructions.back().resultTypeId =
                semantic::stableTypeId(typeName);
        }
        return value;
    };
    auto emitDefaultStruct = [&](const semantic::TypeSymbol& type,
                                 text::TextSpan span) -> ValueId {
        const auto value = emitValue(
            Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.typeId = type.id;
        instruction.resultTypeId = type.id;
        instruction.symbolName = semantic::canonicalTypeName(type);
        return value;
    };
    auto emitNullableValue = [&](const semantic::TypeSymbol& type,
                                 const semantic::FieldSymbol& hasValueField,
                                 const semantic::FieldSymbol& valueField,
                                 ValueId underlying,
                                 text::TextSpan span) -> ValueId {
        auto value = emitDefaultStruct(type, span);
        const auto present = emitValue(
            Opcode::ConstantBool, semantic::PrimitiveType::Bool, {}, span);
        block(*currentBlockId_).instructions.back().boolImmediate = true;
        value = emitValue(
            Opcode::StoreStructField, semantic::PrimitiveType::Struct,
            {value, present}, span);
        auto& hasValueStore = block(*currentBlockId_).instructions.back();
        hasValueStore.typeId = type.id;
        hasValueStore.resultTypeId = type.id;
        hasValueStore.fieldIndex = hasValueField.index;
        hasValueStore.symbolName = semantic::canonicalTypeName(type);
        value = emitValue(
            Opcode::StoreStructField, semantic::PrimitiveType::Struct,
            {value, underlying}, span);
        auto& valueStore = block(*currentBlockId_).instructions.back();
        valueStore.typeId = type.id;
        valueStore.resultTypeId = type.id;
        valueStore.fieldIndex = valueField.index;
        valueStore.symbolName = semantic::canonicalTypeName(type);
        return value;
    };

    switch (expression.kind()) {
    case semantic::BoundNodeKind::DelegateCreationExpression: {
        const auto& creation = static_cast<const
            semantic::BoundDelegateCreationExpression&>(expression);
        Instruction instruction;
        instruction.result = nextValueId_++;
        instruction.resultType = semantic::PrimitiveType::Object;
        instruction.resultTypeId = creation.delegateType.id;
        instruction.opcode = Opcode::NewDelegate;
        instruction.typeId = creation.delegateType.id;
        if (creation.closureType) {
            const auto closure = emitValue(
                Opcode::NewObject,
                semantic::PrimitiveType::Object,
                {},
                expression.span);
            auto& allocation =
                block(*currentBlockId_).instructions.back();
            allocation.typeId = creation.closureType->id;
            allocation.resultTypeId = creation.closureType->id;
            allocation.symbolName = semantic::canonicalTypeName(
                *creation.closureType);
            for (std::size_t index = 0;
                 index < creation.captures.size(); ++index) {
                const auto captured = lowerExpression(
                    *creation.captures[index]);
                Instruction store;
                store.resultType = semantic::PrimitiveType::Void;
                store.opcode = Opcode::StoreField;
                store.operands = {closure, captured};
                store.typeId = creation.closureType->id;
                store.fieldIndex = creation.captureFields[index].index;
                store.symbolName = semantic::canonicalTypeName(
                    *creation.closureType);
                store.sourceSpan = expression.span;
                block(*currentBlockId_).instructions.push_back(
                    std::move(store));
            }
            instruction.operands.push_back(closure);
        } else if (creation.receiver) {
            instruction.operands.push_back(
                lowerExpression(*creation.receiver));
        }
        instruction.symbolId = creation.function.id;
        instruction.elementType = creation.function.returnType;
        instruction.elementTypeId = semantic::isExactType(
                creation.function.returnType)
            ? semantic::stableTypeId(creation.function.returnTypeName)
            : 0;
        instruction.virtualDispatch = creation.virtualDispatch;
        instruction.virtualSlot = creation.virtualSlot;
        instruction.interfaceDispatch = creation.interfaceDispatch;
        instruction.interfaceTypeId = creation.interfaceTypeId;
        instruction.interfaceSlot = creation.interfaceSlot;
        instruction.symbolName = creation.function.moduleName + "::" +
            (creation.function.ownerTypeName.empty()
                ? creation.function.name
                : creation.function.ownerTypeName + "." +
                    creation.function.name);
        instruction.parameterTypes.reserve(
            creation.function.parameters.size());
        instruction.parameterTypeIds.reserve(
            creation.function.parameters.size());
        for (const auto& parameter : creation.function.parameters) {
            const auto type = semantic::storageTypeOf(parameter);
            const auto& typeName = semantic::storageTypeNameOf(parameter);
            instruction.parameterTypes.push_back(type);
            instruction.parameterTypeIds.push_back(
                semantic::isExactType(type)
                    ? semantic::stableTypeId(typeName)
                    : 0);
        }
        instruction.sourceSpan = expression.span;
        block(*currentBlockId_).instructions.push_back(
            std::move(instruction));
        return block(*currentBlockId_).instructions.back().result;
    }
    case semantic::BoundNodeKind::DelegateInvocationExpression: {
        const auto& invocation = static_cast<const
            semantic::BoundDelegateInvocationExpression&>(expression);
        Instruction instruction;
        instruction.resultType = invocation.type;
        instruction.resultTypeId = semantic::isExactType(invocation.type)
            ? semantic::stableTypeId(invocation.typeName)
            : 0;
        instruction.opcode = Opcode::InvokeDelegate;
        instruction.typeId = invocation.delegateType.id;
        instruction.operands.push_back(
            lowerExpression(*invocation.delegate));
        struct Writeback {
            semantic::VariableSymbol variable;
            semantic::TypeSymbol wrapperType;
            semantic::FieldSymbol field;
            ValueId wrapper = -1;
        };
        std::vector<Writeback> writebacks;
        for (const auto& argument : invocation.arguments) {
            if (argument.modifier ==
                    semantic::ParameterModifier::None ||
                argument.modifier ==
                    semantic::ParameterModifier::In ||
                argument.forwarded) {
                instruction.operands.push_back(
                    lowerExpression(*argument.value));
                continue;
            }
            const auto wrapper = emitValue(
                Opcode::NewObject,
                semantic::PrimitiveType::Object,
                {}, expression.span);
            auto& allocation =
                block(*currentBlockId_).instructions.back();
            allocation.typeId = argument.wrapperType.id;
            allocation.resultTypeId = argument.wrapperType.id;
            allocation.symbolName = semantic::canonicalTypeName(
                argument.wrapperType);
            if (argument.modifier ==
                    semantic::ParameterModifier::Ref &&
                argument.value) {
                const auto initial = lowerExpression(*argument.value);
                Instruction store;
                store.resultType = semantic::PrimitiveType::Void;
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
            instruction.operands.push_back(wrapper);
            writebacks.push_back(Writeback{
                argument.variable,
                argument.wrapperType,
                argument.valueField,
                wrapper});
        }
        instruction.symbolName = semantic::canonicalTypeName(
            invocation.delegateType) + ".Invoke";
        instruction.sourceSpan = expression.span;
        if (invocation.type != semantic::PrimitiveType::Void) {
            instruction.result = nextValueId_++;
        }
        block(*currentBlockId_).instructions.push_back(
            std::move(instruction));
        const auto callResult =
            block(*currentBlockId_).instructions.back().result;
        for (const auto& writeback : writebacks) {
            const auto value = emitValue(
                Opcode::LoadField,
                writeback.field.type,
                {writeback.wrapper}, expression.span);
            auto& load = block(*currentBlockId_).instructions.back();
            load.typeId = writeback.wrapperType.id;
            load.resultTypeId = semantic::isExactType(
                    writeback.field.type)
                ? semantic::stableTypeId(writeback.field.typeName)
                : 0;
            load.fieldIndex = writeback.field.index;
            load.symbolName = semantic::canonicalTypeName(
                writeback.wrapperType);
            emitStoreLocal(
                writeback.variable.index, value, expression.span);
        }
        return callResult;
    }
    case semantic::BoundNodeKind::DelegateCombinationExpression: {
        const auto& combination = static_cast<const
            semantic::BoundDelegateCombinationExpression&>(expression);
        const auto left = lowerExpression(*combination.left);
        const auto right = lowerExpression(*combination.right);
        const auto result = emitValue(
            combination.remove
                ? Opcode::RemoveDelegate
                : Opcode::CombineDelegate,
            semantic::PrimitiveType::Object,
            {left, right},
            expression.span);
        auto& instruction = block(
            *currentBlockId_).instructions.back();
        instruction.typeId = combination.delegateType.id;
        instruction.resultTypeId = combination.delegateType.id;
        instruction.symbolName = semantic::canonicalTypeName(
            combination.delegateType);
        return result;
    }
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
        case semantic::ConversionKind::Numeric:
        {
            const auto value = emitValue(
                Opcode::ConvertNumeric, conversion.type, {operand},
                expression.span);
            block(*currentBlockId_).instructions.back().checkedArithmetic =
                conversion.checkedArithmetic;
            return value;
        }
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
        const auto elementTypeId = instruction.elementTypeId;
        for (std::size_t index = 0;
             index < allocation.initialValues.size(); ++index) {
            const auto indexValue = emitValue(
                Opcode::ConstantInt, semantic::PrimitiveType::Int,
                {}, expression.span);
            block(*currentBlockId_).instructions.back().integerImmediate =
                static_cast<std::int64_t>(index);
            const auto element = lowerExpression(
                *allocation.initialValues[index]);
            Instruction store;
            store.resultType = semantic::PrimitiveType::Void;
            store.opcode = Opcode::StoreElement;
            store.operands = {value, indexValue, element};
            store.elementType = allocation.elementType;
            store.elementTypeId = elementTypeId;
            store.sourceSpan = expression.span;
            block(*currentBlockId_).instructions.push_back(std::move(store));
        }
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
            std::vector<ValueId> lowered(allocation.arguments.size(), -1);
            const auto& order = allocation.argumentEvaluationOrder;
            if (order.empty()) {
                for (std::size_t index = 0; index < allocation.arguments.size(); ++index) {
                    lowered[index] = lowerExpression(*allocation.arguments[index]);
                }
            } else {
                for (const auto index : order) {
                    lowered[index] = lowerExpression(*allocation.arguments[index]);
                }
            }
            arguments.insert(arguments.end(), lowered.begin(), lowered.end());
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
        for (const auto& initializer : allocation.initializers) {
            if (initializer.kind == semantic::BoundNewObjectExpression::Initializer::Kind::Field) {
                const auto assigned = lowerExpression(*initializer.arguments.front());
                Instruction store;
                store.resultType = semantic::PrimitiveType::Void;
                store.opcode = Opcode::StoreField;
                store.operands = {value, assigned};
                store.typeId = allocation.objectType.id;
                store.fieldIndex = initializer.field.index;
                store.symbolName = semantic::canonicalTypeName(allocation.objectType);
                store.sourceSpan = expression.span;
                block(*currentBlockId_).instructions.push_back(std::move(store));
                continue;
            }
            std::vector<ValueId> arguments{value};
            for (const auto& argument : initializer.arguments) {
                arguments.push_back(lowerExpression(*argument));
            }
            (void)emitCallInstruction(
                initializer.function,
                semantic::storageReturnTypeOf(initializer.function),
                semantic::storageReturnTypeNameOf(initializer.function),
                std::move(arguments), expression.span,
                false, std::numeric_limits<std::uint32_t>::max(),
                false, 0, std::numeric_limits<std::uint32_t>::max());
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
            std::vector<ValueId> lowered(allocation.arguments.size(), -1);
            const auto& order = allocation.argumentEvaluationOrder;
            if (order.empty()) {
                for (std::size_t index = 0; index < allocation.arguments.size(); ++index) {
                    lowered[index] = lowerExpression(*allocation.arguments[index]);
                }
            } else {
                for (const auto index : order) {
                    lowered[index] = lowerExpression(*allocation.arguments[index]);
                }
            }
            arguments.insert(arguments.end(), lowered.begin(), lowered.end());
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
        for (const auto& initializer : allocation.initializers) {
            if (initializer.kind !=
                semantic::BoundNewObjectExpression::Initializer::Kind::Field) {
                throw std::logic_error(
                    "struct property/collection initializer reached MIR lowering");
            }
            const auto assigned = lowerExpression(*initializer.arguments.front());
            value = emitValue(
                Opcode::StoreStructField, semantic::PrimitiveType::Struct,
                {value, assigned}, expression.span);
            auto& store = block(*currentBlockId_).instructions.back();
            store.typeId = allocation.structType.id;
            store.resultTypeId = allocation.structType.id;
            store.fieldIndex = initializer.field.index;
            store.symbolName = semantic::canonicalTypeName(allocation.structType);
        }
        return value;
    }
    case semantic::BoundNodeKind::MemberAccessExpression: {
        const auto& member = static_cast<const semantic::BoundMemberAccessExpression&>(expression);
        const auto receiver = lowerExpression(*member.receiver);
        if (member.nullConditional) {
            const auto nullReceiver = emitTypedNull(
                semantic::PrimitiveType::Object,
                semantic::canonicalTypeName(member.ownerType),
                member.receiver->span);
            const auto hasReceiver = emitValue(
                Opcode::NotEqual, semantic::PrimitiveType::Bool,
                {receiver, nullReceiver}, expression.span);
            const auto presentBlock = createBlock();
            const auto missingBlock = createBlock();
            const auto mergeBlock = createBlock();
            const auto resultTypeId = semantic::isExactType(member.type)
                ? semantic::stableTypeId(member.typeName)
                : 0;
            const auto mergeValue = addBlockParameter(
                mergeBlock, member.type, resultTypeId);
            emitBranch(
                hasReceiver, presentBlock, missingBlock,
                {}, {}, expression.span);
            setCurrentBlock(presentBlock);
            auto present = emitValue(
                Opcode::LoadField, member.field.type,
                {receiver}, expression.span);
            auto& load = block(*currentBlockId_).instructions.back();
            load.typeId = member.ownerType.id;
            load.resultTypeId = semantic::isExactType(member.field.type)
                ? semantic::stableTypeId(member.field.typeName)
                : 0;
            load.fieldIndex = member.field.index;
            load.symbolName = semantic::canonicalTypeName(member.ownerType);
            if (member.nullConditionalNullableType.id != 0) {
                present = emitNullableValue(
                    member.nullConditionalNullableType,
                    member.nullConditionalHasValueField,
                    member.nullConditionalValueField,
                    present, expression.span);
            }
            emitJump(mergeBlock, {present}, expression.span);
            setCurrentBlock(missingBlock);
            const auto missing =
                member.nullConditionalNullableType.id != 0
                ? emitDefaultStruct(
                    member.nullConditionalNullableType, expression.span)
                : emitTypedNull(
                    member.type, member.typeName, expression.span);
            emitJump(mergeBlock, {missing}, expression.span);
            setCurrentBlock(mergeBlock);
            return mergeValue;
        }
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
        ValueId wrapper = -1;
        ValueId current = -1;
        if (assignment.wrappedVariable) {
            wrapper = emitValue(
                Opcode::LoadLocal, semantic::PrimitiveType::Object,
                {}, expression.span);
            auto& loadWrapper = block(*currentBlockId_).instructions.back();
            loadWrapper.localIndex = assignment.variable.index;
            loadWrapper.resultTypeId = assignment.wrapperType.id;
            current = emitValue(
                Opcode::LoadField, semantic::PrimitiveType::Struct,
                {wrapper}, expression.span);
            auto& loadValue = block(*currentBlockId_).instructions.back();
            loadValue.typeId = assignment.wrapperType.id;
            loadValue.resultTypeId = assignment.ownerType.id;
            loadValue.fieldIndex = assignment.wrapperValueField.index;
            loadValue.symbolName = semantic::canonicalTypeName(
                assignment.wrapperType);
        } else {
            current = emitValue(
                Opcode::LoadLocal, semantic::PrimitiveType::Struct,
                {}, expression.span);
            auto& load = block(*currentBlockId_).instructions.back();
            load.localIndex = assignment.variable.index;
            load.resultTypeId = assignment.ownerType.id;
        }
        const auto assigned = lowerExpression(*assignment.expression);
        const auto updated = emitValue(Opcode::StoreStructField, semantic::PrimitiveType::Struct,
            {current, assigned}, expression.span);
        auto& store = block(*currentBlockId_).instructions.back();
        store.typeId = assignment.ownerType.id;
        store.resultTypeId = assignment.ownerType.id;
        store.fieldIndex = assignment.field.index;
        store.symbolName = semantic::canonicalTypeName(assignment.ownerType);
        if (assignment.wrappedVariable) {
            Instruction writeback;
            writeback.resultType = semantic::PrimitiveType::Void;
            writeback.opcode = Opcode::StoreField;
            writeback.operands = {wrapper, updated};
            writeback.typeId = assignment.wrapperType.id;
            writeback.fieldIndex = assignment.wrapperValueField.index;
            writeback.symbolName = semantic::canonicalTypeName(
                assignment.wrapperType);
            writeback.sourceSpan = expression.span;
            block(*currentBlockId_).instructions.push_back(
                std::move(writeback));
        } else {
            emitStoreLocal(assignment.variable.index, updated, expression.span);
        }
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
        const auto stored = emitValue(
            Opcode::LoadField,
            semantic::PrimitiveType::Object,
            {receiver}, expression.span);
        auto& load = block(*currentBlockId_).instructions.back();
        load.typeId = invocation.ownerType.id;
        load.fieldIndex = invocation.event.storageField.index;
        load.resultTypeId = invocation.delegateType.id;
        load.symbolName = semantic::canonicalTypeName(
            invocation.ownerType);
        const auto rawNull = emitValue(
            Opcode::ConstantNull,
            semantic::PrimitiveType::Null,
            {}, expression.span);
        const auto nullValue = emitValue(
            Opcode::ConvertNullToObject,
            semantic::PrimitiveType::Object,
            {rawNull}, expression.span);
        block(*currentBlockId_).instructions.back().resultTypeId =
            invocation.delegateType.id;
        const auto empty = emitValue(
            Opcode::Equal,
            semantic::PrimitiveType::Bool,
            {stored, nullValue}, expression.span);
        const auto nextBlock = createBlock();
        const auto invokeBlock = createBlock();
        emitBranch(
            empty, nextBlock, invokeBlock, {}, {}, expression.span);
        setCurrentBlock(invokeBlock);
        Instruction call;
        call.resultType = semantic::PrimitiveType::Void;
        call.opcode = Opcode::InvokeDelegate;
        call.typeId = invocation.delegateType.id;
        call.operands.push_back(stored);
        call.operands.insert(
            call.operands.end(), values.begin(), values.end());
        call.symbolName = semantic::canonicalTypeName(
            invocation.delegateType) + ".Invoke";
        call.sourceSpan = expression.span;
        block(*currentBlockId_).instructions.push_back(std::move(call));
        emitJump(nextBlock, {}, expression.span);
        setCurrentBlock(nextBlock);
        return -1;
    }
    case semantic::BoundNodeKind::ReferenceCallExpression: {
        const auto& call = static_cast<const
            semantic::BoundReferenceCallExpression&>(expression);
        std::vector<ValueId> arguments;
        struct Writeback {
            semantic::ReferenceTargetKind targetKind =
                semantic::ReferenceTargetKind::None;
            semantic::VariableSymbol variable;
            semantic::TypeSymbol wrapperType;
            semantic::FieldSymbol field;
            ValueId wrapper = -1;
            ValueId targetReceiver = -1;
            ValueId targetIndex = -1;
            semantic::TypeSymbol targetOwnerType;
            semantic::FieldSymbol targetField;
            semantic::PrimitiveType targetElementType =
                semantic::PrimitiveType::Error;
            std::string targetElementTypeName;
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

            ValueId targetReceiver = -1;
            ValueId targetIndex = -1;
            ValueId initial = -1;
            switch (argument.targetKind) {
            case semantic::ReferenceTargetKind::Variable:
                if (argument.modifier ==
                        semantic::ParameterModifier::Ref &&
                    argument.value) {
                    initial = lowerExpression(*argument.value);
                }
                break;
            case semantic::ReferenceTargetKind::ObjectField: {
                targetReceiver = lowerExpression(*argument.targetReceiver);
                targetReceiver = emitValue(
                    Opcode::CheckNotNull,
                    semantic::PrimitiveType::Object,
                    {targetReceiver}, expression.span);
                auto& check = block(*currentBlockId_).instructions.back();
                check.typeId = argument.targetOwnerType.id;
                check.resultTypeId = argument.targetOwnerType.id;
                check.symbolName = semantic::canonicalTypeName(
                    argument.targetOwnerType);
                if (argument.modifier == semantic::ParameterModifier::Ref) {
                    initial = emitValue(
                        Opcode::LoadField, argument.targetField.type,
                        {targetReceiver}, expression.span);
                    auto& load = block(*currentBlockId_).instructions.back();
                    load.typeId = argument.targetOwnerType.id;
                    load.resultTypeId = semantic::isExactType(
                            argument.targetField.type)
                        ? semantic::stableTypeId(
                            argument.targetField.typeName)
                        : 0;
                    load.fieldIndex = argument.targetField.index;
                    load.symbolName = semantic::canonicalTypeName(
                        argument.targetOwnerType);
                }
                break;
            }
            case semantic::ReferenceTargetKind::ArrayElement:
                targetReceiver = lowerExpression(*argument.targetReceiver);
                targetIndex = lowerExpression(*argument.targetIndex);
                if (argument.modifier == semantic::ParameterModifier::Ref) {
                    initial = emitValue(
                        Opcode::LoadElement, argument.targetElementType,
                        {targetReceiver, targetIndex}, expression.span);
                    auto& load = block(*currentBlockId_).instructions.back();
                    load.elementType = argument.targetElementType;
                    load.elementTypeId = semantic::isExactType(
                            argument.targetElementType)
                        ? semantic::stableTypeId(
                            argument.targetElementTypeName)
                        : 0;
                    load.resultTypeId = load.elementTypeId;
                }
                break;
            case semantic::ReferenceTargetKind::StructField:
                targetReceiver = lowerExpression(*argument.targetReceiver);
                if (argument.modifier == semantic::ParameterModifier::Ref) {
                    initial = emitValue(
                        Opcode::LoadStructField, argument.targetField.type,
                        {targetReceiver}, expression.span);
                    auto& load = block(*currentBlockId_).instructions.back();
                    load.typeId = argument.targetOwnerType.id;
                    load.fieldIndex = argument.targetField.index;
                    load.resultTypeId = semantic::isExactType(
                            argument.targetField.type)
                        ? semantic::stableTypeId(
                            argument.targetField.typeName)
                        : 0;
                    load.symbolName = semantic::canonicalTypeName(
                        argument.targetOwnerType);
                }
                break;
            case semantic::ReferenceTargetKind::None:
                break;
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
                initial >= 0) {
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
            if (!argument.defensiveCopy) {
                writebacks.push_back(Writeback{
                    argument.targetKind,
                    argument.variable,
                    argument.wrapperType,
                    argument.valueField,
                    wrapper,
                    targetReceiver,
                    targetIndex,
                    argument.targetOwnerType,
                    argument.targetField,
                    argument.targetElementType,
                    argument.targetElementTypeName});
            }
        }

        const auto callResult = emitCallInstruction(
            call.function,
            call.type,
            semantic::storageReturnTypeNameOf(call.function),
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
            switch (writeback.targetKind) {
            case semantic::ReferenceTargetKind::Variable:
                emitStoreLocal(
                    writeback.variable.index, value, expression.span);
                break;
            case semantic::ReferenceTargetKind::ObjectField: {
                Instruction store;
                store.resultType = semantic::PrimitiveType::Void;
                store.opcode = Opcode::StoreField;
                store.operands = {writeback.targetReceiver, value};
                store.typeId = writeback.targetOwnerType.id;
                store.fieldIndex = writeback.targetField.index;
                store.symbolName = semantic::canonicalTypeName(
                    writeback.targetOwnerType);
                store.sourceSpan = expression.span;
                block(*currentBlockId_).instructions.push_back(
                    std::move(store));
                break;
            }
            case semantic::ReferenceTargetKind::ArrayElement: {
                Instruction store;
                store.resultType = semantic::PrimitiveType::Void;
                store.opcode = Opcode::StoreElement;
                store.operands = {
                    writeback.targetReceiver,
                    writeback.targetIndex,
                    value};
                store.elementType = writeback.targetElementType;
                store.elementTypeId = semantic::isExactType(
                        writeback.targetElementType)
                    ? semantic::stableTypeId(
                        writeback.targetElementTypeName)
                    : 0;
                store.sourceSpan = expression.span;
                block(*currentBlockId_).instructions.push_back(
                    std::move(store));
                break;
            }
            case semantic::ReferenceTargetKind::StructField: {
                const auto updated = emitValue(
                    Opcode::StoreStructField,
                    semantic::PrimitiveType::Struct,
                    {writeback.targetReceiver, value}, expression.span);
                auto& store = block(*currentBlockId_).instructions.back();
                store.typeId = writeback.targetOwnerType.id;
                store.resultTypeId = writeback.targetOwnerType.id;
                store.fieldIndex = writeback.targetField.index;
                store.symbolName = semantic::canonicalTypeName(
                    writeback.targetOwnerType);
                emitStoreLocal(
                    writeback.variable.index, updated, expression.span);
                break;
            }
            case semantic::ReferenceTargetKind::None:
                break;
            }
        }
        return callResult;
    }
    case semantic::BoundNodeKind::CallExpression: {
        const auto& call = static_cast<const semantic::BoundCallExpression&>(expression);
        if (call.nullConditional) {
            const auto receiver = lowerExpression(*call.arguments.front());
            const auto nullReceiver = emitTypedNull(
                semantic::PrimitiveType::Object,
                call.function.moduleName + "::" +
                    call.function.ownerTypeName,
                call.arguments.front()->span);
            const auto hasReceiver = emitValue(
                Opcode::NotEqual, semantic::PrimitiveType::Bool,
                {receiver, nullReceiver}, expression.span);
            const auto presentBlock = createBlock();
            const auto missingBlock = createBlock();
            const auto mergeBlock = createBlock();
            const auto resultTypeId = semantic::isExactType(call.type)
                ? semantic::stableTypeId(call.typeName)
                : 0;
            const auto mergeValue = addBlockParameter(
                mergeBlock, call.type, resultTypeId);
            emitBranch(
                hasReceiver, presentBlock, missingBlock,
                {}, {}, expression.span);
            setCurrentBlock(presentBlock);
            std::vector<ValueId> presentArguments(call.arguments.size(), -1);
            presentArguments[0] = receiver;
            if (call.argumentEvaluationOrder.empty()) {
                for (std::size_t index = 1; index < call.arguments.size(); ++index) {
                    presentArguments[index] = lowerExpression(*call.arguments[index]);
                }
            } else {
                for (const auto index : call.argumentEvaluationOrder) {
                    if (index == 0) continue;
                    presentArguments[index] = lowerExpression(*call.arguments[index]);
                }
            }
            auto present = emitCallInstruction(
                call.function, call.nullConditionalValueType,
                semantic::storageReturnTypeNameOf(call.function),
                std::move(presentArguments), expression.span,
                call.virtualDispatch, call.virtualSlot,
                call.interfaceDispatch, call.interfaceTypeId,
                call.interfaceSlot);
            if (call.nullConditionalNullableType.id != 0) {
                present = emitNullableValue(
                    call.nullConditionalNullableType,
                    call.nullConditionalHasValueField,
                    call.nullConditionalValueField,
                    present, expression.span);
            }
            emitJump(mergeBlock, {present}, expression.span);
            setCurrentBlock(missingBlock);
            const auto missing = call.nullConditionalNullableType.id != 0
                ? emitDefaultStruct(
                    call.nullConditionalNullableType, expression.span)
                : emitTypedNull(
                    call.type, call.typeName, expression.span);
            emitJump(mergeBlock, {missing}, expression.span);
            setCurrentBlock(mergeBlock);
            return mergeValue;
        }
        std::vector<ValueId> arguments(call.arguments.size(), -1);
        auto evaluationOrder = call.argumentEvaluationOrder;
        if (evaluationOrder.empty()) {
            for (std::size_t index = 0; index < call.arguments.size(); ++index) {
                evaluationOrder.push_back(index);
            }
        }
        for (const auto index : evaluationOrder) {
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
            arguments[index] = value;
        }
        return emitCallInstruction(
            call.function,
            call.type,
            semantic::storageReturnTypeNameOf(call.function),
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
        {
            const auto value = emitValue(
                (unary.type == semantic::PrimitiveType::Long ||
                 unary.type == semantic::PrimitiveType::ULong) ? Opcode::NegateLong :
                semantic::isFloatingPointType(unary.type) ? Opcode::NegateDouble :
                Opcode::NegateInt,
                unary.type, {operand}, expression.span);
            block(*currentBlockId_).instructions.back().checkedArithmetic =
                unary.checkedArithmetic;
            return value;
        }
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
        const bool isLong = numericType == semantic::PrimitiveType::Long ||
            numericType == semantic::PrimitiveType::ULong;
        const bool isDouble = semantic::isFloatingPointType(numericType);
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
        const auto value = emitValue(
            opcode, binary.type, {left, right}, expression.span);
        const auto arithmetic =
            binary.operatorKind == semantic::BoundBinaryOperatorKind::Addition ||
            binary.operatorKind == semantic::BoundBinaryOperatorKind::Subtraction ||
            binary.operatorKind == semantic::BoundBinaryOperatorKind::Multiplication ||
            binary.operatorKind == semantic::BoundBinaryOperatorKind::Division ||
            binary.operatorKind == semantic::BoundBinaryOperatorKind::Remainder;
        if (arithmetic) {
            block(*currentBlockId_).instructions.back().checkedArithmetic =
                binary.checkedArithmetic;
        }
        return value;
    }
    case semantic::BoundNodeKind::ConditionalExpression:
        return lowerConditional(static_cast<const
            semantic::BoundConditionalExpression&>(expression));
    case semantic::BoundNodeKind::NullCoalescingExpression:
        return lowerNullCoalescing(static_cast<const
            semantic::BoundNullCoalescingExpression&>(expression));
    case semantic::BoundNodeKind::SwitchExpression: {
        const auto& switchExpression = static_cast<const
            semantic::BoundSwitchExpression&>(expression);
        const auto input = lowerExpression(*switchExpression.expression);
        const auto mergeBlock = createBlock();
        const auto resultTypeId = semantic::isExactType(expression.type)
            ? semantic::stableTypeId(expression.typeName)
            : 0;
        const auto resultValue = addBlockParameter(
            mergeBlock, expression.type, resultTypeId);
        std::vector<BlockId> armBlocks;
        armBlocks.reserve(switchExpression.arms.size());
        std::optional<std::size_t> discardIndex;
        for (std::size_t index = 0;
             index < switchExpression.arms.size(); ++index) {
            armBlocks.push_back(createBlock());
            if (switchExpression.arms[index].discard) {
                discardIndex = index;
            }
        }
        for (std::size_t index = 0;
             index < switchExpression.arms.size(); ++index) {
            const auto& arm = switchExpression.arms[index];
            if (arm.discard) continue;
            const auto nextCheck = createBlock();
            ValueId matched = -1;
            if (arm.patternType != semantic::PrimitiveType::Error) {
                matched = emitValue(
                    Opcode::IsType, semantic::PrimitiveType::Bool,
                    {input}, arm.span);
                auto& test = block(*currentBlockId_).instructions.back();
                test.elementType = arm.patternType;
                test.elementTypeId = arm.patternTypeId;
                test.parameterTypes = {switchExpression.expression->type};
                test.symbolName = arm.patternTypeName;
                if (arm.patternVariable) {
                    const auto cast = emitValue(
                        Opcode::AsType, arm.patternType, {input}, arm.span);
                    auto& asType = block(*currentBlockId_).instructions.back();
                    asType.elementType = arm.patternType;
                    asType.elementTypeId = arm.patternTypeId;
                    asType.parameterTypes = {switchExpression.expression->type};
                    asType.resultTypeId = arm.patternTypeId;
                    asType.symbolName = arm.patternTypeName;
                    emitStoreLocal(
                        arm.patternVariable->index, cast, arm.span);
                }
            } else {
                const auto label = lowerExpression(*arm.label);
                matched = emitValue(
                    Opcode::Equal, semantic::PrimitiveType::Bool,
                    {input, label}, arm.span);
            }
            if (arm.guard) {
                const auto guardBlock = createBlock();
                emitBranch(matched, guardBlock, nextCheck, {}, {}, arm.span);
                setCurrentBlock(guardBlock);
                emitBranch(
                    lowerExpression(*arm.guard), armBlocks[index], nextCheck,
                    {}, {}, arm.span);
            } else {
                emitBranch(
                    matched, armBlocks[index], nextCheck,
                    {}, {}, arm.span);
            }
            setCurrentBlock(nextCheck);
        }
        if (!discardIndex) {
            throw std::logic_error(
                "non-exhaustive switch expression reached MIR lowering");
        }
        const auto& discard = switchExpression.arms[*discardIndex];
        if (discard.guard) {
            emitBranch(
                lowerExpression(*discard.guard), armBlocks[*discardIndex],
                mergeBlock, {}, {}, discard.span);
        } else {
            emitJump(armBlocks[*discardIndex], {}, expression.span);
        }
        for (std::size_t index = 0;
             index < switchExpression.arms.size(); ++index) {
            setCurrentBlock(armBlocks[index]);
            const auto value = lowerExpression(
                *switchExpression.arms[index].value);
            emitJump(mergeBlock, {value}, switchExpression.arms[index].span);
        }
        setCurrentBlock(mergeBlock);
        return resultValue;
    }
    case semantic::BoundNodeKind::TypeBinaryExpression: {
        const auto& typeTest = static_cast<const
            semantic::BoundTypeBinaryExpression&>(expression);
        const auto operand = lowerExpression(*typeTest.expression);
        const auto value = emitValue(
            typeTest.safeCast ? Opcode::AsType : Opcode::IsType,
            typeTest.type,
            {operand}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.elementType = typeTest.targetType;
        instruction.elementTypeId = typeTest.targetTypeId;
        instruction.parameterTypes = {typeTest.expression->type};
        instruction.resultTypeId = semantic::isExactType(typeTest.type)
            ? typeTest.targetTypeId
            : 0;
        instruction.symbolName = typeTest.targetTypeName;
        if (typeTest.patternVariable) {
            const auto cast = emitValue(
                Opcode::AsType, typeTest.targetType, {operand}, expression.span);
            auto& castInstruction = block(*currentBlockId_).instructions.back();
            castInstruction.elementType = typeTest.targetType;
            castInstruction.elementTypeId = typeTest.targetTypeId;
            castInstruction.parameterTypes = {typeTest.expression->type};
            castInstruction.resultTypeId = typeTest.targetTypeId;
            castInstruction.symbolName = typeTest.targetTypeName;
            emitStoreLocal(
                typeTest.patternVariable->index, cast, expression.span);
        }
        return value;
    }
    case semantic::BoundNodeKind::TypeOfExpression: {
        const auto& typeOf = static_cast<const
            semantic::BoundTypeOfExpression&>(expression);
        const auto value = emitValue(
            Opcode::ConstantTypeId, semantic::PrimitiveType::ULong,
            {}, expression.span);
        block(*currentBlockId_).instructions.back().integerImmediate =
            static_cast<std::int64_t>(typeOf.queriedTypeId);
        return value;
    }
    default:
        break;
    }
    throw std::logic_error("unsupported bound expression in MIR lowerer");
}

ValueId Lowerer::lowerConditional(
    const semantic::BoundConditionalExpression& expression) {
    const auto condition = lowerExpression(*expression.condition);
    const auto trueBlock = createBlock();
    const auto falseBlock = createBlock();
    const auto mergeBlock = createBlock();
    const auto typeId = semantic::isExactType(expression.type)
        ? semantic::stableTypeId(expression.typeName)
        : 0;
    const auto mergeValue = addBlockParameter(
        mergeBlock, expression.type, typeId);
    emitBranch(
        condition, trueBlock, falseBlock, {}, {}, expression.span);

    setCurrentBlock(trueBlock);
    const auto whenTrue = lowerExpression(*expression.whenTrue);
    emitJump(mergeBlock, {whenTrue}, expression.whenTrue->span);

    setCurrentBlock(falseBlock);
    const auto whenFalse = lowerExpression(*expression.whenFalse);
    emitJump(mergeBlock, {whenFalse}, expression.whenFalse->span);

    setCurrentBlock(mergeBlock);
    return mergeValue;
}

ValueId Lowerer::lowerNullCoalescing(
    const semantic::BoundNullCoalescingExpression& expression) {
    const auto left = lowerExpression(*expression.left);
    if (expression.nullableValue) {
        const auto hasValue = emitValue(
            Opcode::LoadStructField, semantic::PrimitiveType::Bool,
            {left}, expression.span);
        auto& hasValueLoad = block(*currentBlockId_).instructions.back();
        hasValueLoad.typeId = expression.nullableType.id;
        hasValueLoad.fieldIndex = expression.hasValueField.index;
        hasValueLoad.symbolName = semantic::canonicalTypeName(
            expression.nullableType);
        const auto presentBlock = createBlock();
        const auto fallbackBlock = createBlock();
        const auto mergeBlock = createBlock();
        const auto typeId = semantic::isExactType(expression.type)
            ? semantic::stableTypeId(expression.typeName)
            : 0;
        const auto mergeValue = addBlockParameter(
            mergeBlock, expression.type, typeId);
        emitBranch(
            hasValue, presentBlock, fallbackBlock,
            {}, {}, expression.span);
        setCurrentBlock(presentBlock);
        const auto present = emitValue(
            Opcode::LoadStructField, expression.valueField.type,
            {left}, expression.span);
        auto& valueLoad = block(*currentBlockId_).instructions.back();
        valueLoad.typeId = expression.nullableType.id;
        valueLoad.fieldIndex = expression.valueField.index;
        valueLoad.resultTypeId = typeId;
        valueLoad.symbolName = semantic::canonicalTypeName(
            expression.nullableType);
        emitJump(mergeBlock, {present}, expression.span);
        setCurrentBlock(fallbackBlock);
        const auto fallback = lowerExpression(*expression.right);
        emitJump(mergeBlock, {fallback}, expression.right->span);
        setCurrentBlock(mergeBlock);
        return mergeValue;
    }
    const auto rawNull = emitValue(
        Opcode::ConstantNull, semantic::PrimitiveType::Null, {},
        expression.span);
    Opcode conversionOpcode = Opcode::ConvertNullToObject;
    if (expression.type == semantic::PrimitiveType::String) {
        conversionOpcode = Opcode::ConvertNullToString;
    } else if (expression.type == semantic::PrimitiveType::Array) {
        conversionOpcode = Opcode::ConvertNullToArray;
    }
    const auto typedNull = emitValue(
        conversionOpcode, expression.type, {rawNull}, expression.span);
    if (semantic::isExactType(expression.type)) {
        block(*currentBlockId_).instructions.back().resultTypeId =
            semantic::stableTypeId(expression.typeName);
    }
    const auto hasValue = emitValue(
        Opcode::NotEqual, semantic::PrimitiveType::Bool,
        {left, typedNull}, expression.span);
    const auto fallbackBlock = createBlock();
    const auto mergeBlock = createBlock();
    const auto typeId = semantic::isExactType(expression.type)
        ? semantic::stableTypeId(expression.typeName)
        : 0;
    const auto mergeValue = addBlockParameter(
        mergeBlock, expression.type, typeId);
    emitBranch(
        hasValue, mergeBlock, fallbackBlock,
        {left}, {}, expression.span);

    setCurrentBlock(fallbackBlock);
    const auto fallback = lowerExpression(*expression.right);
    emitJump(mergeBlock, {fallback}, expression.right->span);
    setCurrentBlock(mergeBlock);
    return mergeValue;
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

void Lowerer::emitThrow(ValueId value, text::TextSpan sourceSpan) {
    if (!hasCurrentBlock() || currentBlockTerminated()) {
        throw std::logic_error("cannot emit throw into a closed MIR block");
    }
    auto& terminator = block(*currentBlockId_).terminator;
    terminator.kind = TerminatorKind::Throw;
    terminator.value = value;
    terminator.sourceSpan = sourceSpan;
}

} // namespace realscript::mir
