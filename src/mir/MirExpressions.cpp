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
        if (function.moduleName == "RealScript.Async" &&
            function.ownerTypeName == "Async" &&
            function.name.rfind("__", 0) == 0) {
            instruction.symbolName = "$intrinsic::Async." + function.name;
            instruction.symbolId = semantic::stableTypeId(instruction.symbolName);
        }
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
    auto emitLoadPatternVar = [&](const semantic::VariableSymbol& var,
                                  text::TextSpan span) -> ValueId {
        const auto st = semantic::storageTypeOf(var);
        const auto& stName = semantic::storageTypeNameOf(var);
        const auto raw = emitValue(Opcode::LoadLocal, st, {}, span);
        auto& loadInst = block(*currentBlockId_).instructions.back();
        loadInst.localIndex = var.index;
        loadInst.resultTypeId = semantic::isExactType(st)
            ? semantic::stableTypeId(stName)
            : 0;
        if (st != var.type && var.modifier == semantic::ParameterModifier::None) {
            const auto cellTypeId = loadInst.resultTypeId;
            const auto inner = emitValue(Opcode::LoadField, var.type, {raw}, span);
            auto& fld = block(*currentBlockId_).instructions.back();
            fld.typeId = cellTypeId;
            fld.resultTypeId = semantic::isExactType(var.type)
                ? semantic::stableTypeId(var.typeName)
                : 0;
            fld.fieldIndex = 0;
            fld.symbolName = stName;
            return inner;
        }
        return raw;
    };
    auto emitStorePatternVar = [&](const semantic::VariableSymbol& var,
                                   ValueId val,
                                   text::TextSpan span) {
        if (semantic::storageTypeOf(var) != var.type &&
            var.modifier == semantic::ParameterModifier::None) {
            const auto cell = emitValue(
                Opcode::NewObject, semantic::PrimitiveType::Object, {}, span);
            auto& alloc = block(*currentBlockId_).instructions.back();
            alloc.typeId = semantic::stableTypeId(semantic::storageTypeNameOf(var));
            alloc.resultTypeId = alloc.typeId;
            alloc.symbolName = semantic::storageTypeNameOf(var);
            const auto cellTid = alloc.typeId;
            const auto cellSym = alloc.symbolName;
            Instruction storeField;
            storeField.resultType = semantic::PrimitiveType::Void;
            storeField.opcode = Opcode::StoreField;
            storeField.operands = {cell, val};
            storeField.typeId = cellTid;
            storeField.fieldIndex = 0;
            storeField.symbolName = cellSym;
            storeField.sourceSpan = span;
            block(*currentBlockId_).instructions.push_back(std::move(storeField));
            emitStoreLocal(var.index, cell, span);
            return;
        }
        emitStoreLocal(var.index, val, span);
    };
    auto emitConvertToCarrier = [&](ValueId val,
                                    semantic::PrimitiveType fromType,
                                    semantic::PrimitiveType toType,
                                    text::TextSpan span) -> ValueId {
        if (fromType == toType) return val;
        if (fromType == semantic::PrimitiveType::Int &&
            toType == semantic::PrimitiveType::Long) {
            return emitValue(Opcode::ConvertIntToLong, toType, {val}, span);
        }
        if (fromType == semantic::PrimitiveType::Int &&
            toType == semantic::PrimitiveType::Double) {
            return emitValue(Opcode::ConvertIntToDouble, toType, {val}, span);
        }
        if (fromType == semantic::PrimitiveType::Long &&
            toType == semantic::PrimitiveType::Double) {
            return emitValue(Opcode::ConvertLongToDouble, toType, {val}, span);
        }
        const auto conv = emitValue(Opcode::ConvertNumeric, toType, {val}, span);
        block(*currentBlockId_).instructions.back().checkedArithmetic = false;
        return conv;
    };
    auto emitDefaultValueForType = [&](semantic::PrimitiveType t,
                                       const std::string& tName,
                                       text::TextSpan span) -> ValueId {
        const auto tid = semantic::isExactType(t) ? semantic::stableTypeId(tName) : 0;
        if (t == semantic::PrimitiveType::Int ||
            t == semantic::PrimitiveType::Long ||
            t == semantic::PrimitiveType::Enum) {
            const auto v = emitValue(Opcode::ConstantInt, t, {}, span);
            auto& inst = block(*currentBlockId_).instructions.back();
            inst.integerImmediate = 0;
            inst.resultTypeId = (t == semantic::PrimitiveType::Enum) ? tid : 0;
            return v;
        }
        if (semantic::isIntegralType(t)) {
            const auto v = emitValue(Opcode::ConstantInt, semantic::PrimitiveType::Int, {}, span);
            block(*currentBlockId_).instructions.back().integerImmediate = 0;
            const auto c = emitValue(Opcode::ConvertNumeric, t, {v}, span);
            block(*currentBlockId_).instructions.back().checkedArithmetic = false;
            return c;
        }
        if (t == semantic::PrimitiveType::Double || t == semantic::PrimitiveType::Float) {
            const auto v = emitValue(
                Opcode::ConstantDouble, semantic::PrimitiveType::Double, {}, span);
            block(*currentBlockId_).instructions.back().doubleImmediate = 0.0;
            if (t == semantic::PrimitiveType::Float) {
                const auto c = emitValue(Opcode::ConvertNumeric, t, {v}, span);
                block(*currentBlockId_).instructions.back().checkedArithmetic = false;
                return c;
            }
            return v;
        }
        if (t == semantic::PrimitiveType::Bool) {
            const auto v = emitValue(Opcode::ConstantBool, t, {}, span);
            block(*currentBlockId_).instructions.back().boolImmediate = false;
            return v;
        }
        if (t == semantic::PrimitiveType::Struct) {
            const auto v = emitValue(Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, span);
            auto& inst = block(*currentBlockId_).instructions.back();
            inst.typeId = tid;
            inst.resultTypeId = tid;
            inst.symbolName = tName;
            return v;
        }
        return emitTypedNull(t, tName, span);
    };
    auto emitPatternBranch = [&](auto& self,
                                 const semantic::BoundPattern& pat,
                                 const semantic::VariableSymbol& inputVar,
                                 BlockId trueTarget,
                                 BlockId falseTarget) -> void {
        if (pat.kind == semantic::BoundPatternKind::Constant &&
            pat.constantNullCheck && pat.unwrapsNullableStruct) {
            const auto sVal = emitLoadPatternVar(inputVar, pat.span);
            const auto hasVal = emitValue(
                Opcode::LoadStructField, semantic::PrimitiveType::Bool, {sVal}, pat.span);
            auto& loadInst = block(*currentBlockId_).instructions.back();
            loadInst.typeId = pat.nullableStructType.id;
            loadInst.fieldIndex = pat.nullableHasValueField.index;
            loadInst.symbolName = semantic::canonicalTypeName(pat.nullableStructType);
            emitBranch(hasVal, falseTarget, trueTarget, {}, {}, pat.span);
            return;
        }

        semantic::VariableSymbol activeVar = inputVar;
        if (pat.unwrapsNullableStruct && pat.unwrappedTempVariable) {
            const auto sVal = emitLoadPatternVar(inputVar, pat.span);
            const auto hasVal = emitValue(
                Opcode::LoadStructField, semantic::PrimitiveType::Bool, {sVal}, pat.span);
            auto& hasLoad = block(*currentBlockId_).instructions.back();
            hasLoad.typeId = pat.nullableStructType.id;
            hasLoad.fieldIndex = pat.nullableHasValueField.index;
            hasLoad.symbolName = semantic::canonicalTypeName(pat.nullableStructType);
            const auto unwrapBlock = createBlock();
            emitBranch(hasVal, unwrapBlock, falseTarget, {}, {}, pat.span);
            setCurrentBlock(unwrapBlock);
            const auto sVal2 = emitLoadPatternVar(inputVar, pat.span);
            const auto innerVal = emitValue(
                Opcode::LoadStructField, pat.nullableValueField.type, {sVal2}, pat.span);
            auto& valLoad = block(*currentBlockId_).instructions.back();
            valLoad.typeId = pat.nullableStructType.id;
            valLoad.fieldIndex = pat.nullableValueField.index;
            valLoad.resultTypeId = semantic::isExactType(pat.nullableValueField.type)
                ? semantic::stableTypeId(pat.nullableValueField.typeName)
                : 0;
            valLoad.symbolName = semantic::canonicalTypeName(pat.nullableStructType);
            emitStoreLocal(pat.unwrappedTempVariable->index, innerVal, pat.span);
            activeVar = *pat.unwrappedTempVariable;
        }

        switch (pat.kind) {
        case semantic::BoundPatternKind::Discard:
            emitJump(trueTarget, {}, pat.span);
            return;
        case semantic::BoundPatternKind::Var:
            if (pat.variable) {
                const auto curVal = emitLoadPatternVar(activeVar, pat.span);
                emitStorePatternVar(*pat.variable, curVal, pat.span);
            }
            emitJump(trueTarget, {}, pat.span);
            return;
        case semantic::BoundPatternKind::Constant: {
            if (pat.hasStaticBool) {
                emitJump(pat.staticBoolValue ? trueTarget : falseTarget, {}, pat.span);
                return;
            }
            if (!pat.constantValue && !pat.constantNullCheck) {
                emitJump(falseTarget, {}, pat.span);
                return;
            }
            auto lhs = emitLoadPatternVar(activeVar, pat.span);
            if (pat.constantNullCheck) {
                const auto nullVal = emitTypedNull(activeVar.type, activeVar.typeName, pat.span);
                const auto eq = emitValue(
                    Opcode::Equal, semantic::PrimitiveType::Bool, {lhs, nullVal}, pat.span);
                emitBranch(eq, trueTarget, falseTarget, {}, {}, pat.span);
                return;
            }
            lhs = emitConvertToCarrier(lhs, activeVar.type, pat.comparisonType, pat.span);
            const auto rhs = lowerExpression(*pat.constantValue);
            const auto eq = emitValue(
                Opcode::Equal, semantic::PrimitiveType::Bool, {lhs, rhs}, pat.span);
            emitBranch(eq, trueTarget, falseTarget, {}, {}, pat.span);
            return;
        }
        case semantic::BoundPatternKind::Relational: {
            if (!pat.constantValue) {
                emitJump(falseTarget, {}, pat.span);
                return;
            }
            auto lhs = emitLoadPatternVar(activeVar, pat.span);
            lhs = emitConvertToCarrier(lhs, activeVar.type, pat.comparisonType, pat.span);
            const auto rhs = lowerExpression(*pat.constantValue);
            const bool isDouble = semantic::isFloatingPointType(pat.comparisonType);
            const bool isLong = (pat.comparisonType == semantic::PrimitiveType::Long ||
                                 pat.comparisonType == semantic::PrimitiveType::ULong);
            Opcode cmpOp = Opcode::LessInt;
            switch (pat.comparisonOperator) {
            case semantic::BoundBinaryOperatorKind::Less:
                cmpOp = isDouble ? Opcode::LessDouble
                    : (isLong ? Opcode::LessLong : Opcode::LessInt);
                break;
            case semantic::BoundBinaryOperatorKind::LessOrEquals:
                cmpOp = isDouble ? Opcode::LessOrEqualDouble
                    : (isLong ? Opcode::LessOrEqualLong : Opcode::LessOrEqualInt);
                break;
            case semantic::BoundBinaryOperatorKind::Greater:
                cmpOp = isDouble ? Opcode::GreaterDouble
                    : (isLong ? Opcode::GreaterLong : Opcode::GreaterInt);
                break;
            default:
                cmpOp = isDouble ? Opcode::GreaterOrEqualDouble
                    : (isLong ? Opcode::GreaterOrEqualLong : Opcode::GreaterOrEqualInt);
                break;
            }
            const auto cmpVal = emitValue(
                cmpOp, semantic::PrimitiveType::Bool, {lhs, rhs}, pat.span);
            emitBranch(cmpVal, trueTarget, falseTarget, {}, {}, pat.span);
            return;
        }
        case semantic::BoundPatternKind::LogicalNot:
            if (!pat.left) {
                emitJump(falseTarget, {}, pat.span);
                return;
            }
            self(self, *pat.left, activeVar, falseTarget, trueTarget);
            return;
        case semantic::BoundPatternKind::LogicalAnd: {
            if (!pat.left || !pat.right) {
                emitJump(falseTarget, {}, pat.span);
                return;
            }
            const auto rhsBlock = createBlock();
            self(self, *pat.left, activeVar, rhsBlock, falseTarget);
            setCurrentBlock(rhsBlock);
            self(self, *pat.right, activeVar, trueTarget, falseTarget);
            return;
        }
        case semantic::BoundPatternKind::LogicalOr: {
            if (!pat.left || !pat.right) {
                emitJump(falseTarget, {}, pat.span);
                return;
            }
            const auto rhsBlock = createBlock();
            self(self, *pat.left, activeVar, trueTarget, rhsBlock);
            setCurrentBlock(rhsBlock);
            self(self, *pat.right, activeVar, trueTarget, falseTarget);
            return;
        }
        case semantic::BoundPatternKind::Type: {
            if (pat.staticTypeMatch) {
                if (pat.variable) {
                    const auto curVal = emitLoadPatternVar(activeVar, pat.span);
                    emitStorePatternVar(*pat.variable, curVal, pat.span);
                }
                emitJump(trueTarget, {}, pat.span);
                return;
            }
            const auto curVal = emitLoadPatternVar(activeVar, pat.span);
            const auto isMatch = emitValue(
                Opcode::IsType, semantic::PrimitiveType::Bool, {curVal}, pat.span);
            auto& test = block(*currentBlockId_).instructions.back();
            test.elementType = pat.targetType;
            test.elementTypeId = pat.targetTypeId;
            test.parameterTypes = {activeVar.type};
            test.symbolName = pat.targetTypeName;
            if (pat.variable) {
                const auto castBlock = createBlock();
                emitBranch(isMatch, castBlock, falseTarget, {}, {}, pat.span);
                setCurrentBlock(castBlock);
                const auto curVal2 = emitLoadPatternVar(activeVar, pat.span);
                const auto castVal = emitValue(
                    Opcode::AsType, pat.targetType, {curVal2}, pat.span);
                auto& asInst = block(*currentBlockId_).instructions.back();
                asInst.elementType = pat.targetType;
                asInst.elementTypeId = pat.targetTypeId;
                asInst.parameterTypes = {activeVar.type};
                asInst.resultTypeId = pat.targetTypeId;
                asInst.symbolName = pat.targetTypeName;
                emitStorePatternVar(*pat.variable, castVal, pat.span);
                emitJump(trueTarget, {}, pat.span);
                return;
            }
            emitBranch(isMatch, trueTarget, falseTarget, {}, {}, pat.span);
            return;
        }
        case semantic::BoundPatternKind::Property: {
            if (!pat.receiverTempVariable) {
                emitJump(falseTarget, {}, pat.span);
                return;
            }
            if (pat.hasTypeCheck && !pat.staticTypeMatch) {
                const auto curVal = emitLoadPatternVar(activeVar, pat.span);
                const auto isMatch = emitValue(
                    Opcode::IsType, semantic::PrimitiveType::Bool, {curVal}, pat.span);
                auto& test = block(*currentBlockId_).instructions.back();
                test.elementType = pat.targetType;
                test.elementTypeId = pat.targetTypeId;
                test.parameterTypes = {activeVar.type};
                test.symbolName = pat.targetTypeName;
                const auto castBlock = createBlock();
                emitBranch(isMatch, castBlock, falseTarget, {}, {}, pat.span);
                setCurrentBlock(castBlock);
                const auto curVal2 = emitLoadPatternVar(activeVar, pat.span);
                const auto castVal = emitValue(
                    Opcode::AsType, pat.targetType, {curVal2}, pat.span);
                auto& asInst = block(*currentBlockId_).instructions.back();
                asInst.elementType = pat.targetType;
                asInst.elementTypeId = pat.targetTypeId;
                asInst.parameterTypes = {activeVar.type};
                asInst.resultTypeId = pat.targetTypeId;
                asInst.symbolName = pat.targetTypeName;
                emitStoreLocal(pat.receiverTempVariable->index, castVal, pat.span);
            } else if (semantic::isReferenceType(activeVar.type)) {
                const auto curVal = emitLoadPatternVar(activeVar, pat.span);
                const auto nullVal = emitTypedNull(activeVar.type, activeVar.typeName, pat.span);
                const auto nonNull = emitValue(
                    Opcode::NotEqual, semantic::PrimitiveType::Bool,
                    {curVal, nullVal}, pat.span);
                const auto nonNullBlock = createBlock();
                emitBranch(nonNull, nonNullBlock, falseTarget, {}, {}, pat.span);
                setCurrentBlock(nonNullBlock);
                const auto curVal2 = emitLoadPatternVar(activeVar, pat.span);
                emitStoreLocal(pat.receiverTempVariable->index, curVal2, pat.span);
            } else {
                const auto curVal = emitLoadPatternVar(activeVar, pat.span);
                emitStoreLocal(pat.receiverTempVariable->index, curVal, pat.span);
            }

            for (const auto& sub : pat.properties) {
                if (!sub.memberTempVariable || !sub.memberAccess || !sub.pattern) {
                    emitJump(falseTarget, {}, sub.span);
                    return;
                }
                const auto memVal = lowerExpression(*sub.memberAccess);
                emitStoreLocal(sub.memberTempVariable->index, memVal, sub.span);
                const auto nextSubBlock = createBlock();
                self(self, *sub.pattern, *sub.memberTempVariable, nextSubBlock, falseTarget);
                setCurrentBlock(nextSubBlock);
            }

            if (pat.variable) {
                const auto recvVal = emitLoadPatternVar(*pat.receiverTempVariable, pat.span);
                emitStorePatternVar(*pat.variable, recvVal, pat.span);
            }
            emitJump(trueTarget, {}, pat.span);
            return;
        }
        }
        emitJump(falseTarget, {}, pat.span);
    };

    switch (expression.kind()) {
    case semantic::BoundNodeKind::AwaitExpression: {
        const auto& awaitExpr = static_cast<const
            semantic::BoundAwaitExpression&>(expression);
        const auto operandVal = lowerExpression(*awaitExpr.operand);
        const auto awaitLocalIndex = currentFunction_->localTypes.size();
        currentFunction_->localTypes.push_back(awaitExpr.operand->type);
        currentFunction_->localTypeIds.push_back(awaitExpr.operandTypeId);

        const auto resumeBlock = createBlock();

        Instruction suspendInst;
        suspendInst.result = -1;
        suspendInst.resultType = semantic::PrimitiveType::Void;
        suspendInst.opcode = Opcode::AwaitSuspend;
        suspendInst.operands = {operandVal};
        suspendInst.localIndex = awaitLocalIndex;
        suspendInst.typeId = awaitExpr.operandTypeId;
        suspendInst.symbolName = awaitExpr.operand->typeName;
        suspendInst.integerImmediate = static_cast<std::int64_t>(resumeBlock);
        suspendInst.sourceSpan = expression.span;
        block(*currentBlockId_).instructions.push_back(std::move(suspendInst));

        emitJump(resumeBlock, {}, expression.span);
        setCurrentBlock(resumeBlock);

        const bool isVoidResult = (awaitExpr.type == semantic::PrimitiveType::Void);
        Instruction resumeInst;
        resumeInst.result = isVoidResult ? -1 : nextValueId_++;
        resumeInst.resultType = awaitExpr.type;
        resumeInst.resultTypeId = semantic::isExactType(awaitExpr.type)
            ? semantic::stableTypeId(awaitExpr.typeName)
            : 0;
        resumeInst.opcode = Opcode::AwaitResume;
        resumeInst.localIndex = awaitLocalIndex;
        resumeInst.typeId = awaitExpr.operandTypeId;
        resumeInst.elementType = awaitExpr.type;
        resumeInst.elementTypeId = resumeInst.resultTypeId;
        resumeInst.symbolName = awaitExpr.operand->typeName;
        resumeInst.sourceSpan = expression.span;
        block(*currentBlockId_).instructions.push_back(std::move(resumeInst));
        return isVoidResult ? -1 : block(*currentBlockId_).instructions.back().result;
    }
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
            std::optional<semantic::FunctionSymbol> assignHandle;
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
            } else if (argument.modifier ==
                           semantic::ParameterModifier::Out &&
                       argument.valueField.type ==
                           semantic::PrimitiveType::Struct &&
                       !argument.valueField.typeName.empty()) {
                const auto defaultStruct = emitValue(
                    Opcode::NewStruct,
                    semantic::PrimitiveType::Struct,
                    {}, expression.span);
                auto& structInst =
                    block(*currentBlockId_).instructions.back();
                structInst.typeId = semantic::stableTypeId(
                    argument.valueField.typeName);
                structInst.resultTypeId = structInst.typeId;
                structInst.symbolName = argument.valueField.typeName;
                Instruction store;
                store.resultType = semantic::PrimitiveType::Void;
                store.opcode = Opcode::StoreField;
                store.operands = {wrapper, defaultStruct};
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
                wrapper,
                argument.writebackAssignHandle});
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
            auto value = emitValue(
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
            const bool capturedCell =
                (semantic::storageTypeOf(writeback.variable) !=
                     writeback.variable.type ||
                 semantic::storageTypeNameOf(writeback.variable) !=
                     writeback.variable.typeName) &&
                writeback.variable.modifier ==
                    semantic::ParameterModifier::None;
            if (writeback.assignHandle) {
                ValueId oldVal = -1;
                if (capturedCell) {
                    const auto cell = emitValue(
                        Opcode::LoadLocal,
                        semantic::PrimitiveType::Object,
                        {}, expression.span);
                    auto& cellLoad =
                        block(*currentBlockId_).instructions.back();
                    cellLoad.localIndex = writeback.variable.index;
                    cellLoad.resultTypeId = semantic::stableTypeId(
                        semantic::storageTypeNameOf(writeback.variable));
                    oldVal = emitValue(
                        Opcode::LoadField,
                        writeback.field.type,
                        {cell}, expression.span);
                    auto& oldFieldLoad =
                        block(*currentBlockId_).instructions.back();
                    oldFieldLoad.typeId = cellLoad.resultTypeId;
                    oldFieldLoad.resultTypeId = semantic::isExactType(
                            writeback.field.type)
                        ? semantic::stableTypeId(writeback.field.typeName)
                        : 0;
                    oldFieldLoad.fieldIndex = 0;
                    oldFieldLoad.symbolName =
                        semantic::storageTypeNameOf(writeback.variable);
                } else {
                    oldVal = emitValue(
                        Opcode::LoadLocal,
                        writeback.field.type,
                        {}, expression.span);
                    auto& oldLocalLoad =
                        block(*currentBlockId_).instructions.back();
                    oldLocalLoad.localIndex = writeback.variable.index;
                    oldLocalLoad.resultTypeId = semantic::isExactType(
                            writeback.field.type)
                        ? semantic::stableTypeId(writeback.field.typeName)
                        : 0;
                }
                value = emitCallInstruction(
                    *writeback.assignHandle,
                    writeback.field.type,
                    writeback.field.typeName,
                    {oldVal, value},
                    expression.span,
                    false,
                    std::numeric_limits<std::uint32_t>::max(),
                    false,
                    0,
                    std::numeric_limits<std::uint32_t>::max());
            }
            if (capturedCell) {
                const auto cell = emitValue(
                    Opcode::LoadLocal,
                    semantic::PrimitiveType::Object,
                    {}, expression.span);
                auto& cellLoad =
                    block(*currentBlockId_).instructions.back();
                cellLoad.localIndex = writeback.variable.index;
                cellLoad.resultTypeId = semantic::stableTypeId(
                    semantic::storageTypeNameOf(writeback.variable));
                Instruction storeCell;
                storeCell.resultType = semantic::PrimitiveType::Void;
                storeCell.opcode = Opcode::StoreField;
                storeCell.operands = {cell, value};
                storeCell.typeId = cellLoad.resultTypeId;
                storeCell.fieldIndex = 0;
                storeCell.symbolName =
                    semantic::storageTypeNameOf(writeback.variable);
                storeCell.sourceSpan = expression.span;
                block(*currentBlockId_).instructions.push_back(
                    std::move(storeCell));
            } else {
                emitStoreLocal(
                    writeback.variable.index, value, expression.span);
            }
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
            instruction.integerImmediate = std::holds_alternative<std::int64_t>(literal.value)
                ? std::get<std::int64_t>(literal.value)
                : 0;
            instruction.resultTypeId = literal.type == semantic::PrimitiveType::Enum
                ? semantic::stableTypeId(literal.typeName)
                : 0;
            return value;
        }
        if (semantic::isIntegralType(literal.type)) {
            const auto intVal = emitValue(
                Opcode::ConstantInt,
                semantic::PrimitiveType::Long,
                {},
                expression.span);
            block(*currentBlockId_).instructions.back().integerImmediate =
                std::holds_alternative<std::int64_t>(literal.value)
                    ? std::get<std::int64_t>(literal.value)
                    : 0;
            return emitValue(
                Opcode::ConvertNumeric,
                literal.type,
                {intVal},
                expression.span);
        }
        if (literal.type == semantic::PrimitiveType::Float) {
            const auto dblVal = emitValue(
                Opcode::ConstantDouble,
                semantic::PrimitiveType::Double,
                {},
                expression.span);
            block(*currentBlockId_).instructions.back().doubleImmediate =
                std::get<double>(literal.value);
            return emitValue(
                Opcode::ConvertNumeric,
                semantic::PrimitiveType::Float,
                {dblVal},
                expression.span);
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
        if (literal.type == semantic::PrimitiveType::Error) {
            return emitValue(Opcode::ConstantInt, semantic::PrimitiveType::Int, {}, expression.span);
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
        if (allocation.objectType.staticConstructor &&
            (!currentFunction_ || currentFunction_->symbolId != allocation.objectType.staticConstructor->id)) {
            (void)emitCallInstruction(
                *allocation.objectType.staticConstructor,
                semantic::PrimitiveType::Void,
                {},
                {},
                expression.span,
                false,
                std::numeric_limits<std::uint32_t>::max(),
                false,
                0,
                std::numeric_limits<std::uint32_t>::max());
        }
        const auto value = emitValue(Opcode::NewObject, semantic::PrimitiveType::Object, {}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.typeId = allocation.objectType.id;
        instruction.resultTypeId = allocation.objectType.id;
        instruction.symbolName = semantic::canonicalTypeName(allocation.objectType);
        auto emitConstantFieldValue = [&](const semantic::FieldSymbol& field,
                                          text::TextSpan span) -> ValueId {
            const auto fieldTypeId = semantic::isExactType(field.type)
                ? semantic::stableTypeId(field.typeName)
                : 0;
            if (field.hasConstantValue) {
                if (field.type == semantic::PrimitiveType::Int ||
                    field.type == semantic::PrimitiveType::Long ||
                    field.type == semantic::PrimitiveType::Enum) {
                    const auto v = emitValue(Opcode::ConstantInt, field.type, {}, span);
                    auto& inst = block(*currentBlockId_).instructions.back();
                    inst.integerImmediate = std::holds_alternative<std::int64_t>(field.constantValue)
                        ? std::get<std::int64_t>(field.constantValue)
                        : 0;
                    inst.resultTypeId = field.type == semantic::PrimitiveType::Enum
                        ? fieldTypeId
                        : 0;
                    return v;
                }
                if (semantic::isIntegralType(field.type)) {
                    const auto carrier = (field.type == semantic::PrimitiveType::UInt ||
                                          field.type == semantic::PrimitiveType::ULong)
                        ? semantic::PrimitiveType::Long
                        : semantic::PrimitiveType::Int;
                    const auto v = emitValue(Opcode::ConstantInt, carrier, {}, span);
                    block(*currentBlockId_).instructions.back().integerImmediate =
                        std::holds_alternative<std::int64_t>(field.constantValue)
                            ? std::get<std::int64_t>(field.constantValue)
                            : 0;
                    return emitValue(Opcode::ConvertNumeric, field.type, {v}, span);
                }
                if (field.type == semantic::PrimitiveType::Double ||
                    field.type == semantic::PrimitiveType::Float) {
                    const auto v = emitValue(
                        Opcode::ConstantDouble, semantic::PrimitiveType::Double, {}, span);
                    block(*currentBlockId_).instructions.back().doubleImmediate =
                        std::holds_alternative<double>(field.constantValue)
                            ? std::get<double>(field.constantValue)
                            : (std::holds_alternative<std::int64_t>(field.constantValue)
                                ? static_cast<double>(std::get<std::int64_t>(field.constantValue))
                                : 0.0);
                    if (field.type == semantic::PrimitiveType::Float) {
                        return emitValue(Opcode::ConvertNumeric, field.type, {v}, span);
                    }
                    return v;
                }
                if (field.type == semantic::PrimitiveType::Bool) {
                    const auto v = emitValue(Opcode::ConstantBool, field.type, {}, span);
                    block(*currentBlockId_).instructions.back().boolImmediate =
                        std::holds_alternative<bool>(field.constantValue)
                            ? std::get<bool>(field.constantValue)
                            : false;
                    return v;
                }
                if (field.type == semantic::PrimitiveType::String) {
                    if (std::holds_alternative<std::string>(field.constantValue)) {
                        const auto v = emitValue(Opcode::ConstantString, field.type, {}, span);
                        block(*currentBlockId_).instructions.back().stringImmediate =
                            std::get<std::string>(field.constantValue);
                        return v;
                    }
                    return emitTypedNull(field.type, field.typeName, span);
                }
                if (std::holds_alternative<std::monostate>(field.constantValue) &&
                    (field.type == semantic::PrimitiveType::Object ||
                     field.type == semantic::PrimitiveType::Array)) {
                    return emitTypedNull(field.type, field.typeName, span);
                }
            }
            if (field.type == semantic::PrimitiveType::Int ||
                field.type == semantic::PrimitiveType::Long ||
                field.type == semantic::PrimitiveType::Enum) {
                const auto v = emitValue(Opcode::ConstantInt, field.type, {}, span);
                auto& inst = block(*currentBlockId_).instructions.back();
                inst.integerImmediate = 0;
                inst.resultTypeId = field.type == semantic::PrimitiveType::Enum ? fieldTypeId : 0;
                return v;
            }
            if (semantic::isIntegralType(field.type)) {
                const auto v = emitValue(Opcode::ConstantInt, semantic::PrimitiveType::Int, {}, span);
                block(*currentBlockId_).instructions.back().integerImmediate = 0;
                return emitValue(Opcode::ConvertNumeric, field.type, {v}, span);
            }
            if (field.type == semantic::PrimitiveType::Double ||
                field.type == semantic::PrimitiveType::Float) {
                const auto v = emitValue(
                    Opcode::ConstantDouble, semantic::PrimitiveType::Double, {}, span);
                block(*currentBlockId_).instructions.back().doubleImmediate = 0.0;
                if (field.type == semantic::PrimitiveType::Float) {
                    return emitValue(Opcode::ConvertNumeric, field.type, {v}, span);
                }
                return v;
            }
            if (field.type == semantic::PrimitiveType::Bool) {
                const auto v = emitValue(Opcode::ConstantBool, field.type, {}, span);
                block(*currentBlockId_).instructions.back().boolImmediate = false;
                return v;
            }
            if (field.type == semantic::PrimitiveType::Struct) {
                const auto v = emitValue(
                    Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, span);
                auto& inst = block(*currentBlockId_).instructions.back();
                inst.typeId = fieldTypeId;
                inst.resultTypeId = fieldTypeId;
                inst.symbolName = field.typeName;
                return v;
            }
            return emitTypedNull(field.type, field.typeName, span);
        };
        for (const auto& field : allocation.objectType.fields) {
            if (field.staticField || field.constField || !field.hasConstantValue) {
                continue;
            }
            const auto defaultVal = emitConstantFieldValue(field, expression.span);
            Instruction store;
            store.resultType = semantic::PrimitiveType::Void;
            store.opcode = Opcode::StoreField;
            store.operands = {value, defaultVal};
            store.typeId = allocation.objectType.id;
            store.fieldIndex = field.index;
            store.symbolName = semantic::canonicalTypeName(allocation.objectType);
            store.sourceSpan = expression.span;
            block(*currentBlockId_).instructions.push_back(std::move(store));
        }
        for (const auto& initializer : allocation.defaultFieldInitializers) {
            if (initializer.arguments.empty() || !initializer.arguments.front()) {
                continue;
            }
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
        }
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
        if (allocation.structType.staticConstructor &&
            (!currentFunction_ || currentFunction_->symbolId != allocation.structType.staticConstructor->id)) {
            (void)emitCallInstruction(
                *allocation.structType.staticConstructor,
                semantic::PrimitiveType::Void,
                {},
                {},
                expression.span,
                false,
                std::numeric_limits<std::uint32_t>::max(),
                false,
                0,
                std::numeric_limits<std::uint32_t>::max());
        }
        auto value = emitValue(Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, expression.span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.typeId = allocation.structType.id;
        instruction.resultTypeId = allocation.structType.id;
        instruction.symbolName = semantic::canonicalTypeName(allocation.structType);
        for (const auto& field : allocation.structType.fields) {
            if (field.staticField || field.constField || !field.hasConstantValue) {
                continue;
            }
            const auto fieldTypeId = semantic::isExactType(field.type)
                ? semantic::stableTypeId(field.typeName)
                : 0;
            ValueId defaultVal = -1;
            if (field.type == semantic::PrimitiveType::Int ||
                field.type == semantic::PrimitiveType::Long ||
                field.type == semantic::PrimitiveType::Enum) {
                defaultVal = emitValue(Opcode::ConstantInt, field.type, {}, expression.span);
                auto& inst = block(*currentBlockId_).instructions.back();
                inst.integerImmediate = std::holds_alternative<std::int64_t>(field.constantValue)
                    ? std::get<std::int64_t>(field.constantValue)
                    : 0;
                inst.resultTypeId = field.type == semantic::PrimitiveType::Enum ? fieldTypeId : 0;
            } else if (semantic::isIntegralType(field.type)) {
                const auto carrier = (field.type == semantic::PrimitiveType::UInt ||
                                      field.type == semantic::PrimitiveType::ULong)
                    ? semantic::PrimitiveType::Long
                    : semantic::PrimitiveType::Int;
                const auto v = emitValue(Opcode::ConstantInt, carrier, {}, expression.span);
                block(*currentBlockId_).instructions.back().integerImmediate =
                    std::holds_alternative<std::int64_t>(field.constantValue)
                        ? std::get<std::int64_t>(field.constantValue)
                        : 0;
                defaultVal = emitValue(Opcode::ConvertNumeric, field.type, {v}, expression.span);
            } else if (field.type == semantic::PrimitiveType::Double ||
                       field.type == semantic::PrimitiveType::Float) {
                const auto v = emitValue(
                    Opcode::ConstantDouble, semantic::PrimitiveType::Double, {}, expression.span);
                block(*currentBlockId_).instructions.back().doubleImmediate =
                    std::holds_alternative<double>(field.constantValue)
                        ? std::get<double>(field.constantValue)
                        : (std::holds_alternative<std::int64_t>(field.constantValue)
                            ? static_cast<double>(std::get<std::int64_t>(field.constantValue))
                            : 0.0);
                defaultVal = field.type == semantic::PrimitiveType::Float
                    ? emitValue(Opcode::ConvertNumeric, field.type, {v}, expression.span)
                    : v;
            } else if (field.type == semantic::PrimitiveType::Bool) {
                defaultVal = emitValue(Opcode::ConstantBool, field.type, {}, expression.span);
                block(*currentBlockId_).instructions.back().boolImmediate =
                    std::holds_alternative<bool>(field.constantValue)
                        ? std::get<bool>(field.constantValue)
                        : false;
            } else if (field.type == semantic::PrimitiveType::String &&
                       std::holds_alternative<std::string>(field.constantValue)) {
                defaultVal = emitValue(Opcode::ConstantString, field.type, {}, expression.span);
                block(*currentBlockId_).instructions.back().stringImmediate =
                    std::get<std::string>(field.constantValue);
            }
            if (defaultVal >= 0) {
                value = emitValue(
                    Opcode::StoreStructField, semantic::PrimitiveType::Struct,
                    {value, defaultVal}, expression.span);
                auto& store = block(*currentBlockId_).instructions.back();
                store.typeId = allocation.structType.id;
                store.resultTypeId = allocation.structType.id;
                store.fieldIndex = field.index;
                store.symbolName = semantic::canonicalTypeName(allocation.structType);
            }
        }
        for (const auto& initializer : allocation.defaultFieldInitializers) {
            if (initializer.arguments.empty() || !initializer.arguments.front()) {
                continue;
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
        if (member.field.staticField) {
            if (member.ownerType.staticConstructor &&
                (!currentFunction_ || currentFunction_->symbolId != member.ownerType.staticConstructor->id)) {
                (void)emitCallInstruction(
                    *member.ownerType.staticConstructor,
                    semantic::PrimitiveType::Void,
                    {},
                    {},
                    expression.span,
                    false,
                    std::numeric_limits<std::uint32_t>::max(),
                    false,
                    0,
                    std::numeric_limits<std::uint32_t>::max());
            }
            const auto fieldTypeId = semantic::isExactType(member.field.type)
                ? semantic::stableTypeId(member.field.typeName)
                : 0;
            if (member.staticInitializer) {
                const auto hasName = "$intrinsic::static_has::" +
                    std::to_string(member.field.id);
                Instruction hasCall;
                hasCall.result = nextValueId_++;
                hasCall.resultType = semantic::PrimitiveType::Bool;
                hasCall.opcode = Opcode::Call;
                hasCall.symbolId = semantic::stableTypeId(hasName);
                hasCall.symbolName = hasName;
                hasCall.sourceSpan = expression.span;
                const auto hasVal = hasCall.result;
                block(*currentBlockId_).instructions.push_back(std::move(hasCall));

                const auto initBlock = createBlock();
                const auto doneBlock = createBlock();
                emitBranch(hasVal, doneBlock, initBlock, {}, {}, expression.span);
                setCurrentBlock(initBlock);
                const auto initVal = lowerExpression(*member.staticInitializer);
                const auto setName = "$intrinsic::static_set::" +
                    std::to_string(member.field.id);
                Instruction setCall;
                setCall.resultType = semantic::PrimitiveType::Void;
                setCall.opcode = Opcode::Call;
                setCall.operands = {initVal};
                setCall.parameterTypes = {member.field.type};
                setCall.parameterTypeIds = {fieldTypeId};
                setCall.symbolId = semantic::stableTypeId(setName);
                setCall.symbolName = setName;
                setCall.sourceSpan = expression.span;
                block(*currentBlockId_).instructions.push_back(std::move(setCall));
                emitJump(doneBlock, {}, expression.span);
                setCurrentBlock(doneBlock);
            }
            ValueId defaultVal = -1;
            const auto& field = member.field;
            if (field.hasConstantValue) {
                if (field.type == semantic::PrimitiveType::Int ||
                    field.type == semantic::PrimitiveType::Long ||
                    field.type == semantic::PrimitiveType::Enum) {
                    defaultVal = emitValue(Opcode::ConstantInt, field.type, {}, expression.span);
                    auto& inst = block(*currentBlockId_).instructions.back();
                    inst.integerImmediate = std::holds_alternative<std::int64_t>(field.constantValue)
                        ? std::get<std::int64_t>(field.constantValue)
                        : 0;
                    inst.resultTypeId = field.type == semantic::PrimitiveType::Enum ? fieldTypeId : 0;
                } else if (semantic::isIntegralType(field.type)) {
                    const auto carrier = (field.type == semantic::PrimitiveType::UInt ||
                                          field.type == semantic::PrimitiveType::ULong)
                        ? semantic::PrimitiveType::Long
                        : semantic::PrimitiveType::Int;
                    const auto v = emitValue(Opcode::ConstantInt, carrier, {}, expression.span);
                    block(*currentBlockId_).instructions.back().integerImmediate =
                        std::holds_alternative<std::int64_t>(field.constantValue)
                            ? std::get<std::int64_t>(field.constantValue)
                            : 0;
                    defaultVal = emitValue(Opcode::ConvertNumeric, field.type, {v}, expression.span);
                } else if (field.type == semantic::PrimitiveType::Double ||
                           field.type == semantic::PrimitiveType::Float) {
                    const auto v = emitValue(
                        Opcode::ConstantDouble, semantic::PrimitiveType::Double, {}, expression.span);
                    block(*currentBlockId_).instructions.back().doubleImmediate =
                        std::holds_alternative<double>(field.constantValue)
                            ? std::get<double>(field.constantValue)
                            : (std::holds_alternative<std::int64_t>(field.constantValue)
                                ? static_cast<double>(std::get<std::int64_t>(field.constantValue))
                                : 0.0);
                    defaultVal = field.type == semantic::PrimitiveType::Float
                        ? emitValue(Opcode::ConvertNumeric, field.type, {v}, expression.span)
                        : v;
                } else if (field.type == semantic::PrimitiveType::Bool) {
                    defaultVal = emitValue(Opcode::ConstantBool, field.type, {}, expression.span);
                    block(*currentBlockId_).instructions.back().boolImmediate =
                        std::holds_alternative<bool>(field.constantValue)
                            ? std::get<bool>(field.constantValue)
                            : false;
                } else if (field.type == semantic::PrimitiveType::String &&
                           std::holds_alternative<std::string>(field.constantValue)) {
                    defaultVal = emitValue(Opcode::ConstantString, field.type, {}, expression.span);
                    block(*currentBlockId_).instructions.back().stringImmediate =
                        std::get<std::string>(field.constantValue);
                }
            }
            if (defaultVal < 0) {
                if (field.type == semantic::PrimitiveType::Int ||
                    field.type == semantic::PrimitiveType::Long ||
                    field.type == semantic::PrimitiveType::Enum) {
                    defaultVal = emitValue(Opcode::ConstantInt, field.type, {}, expression.span);
                    auto& inst = block(*currentBlockId_).instructions.back();
                    inst.integerImmediate = 0;
                    inst.resultTypeId = field.type == semantic::PrimitiveType::Enum ? fieldTypeId : 0;
                } else if (semantic::isIntegralType(field.type)) {
                    const auto v = emitValue(
                        Opcode::ConstantInt, semantic::PrimitiveType::Int, {}, expression.span);
                    block(*currentBlockId_).instructions.back().integerImmediate = 0;
                    defaultVal = emitValue(Opcode::ConvertNumeric, field.type, {v}, expression.span);
                } else if (field.type == semantic::PrimitiveType::Double ||
                           field.type == semantic::PrimitiveType::Float) {
                    const auto v = emitValue(
                        Opcode::ConstantDouble, semantic::PrimitiveType::Double, {}, expression.span);
                    block(*currentBlockId_).instructions.back().doubleImmediate = 0.0;
                    defaultVal = field.type == semantic::PrimitiveType::Float
                        ? emitValue(Opcode::ConvertNumeric, field.type, {v}, expression.span)
                        : v;
                } else if (field.type == semantic::PrimitiveType::Bool) {
                    defaultVal = emitValue(Opcode::ConstantBool, field.type, {}, expression.span);
                    block(*currentBlockId_).instructions.back().boolImmediate = false;
                } else if (field.type == semantic::PrimitiveType::Struct) {
                    defaultVal = emitValue(
                        Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, expression.span);
                    auto& inst = block(*currentBlockId_).instructions.back();
                    inst.typeId = fieldTypeId;
                    inst.resultTypeId = fieldTypeId;
                    inst.symbolName = field.typeName;
                } else {
                    defaultVal = emitTypedNull(field.type, field.typeName, expression.span);
                }
            }
            const auto getName = "$intrinsic::static_get::" +
                std::to_string(member.field.id);
            Instruction getCall;
            getCall.result = nextValueId_++;
            getCall.resultType = member.field.type;
            getCall.resultTypeId = fieldTypeId;
            getCall.opcode = Opcode::Call;
            getCall.operands = {defaultVal};
            getCall.parameterTypes = {member.field.type};
            getCall.parameterTypeIds = {fieldTypeId};
            getCall.symbolId = semantic::stableTypeId(getName);
            getCall.symbolName = getName;
            getCall.sourceSpan = expression.span;
            const auto resultVal = getCall.result;
            block(*currentBlockId_).instructions.push_back(std::move(getCall));
            return resultVal;
        }
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
        if (assignment.field.staticField) {
            if (assignment.ownerType.staticConstructor &&
                (!currentFunction_ || currentFunction_->symbolId != assignment.ownerType.staticConstructor->id)) {
                (void)emitCallInstruction(
                    *assignment.ownerType.staticConstructor,
                    semantic::PrimitiveType::Void,
                    {},
                    {},
                    expression.span,
                    false,
                    std::numeric_limits<std::uint32_t>::max(),
                    false,
                    0,
                    std::numeric_limits<std::uint32_t>::max());
            }
            const auto value = lowerExpression(*assignment.expression);
            const auto fieldTypeId = semantic::isExactType(assignment.field.type)
                ? semantic::stableTypeId(assignment.field.typeName)
                : 0;
            const auto setName = "$intrinsic::static_set::" +
                std::to_string(assignment.field.id);
            Instruction setCall;
            setCall.resultType = semantic::PrimitiveType::Void;
            setCall.opcode = Opcode::Call;
            setCall.operands = {value};
            setCall.parameterTypes = {assignment.field.type};
            setCall.parameterTypeIds = {fieldTypeId};
            setCall.symbolId = semantic::stableTypeId(setName);
            setCall.symbolName = setName;
            setCall.sourceSpan = expression.span;
            block(*currentBlockId_).instructions.push_back(std::move(setCall));
            return value;
        }
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
        const auto effectiveRootKind = assignment.staticFieldReceiver
            ? semantic::StructAssignmentRootKind::StaticField
            : assignment.rootKind;
        const auto& rootStructType = assignment.intermediateHops.empty()
            ? assignment.ownerType
            : assignment.intermediateHops.front().ownerType;
        const auto rootStructTypeId = rootStructType.id;

        // Step 1: Pre-evaluate root lvalue receiver/index handles (preserving left-to-right single evaluation)
        ValueId wrapper = -1;
        ValueId checkedObjRecv = -1;
        ValueId arrRecv = -1;
        ValueId arrIdx = -1;

        if (effectiveRootKind == semantic::StructAssignmentRootKind::StaticField) {
            if (assignment.staticReceiverOwnerType.staticConstructor &&
                (!currentFunction_ || currentFunction_->symbolId != assignment.staticReceiverOwnerType.staticConstructor->id)) {
                (void)emitCallInstruction(
                    *assignment.staticReceiverOwnerType.staticConstructor,
                    semantic::PrimitiveType::Void,
                    {},
                    {},
                    expression.span,
                    false,
                    std::numeric_limits<std::uint32_t>::max(),
                    false,
                    0,
                    std::numeric_limits<std::uint32_t>::max());
            }
            if (assignment.staticReceiverInitializer) {
                const auto hasName = "$intrinsic::static_has::" +
                    std::to_string(assignment.staticReceiverField.id);
                Instruction hasCall;
                hasCall.result = nextValueId_++;
                hasCall.resultType = semantic::PrimitiveType::Bool;
                hasCall.opcode = Opcode::Call;
                hasCall.symbolId = semantic::stableTypeId(hasName);
                hasCall.symbolName = hasName;
                hasCall.sourceSpan = expression.span;
                const auto hasVal = hasCall.result;
                block(*currentBlockId_).instructions.push_back(std::move(hasCall));

                const auto initBlock = createBlock();
                const auto readyBlock = createBlock();
                emitBranch(hasVal, readyBlock, initBlock, {}, {}, expression.span);

                setCurrentBlock(initBlock);
                const auto initVal = lowerExpression(*assignment.staticReceiverInitializer);
                const auto setName = "$intrinsic::static_set::" +
                    std::to_string(assignment.staticReceiverField.id);
                Instruction setCall;
                setCall.resultType = semantic::PrimitiveType::Void;
                setCall.opcode = Opcode::Call;
                setCall.operands = {initVal};
                setCall.parameterTypes = {semantic::PrimitiveType::Struct};
                setCall.parameterTypeIds = {rootStructTypeId};
                setCall.symbolId = semantic::stableTypeId(setName);
                setCall.symbolName = setName;
                setCall.sourceSpan = expression.span;
                block(*currentBlockId_).instructions.push_back(std::move(setCall));
                emitJump(readyBlock, {}, expression.span);

                setCurrentBlock(readyBlock);
            }
        } else if (effectiveRootKind == semantic::StructAssignmentRootKind::ObjectField) {
            const auto rawObj = lowerExpression(*assignment.objectReceiver);
            checkedObjRecv = emitValue(
                Opcode::CheckNotNull,
                semantic::PrimitiveType::Object,
                {rawObj},
                assignment.objectReceiver->span);
            auto& check = block(*currentBlockId_).instructions.back();
            check.typeId = assignment.objectOwnerType.id;
            check.resultTypeId = assignment.objectOwnerType.id;
            check.symbolName = semantic::canonicalTypeName(assignment.objectOwnerType);
        } else if (effectiveRootKind == semantic::StructAssignmentRootKind::ArrayElement) {
            arrRecv = lowerExpression(*assignment.elementReceiver);
            arrIdx = lowerExpression(*assignment.elementIndex);
        } else if (assignment.wrappedVariable) {
            wrapper = emitValue(
                Opcode::LoadLocal, semantic::PrimitiveType::Object,
                {}, expression.span);
            auto& loadWrapper = block(*currentBlockId_).instructions.back();
            loadWrapper.localIndex = assignment.variable.index;
            loadWrapper.resultTypeId = assignment.wrapperType.id;
        }

        // Step 2: Evaluate RHS (or compound read-modify-write expression)
        const auto assigned = lowerExpression(*assignment.expression);

        // Step 3: Load current root struct, drill down intermediate hops, update leaf, rebuild outwards, and write back
        ValueId rootCurrent = -1;
        if (effectiveRootKind == semantic::StructAssignmentRootKind::StaticField) {
            const auto defaultVal = emitValue(
                Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, expression.span);
            auto& defInst = block(*currentBlockId_).instructions.back();
            defInst.typeId = rootStructTypeId;
            defInst.resultTypeId = rootStructTypeId;
            defInst.symbolName = semantic::canonicalTypeName(rootStructType);

            const auto getName = "$intrinsic::static_get::" +
                std::to_string(assignment.staticReceiverField.id);
            Instruction getCall;
            getCall.result = nextValueId_++;
            getCall.resultType = semantic::PrimitiveType::Struct;
            getCall.resultTypeId = rootStructTypeId;
            getCall.opcode = Opcode::Call;
            getCall.operands = {defaultVal};
            getCall.parameterTypes = {semantic::PrimitiveType::Struct};
            getCall.parameterTypeIds = {rootStructTypeId};
            getCall.symbolId = semantic::stableTypeId(getName);
            getCall.symbolName = getName;
            getCall.sourceSpan = expression.span;
            rootCurrent = getCall.result;
            block(*currentBlockId_).instructions.push_back(std::move(getCall));
        } else if (effectiveRootKind == semantic::StructAssignmentRootKind::ObjectField) {
            rootCurrent = emitValue(
                Opcode::LoadField,
                semantic::PrimitiveType::Struct,
                {checkedObjRecv},
                expression.span);
            auto& loadField = block(*currentBlockId_).instructions.back();
            loadField.typeId = assignment.objectOwnerType.id;
            loadField.resultTypeId = rootStructTypeId;
            loadField.fieldIndex = assignment.objectField.index;
            loadField.symbolName = semantic::canonicalTypeName(assignment.objectOwnerType);
        } else if (effectiveRootKind == semantic::StructAssignmentRootKind::ArrayElement) {
            rootCurrent = emitValue(
                Opcode::LoadElement,
                semantic::PrimitiveType::Struct,
                {arrRecv, arrIdx},
                expression.span);
            auto& loadElem = block(*currentBlockId_).instructions.back();
            loadElem.elementType = semantic::PrimitiveType::Struct;
            loadElem.elementTypeId = rootStructTypeId;
            loadElem.resultTypeId = rootStructTypeId;
        } else if (assignment.wrappedVariable) {
            rootCurrent = emitValue(
                Opcode::LoadField, semantic::PrimitiveType::Struct,
                {wrapper}, expression.span);
            auto& loadValue = block(*currentBlockId_).instructions.back();
            loadValue.typeId = assignment.wrapperType.id;
            loadValue.resultTypeId = rootStructTypeId;
            loadValue.fieldIndex = assignment.wrapperValueField.index;
            loadValue.symbolName = semantic::canonicalTypeName(
                assignment.wrapperType);
        } else {
            rootCurrent = emitValue(
                Opcode::LoadLocal, semantic::PrimitiveType::Struct,
                {}, expression.span);
            auto& load = block(*currentBlockId_).instructions.back();
            load.localIndex = assignment.variable.index;
            load.resultTypeId = rootStructTypeId;
        }

        std::vector<ValueId> chainStructs;
        chainStructs.reserve(assignment.intermediateHops.size() + 1);
        chainStructs.push_back(rootCurrent);
        for (std::size_t m = 0; m < assignment.intermediateHops.size(); ++m) {
            const auto& hop = assignment.intermediateHops[m];
            const auto nextTypeId = (m + 1 < assignment.intermediateHops.size())
                ? assignment.intermediateHops[m + 1].ownerType.id
                : assignment.ownerType.id;
            const auto subStruct = emitValue(
                Opcode::LoadStructField,
                semantic::PrimitiveType::Struct,
                {chainStructs.back()},
                expression.span);
            auto& subLoad = block(*currentBlockId_).instructions.back();
            subLoad.typeId = hop.ownerType.id;
            subLoad.fieldIndex = hop.field.index;
            subLoad.resultTypeId = nextTypeId;
            subLoad.symbolName = semantic::canonicalTypeName(hop.ownerType);
            chainStructs.push_back(subStruct);
        }

        ValueId updated = emitValue(
            Opcode::StoreStructField,
            semantic::PrimitiveType::Struct,
            {chainStructs.back(), assigned},
            expression.span);
        {
            auto& leafStore = block(*currentBlockId_).instructions.back();
            leafStore.typeId = assignment.ownerType.id;
            leafStore.resultTypeId = assignment.ownerType.id;
            leafStore.fieldIndex = assignment.field.index;
            leafStore.symbolName = semantic::canonicalTypeName(assignment.ownerType);
        }

        for (std::size_t rev = assignment.intermediateHops.size(); rev > 0; --rev) {
            const std::size_t m = rev - 1u;
            const auto& hop = assignment.intermediateHops[m];
            updated = emitValue(
                Opcode::StoreStructField,
                semantic::PrimitiveType::Struct,
                {chainStructs[m], updated},
                expression.span);
            auto& hopStore = block(*currentBlockId_).instructions.back();
            hopStore.typeId = hop.ownerType.id;
            hopStore.resultTypeId = hop.ownerType.id;
            hopStore.fieldIndex = hop.field.index;
            hopStore.symbolName = semantic::canonicalTypeName(hop.ownerType);
        }

        if (effectiveRootKind == semantic::StructAssignmentRootKind::StaticField) {
            const auto setName = "$intrinsic::static_set::" +
                std::to_string(assignment.staticReceiverField.id);
            Instruction setCall;
            setCall.resultType = semantic::PrimitiveType::Void;
            setCall.opcode = Opcode::Call;
            setCall.operands = {updated};
            setCall.parameterTypes = {semantic::PrimitiveType::Struct};
            setCall.parameterTypeIds = {rootStructTypeId};
            setCall.symbolId = semantic::stableTypeId(setName);
            setCall.symbolName = setName;
            setCall.sourceSpan = expression.span;
            block(*currentBlockId_).instructions.push_back(std::move(setCall));
        } else if (effectiveRootKind == semantic::StructAssignmentRootKind::ObjectField) {
            Instruction storeObj;
            storeObj.resultType = semantic::PrimitiveType::Void;
            storeObj.opcode = Opcode::StoreField;
            storeObj.operands = {checkedObjRecv, updated};
            storeObj.typeId = assignment.objectOwnerType.id;
            storeObj.fieldIndex = assignment.objectField.index;
            storeObj.symbolName = semantic::canonicalTypeName(assignment.objectOwnerType);
            storeObj.sourceSpan = expression.span;
            block(*currentBlockId_).instructions.push_back(std::move(storeObj));
        } else if (effectiveRootKind == semantic::StructAssignmentRootKind::ArrayElement) {
            Instruction storeElem;
            storeElem.resultType = semantic::PrimitiveType::Void;
            storeElem.opcode = Opcode::StoreElement;
            storeElem.operands = {arrRecv, arrIdx, updated};
            storeElem.elementType = semantic::PrimitiveType::Struct;
            storeElem.elementTypeId = rootStructTypeId;
            storeElem.sourceSpan = expression.span;
            block(*currentBlockId_).instructions.push_back(std::move(storeElem));
        } else if (assignment.wrappedVariable) {
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
        if (assignment.staticConstructor &&
            (!currentFunction_ || currentFunction_->symbolId != assignment.staticConstructor->id)) {
            (void)emitCallInstruction(
                *assignment.staticConstructor,
                semantic::PrimitiveType::Void,
                {},
                {},
                expression.span,
                false,
                std::numeric_limits<std::uint32_t>::max(),
                false,
                0,
                std::numeric_limits<std::uint32_t>::max());
        }
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
            std::optional<semantic::FunctionSymbol> assignHandle;
            const semantic::BoundExpression* valueExpr = nullptr;
            const semantic::BoundReferenceCallArgument* refArg = nullptr;
            ValueId structRootWrapper = -1;
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
            ValueId structRootWrapper = -1;
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
                if (argument.targetField.staticField) {
                    if (argument.targetOwnerType.staticConstructor &&
                        (!currentFunction_ || currentFunction_->symbolId != argument.targetOwnerType.staticConstructor->id)) {
                        (void)emitCallInstruction(
                            *argument.targetOwnerType.staticConstructor,
                            semantic::PrimitiveType::Void,
                            {},
                            {},
                            expression.span,
                            false,
                            std::numeric_limits<std::uint32_t>::max(),
                            false,
                            0,
                            std::numeric_limits<std::uint32_t>::max());
                    }
                    if (argument.modifier == semantic::ParameterModifier::Ref &&
                        argument.value) {
                        initial = lowerExpression(*argument.value);
                    }
                    break;
                }
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
            case semantic::ReferenceTargetKind::StructField: {
                const auto& rootStructType = argument.intermediateHops.empty()
                    ? argument.targetOwnerType
                    : argument.intermediateHops.front().ownerType;
                const auto rootStructTypeId = rootStructType.id;

                if (argument.structRootKind == semantic::StructAssignmentRootKind::StaticField) {
                    if (argument.structRootOwnerType.staticConstructor &&
                        (!currentFunction_ || currentFunction_->symbolId != argument.structRootOwnerType.staticConstructor->id)) {
                        (void)emitCallInstruction(
                            *argument.structRootOwnerType.staticConstructor,
                            semantic::PrimitiveType::Void,
                            {},
                            {},
                            expression.span,
                            false,
                            std::numeric_limits<std::uint32_t>::max(),
                            false,
                            0,
                            std::numeric_limits<std::uint32_t>::max());
                    }
                    if (argument.structRootStaticInitializer) {
                        const auto hasName = "$intrinsic::static_has::" +
                            std::to_string(argument.structRootField.id);
                        Instruction hasCall;
                        hasCall.result = nextValueId_++;
                        hasCall.resultType = semantic::PrimitiveType::Bool;
                        hasCall.opcode = Opcode::Call;
                        hasCall.symbolId = semantic::stableTypeId(hasName);
                        hasCall.symbolName = hasName;
                        hasCall.sourceSpan = expression.span;
                        const auto hasVal = hasCall.result;
                        block(*currentBlockId_).instructions.push_back(std::move(hasCall));

                        const auto initBlock = createBlock();
                        const auto readyBlock = createBlock();
                        emitBranch(hasVal, readyBlock, initBlock, {}, {}, expression.span);

                        setCurrentBlock(initBlock);
                        const auto initVal = lowerExpression(*argument.structRootStaticInitializer);
                        const auto setName = "$intrinsic::static_set::" +
                            std::to_string(argument.structRootField.id);
                        Instruction setCall;
                        setCall.resultType = semantic::PrimitiveType::Void;
                        setCall.opcode = Opcode::Call;
                        setCall.operands = {initVal};
                        setCall.parameterTypes = {semantic::PrimitiveType::Struct};
                        setCall.parameterTypeIds = {rootStructTypeId};
                        setCall.symbolId = semantic::stableTypeId(setName);
                        setCall.symbolName = setName;
                        setCall.sourceSpan = expression.span;
                        block(*currentBlockId_).instructions.push_back(std::move(setCall));
                        emitJump(readyBlock, {}, expression.span);

                        setCurrentBlock(readyBlock);
                    }
                } else if (argument.structRootKind == semantic::StructAssignmentRootKind::ObjectField) {
                    const auto rawObj = lowerExpression(*argument.targetReceiver);
                    targetReceiver = emitValue(
                        Opcode::CheckNotNull,
                        semantic::PrimitiveType::Object,
                        {rawObj},
                        argument.targetReceiver->span);
                    auto& check = block(*currentBlockId_).instructions.back();
                    check.typeId = argument.structRootOwnerType.id;
                    check.resultTypeId = argument.structRootOwnerType.id;
                    check.symbolName = semantic::canonicalTypeName(argument.structRootOwnerType);
                } else if (argument.structRootKind == semantic::StructAssignmentRootKind::ArrayElement) {
                    targetReceiver = lowerExpression(*argument.targetReceiver);
                    targetIndex = lowerExpression(*argument.targetIndex);
                } else if (argument.wrappedStructRootVariable) {
                    structRootWrapper = emitValue(
                        Opcode::LoadLocal,
                        semantic::PrimitiveType::Object,
                        {},
                        expression.span);
                    auto& lw = block(*currentBlockId_).instructions.back();
                    lw.localIndex = argument.variable.index;
                    lw.resultTypeId = argument.structRootWrapperType.id;
                }

                if (argument.modifier == semantic::ParameterModifier::Ref) {
                    ValueId cur = -1;
                    if (argument.structRootKind == semantic::StructAssignmentRootKind::StaticField) {
                        const auto defaultVal = emitValue(
                            Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, expression.span);
                        auto& defInst = block(*currentBlockId_).instructions.back();
                        defInst.typeId = rootStructTypeId;
                        defInst.resultTypeId = rootStructTypeId;
                        defInst.symbolName = semantic::canonicalTypeName(rootStructType);
                        const auto getName = "$intrinsic::static_get::" +
                            std::to_string(argument.structRootField.id);
                        Instruction getCall;
                        getCall.result = nextValueId_++;
                        getCall.resultType = semantic::PrimitiveType::Struct;
                        getCall.resultTypeId = rootStructTypeId;
                        getCall.opcode = Opcode::Call;
                        getCall.operands = {defaultVal};
                        getCall.parameterTypes = {semantic::PrimitiveType::Struct};
                        getCall.parameterTypeIds = {rootStructTypeId};
                        getCall.symbolId = semantic::stableTypeId(getName);
                        getCall.symbolName = getName;
                        getCall.sourceSpan = expression.span;
                        cur = getCall.result;
                        block(*currentBlockId_).instructions.push_back(std::move(getCall));
                    } else if (argument.structRootKind == semantic::StructAssignmentRootKind::ObjectField) {
                        cur = emitValue(
                            Opcode::LoadField, semantic::PrimitiveType::Struct,
                            {targetReceiver}, expression.span);
                        auto& lf = block(*currentBlockId_).instructions.back();
                        lf.typeId = argument.structRootOwnerType.id;
                        lf.resultTypeId = rootStructTypeId;
                        lf.fieldIndex = argument.structRootField.index;
                        lf.symbolName = semantic::canonicalTypeName(argument.structRootOwnerType);
                    } else if (argument.structRootKind == semantic::StructAssignmentRootKind::ArrayElement) {
                        cur = emitValue(
                            Opcode::LoadElement, semantic::PrimitiveType::Struct,
                            {targetReceiver, targetIndex}, expression.span);
                        auto& le = block(*currentBlockId_).instructions.back();
                        le.elementType = semantic::PrimitiveType::Struct;
                        le.elementTypeId = rootStructTypeId;
                        le.resultTypeId = rootStructTypeId;
                    } else if (argument.wrappedStructRootVariable) {
                        cur = emitValue(
                            Opcode::LoadField, semantic::PrimitiveType::Struct,
                            {structRootWrapper}, expression.span);
                        auto& lf = block(*currentBlockId_).instructions.back();
                        lf.typeId = argument.structRootWrapperType.id;
                        lf.resultTypeId = rootStructTypeId;
                        lf.fieldIndex = argument.structRootWrapperValueField.index;
                        lf.symbolName = semantic::canonicalTypeName(argument.structRootWrapperType);
                    } else {
                        cur = emitValue(
                            Opcode::LoadLocal, semantic::PrimitiveType::Struct,
                            {}, expression.span);
                        auto& ll = block(*currentBlockId_).instructions.back();
                        ll.localIndex = argument.variable.index;
                        ll.resultTypeId = rootStructTypeId;
                    }
                    for (std::size_t m = 0; m < argument.intermediateHops.size(); ++m) {
                        const auto& hop = argument.intermediateHops[m];
                        const auto nextTid = (m + 1 < argument.intermediateHops.size())
                            ? argument.intermediateHops[m + 1].ownerType.id
                            : argument.targetOwnerType.id;
                        cur = emitValue(
                            Opcode::LoadStructField, semantic::PrimitiveType::Struct,
                            {cur}, expression.span);
                        auto& sl = block(*currentBlockId_).instructions.back();
                        sl.typeId = hop.ownerType.id;
                        sl.fieldIndex = hop.field.index;
                        sl.resultTypeId = nextTid;
                        sl.symbolName = semantic::canonicalTypeName(hop.ownerType);
                    }
                    initial = emitValue(
                        Opcode::LoadStructField, argument.targetField.type,
                        {cur}, expression.span);
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
            }
            case semantic::ReferenceTargetKind::None:
                if (argument.defensiveCopy && argument.value) {
                    initial = lowerExpression(*argument.value);
                }
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
            } else if (argument.modifier ==
                           semantic::ParameterModifier::Out &&
                       argument.valueField.type ==
                           semantic::PrimitiveType::Struct &&
                       !argument.valueField.typeName.empty()) {
                const auto defaultStruct = emitValue(
                    Opcode::NewStruct,
                    semantic::PrimitiveType::Struct,
                    {}, expression.span);
                auto& structInst =
                    block(*currentBlockId_).instructions.back();
                structInst.typeId = semantic::stableTypeId(
                    argument.valueField.typeName);
                structInst.resultTypeId = structInst.typeId;
                structInst.symbolName = argument.valueField.typeName;
                Instruction store;
                store.resultType = semantic::PrimitiveType::Void;
                store.opcode = Opcode::StoreField;
                store.operands = {wrapper, defaultStruct};
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
                    argument.targetElementTypeName,
                    argument.writebackAssignHandle,
                    argument.value.get(),
                    &argument,
                    structRootWrapper});
            }
        }

        if (call.staticConstructor &&
            (!currentFunction_ || currentFunction_->symbolId != call.staticConstructor->id)) {
            (void)emitCallInstruction(
                *call.staticConstructor,
                semantic::PrimitiveType::Void,
                {},
                {},
                expression.span,
                false,
                std::numeric_limits<std::uint32_t>::max(),
                false,
                0,
                std::numeric_limits<std::uint32_t>::max());
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
            auto value = emitValue(
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
            auto applyAssignHandle = [&](ValueId oldVal) -> ValueId {
                if (!writeback.assignHandle || oldVal < 0) {
                    return value;
                }
                return emitCallInstruction(
                    *writeback.assignHandle,
                    writeback.field.type,
                    writeback.field.typeName,
                    {oldVal, value},
                    expression.span,
                    false,
                    std::numeric_limits<std::uint32_t>::max(),
                    false,
                    0,
                    std::numeric_limits<std::uint32_t>::max());
            };
            switch (writeback.targetKind) {
            case semantic::ReferenceTargetKind::Variable: {
                const bool capturedCell =
                    (semantic::storageTypeOf(writeback.variable) !=
                         writeback.variable.type ||
                     semantic::storageTypeNameOf(writeback.variable) !=
                         writeback.variable.typeName) &&
                    writeback.variable.modifier ==
                        semantic::ParameterModifier::None;
                if (writeback.assignHandle) {
                    ValueId oldVal = -1;
                    if (capturedCell) {
                        const auto cell = emitValue(
                            Opcode::LoadLocal,
                            semantic::PrimitiveType::Object,
                            {}, expression.span);
                        auto& cellLoad =
                            block(*currentBlockId_).instructions.back();
                        cellLoad.localIndex = writeback.variable.index;
                        cellLoad.resultTypeId = semantic::stableTypeId(
                            semantic::storageTypeNameOf(writeback.variable));
                        oldVal = emitValue(
                            Opcode::LoadField,
                            writeback.field.type,
                            {cell}, expression.span);
                        auto& oldFieldLoad =
                            block(*currentBlockId_).instructions.back();
                        oldFieldLoad.typeId = cellLoad.resultTypeId;
                        oldFieldLoad.resultTypeId = semantic::isExactType(
                                writeback.field.type)
                            ? semantic::stableTypeId(writeback.field.typeName)
                            : 0;
                        oldFieldLoad.fieldIndex = 0;
                        oldFieldLoad.symbolName =
                            semantic::storageTypeNameOf(writeback.variable);
                    } else {
                        oldVal = emitValue(
                            Opcode::LoadLocal,
                            writeback.field.type,
                            {}, expression.span);
                        auto& oldLocalLoad =
                            block(*currentBlockId_).instructions.back();
                        oldLocalLoad.localIndex = writeback.variable.index;
                        oldLocalLoad.resultTypeId = semantic::isExactType(
                                writeback.field.type)
                            ? semantic::stableTypeId(writeback.field.typeName)
                            : 0;
                    }
                    value = applyAssignHandle(oldVal);
                }
                if (capturedCell) {
                    const auto cell = emitValue(
                        Opcode::LoadLocal,
                        semantic::PrimitiveType::Object,
                        {}, expression.span);
                    auto& cellLoad =
                        block(*currentBlockId_).instructions.back();
                    cellLoad.localIndex = writeback.variable.index;
                    cellLoad.resultTypeId = semantic::stableTypeId(
                        semantic::storageTypeNameOf(writeback.variable));
                    Instruction storeCell;
                    storeCell.resultType = semantic::PrimitiveType::Void;
                    storeCell.opcode = Opcode::StoreField;
                    storeCell.operands = {cell, value};
                    storeCell.typeId = cellLoad.resultTypeId;
                    storeCell.fieldIndex = 0;
                    storeCell.symbolName =
                        semantic::storageTypeNameOf(writeback.variable);
                    storeCell.sourceSpan = expression.span;
                    block(*currentBlockId_).instructions.push_back(
                        std::move(storeCell));
                } else {
                    emitStoreLocal(
                        writeback.variable.index, value, expression.span);
                }
                break;
            }
            case semantic::ReferenceTargetKind::ObjectField: {
                if (writeback.targetField.staticField) {
                    if (writeback.assignHandle && writeback.valueExpr) {
                        value = applyAssignHandle(
                            lowerExpression(*writeback.valueExpr));
                    }
                    const auto fieldTypeId = semantic::isExactType(
                            writeback.targetField.type)
                        ? semantic::stableTypeId(
                            writeback.targetField.typeName)
                        : 0;
                    const auto setName = "$intrinsic::static_set::" +
                        std::to_string(writeback.targetField.id);
                    Instruction setCall;
                    setCall.resultType = semantic::PrimitiveType::Void;
                    setCall.opcode = Opcode::Call;
                    setCall.operands = {value};
                    setCall.parameterTypes = {writeback.targetField.type};
                    setCall.parameterTypeIds = {fieldTypeId};
                    setCall.symbolId = semantic::stableTypeId(setName);
                    setCall.symbolName = setName;
                    setCall.sourceSpan = expression.span;
                    block(*currentBlockId_).instructions.push_back(
                        std::move(setCall));
                    break;
                }
                if (writeback.assignHandle) {
                    const auto oldVal = emitValue(
                        Opcode::LoadField,
                        writeback.targetField.type,
                        {writeback.targetReceiver}, expression.span);
                    auto& oldLoad = block(*currentBlockId_).instructions.back();
                    oldLoad.typeId = writeback.targetOwnerType.id;
                    oldLoad.resultTypeId = semantic::isExactType(
                            writeback.targetField.type)
                        ? semantic::stableTypeId(writeback.targetField.typeName)
                        : 0;
                    oldLoad.fieldIndex = writeback.targetField.index;
                    oldLoad.symbolName = semantic::canonicalTypeName(
                        writeback.targetOwnerType);
                    value = applyAssignHandle(oldVal);
                }
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
                if (writeback.assignHandle) {
                    const auto oldVal = emitValue(
                        Opcode::LoadElement,
                        writeback.targetElementType,
                        {writeback.targetReceiver, writeback.targetIndex},
                        expression.span);
                    auto& oldLoad = block(*currentBlockId_).instructions.back();
                    oldLoad.elementType = writeback.targetElementType;
                    oldLoad.elementTypeId = semantic::isExactType(
                            writeback.targetElementType)
                        ? semantic::stableTypeId(writeback.targetElementTypeName)
                        : 0;
                    oldLoad.resultTypeId = oldLoad.elementTypeId;
                    value = applyAssignHandle(oldVal);
                }
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
                const auto& refArg = *writeback.refArg;
                const auto& rootStructType = refArg.intermediateHops.empty()
                    ? writeback.targetOwnerType
                    : refArg.intermediateHops.front().ownerType;
                const auto rootStructTypeId = rootStructType.id;

                ValueId rootCur = -1;
                if (refArg.structRootKind == semantic::StructAssignmentRootKind::StaticField) {
                    const auto defaultVal = emitValue(
                        Opcode::NewStruct, semantic::PrimitiveType::Struct, {}, expression.span);
                    auto& defInst = block(*currentBlockId_).instructions.back();
                    defInst.typeId = rootStructTypeId;
                    defInst.resultTypeId = rootStructTypeId;
                    defInst.symbolName = semantic::canonicalTypeName(rootStructType);
                    const auto getName = "$intrinsic::static_get::" +
                        std::to_string(refArg.structRootField.id);
                    Instruction getCall;
                    getCall.result = nextValueId_++;
                    getCall.resultType = semantic::PrimitiveType::Struct;
                    getCall.resultTypeId = rootStructTypeId;
                    getCall.opcode = Opcode::Call;
                    getCall.operands = {defaultVal};
                    getCall.parameterTypes = {semantic::PrimitiveType::Struct};
                    getCall.parameterTypeIds = {rootStructTypeId};
                    getCall.symbolId = semantic::stableTypeId(getName);
                    getCall.symbolName = getName;
                    getCall.sourceSpan = expression.span;
                    rootCur = getCall.result;
                    block(*currentBlockId_).instructions.push_back(std::move(getCall));
                } else if (refArg.structRootKind == semantic::StructAssignmentRootKind::ObjectField) {
                    rootCur = emitValue(
                        Opcode::LoadField, semantic::PrimitiveType::Struct,
                        {writeback.targetReceiver}, expression.span);
                    auto& lf = block(*currentBlockId_).instructions.back();
                    lf.typeId = refArg.structRootOwnerType.id;
                    lf.resultTypeId = rootStructTypeId;
                    lf.fieldIndex = refArg.structRootField.index;
                    lf.symbolName = semantic::canonicalTypeName(refArg.structRootOwnerType);
                } else if (refArg.structRootKind == semantic::StructAssignmentRootKind::ArrayElement) {
                    rootCur = emitValue(
                        Opcode::LoadElement, semantic::PrimitiveType::Struct,
                        {writeback.targetReceiver, writeback.targetIndex}, expression.span);
                    auto& le = block(*currentBlockId_).instructions.back();
                    le.elementType = semantic::PrimitiveType::Struct;
                    le.elementTypeId = rootStructTypeId;
                    le.resultTypeId = rootStructTypeId;
                } else if (refArg.wrappedStructRootVariable) {
                    rootCur = emitValue(
                        Opcode::LoadField, semantic::PrimitiveType::Struct,
                        {writeback.structRootWrapper}, expression.span);
                    auto& lf = block(*currentBlockId_).instructions.back();
                    lf.typeId = refArg.structRootWrapperType.id;
                    lf.resultTypeId = rootStructTypeId;
                    lf.fieldIndex = refArg.structRootWrapperValueField.index;
                    lf.symbolName = semantic::canonicalTypeName(refArg.structRootWrapperType);
                } else {
                    rootCur = emitValue(
                        Opcode::LoadLocal, semantic::PrimitiveType::Struct,
                        {}, expression.span);
                    auto& ll = block(*currentBlockId_).instructions.back();
                    ll.localIndex = writeback.variable.index;
                    ll.resultTypeId = rootStructTypeId;
                }

                std::vector<ValueId> chainStructs;
                chainStructs.reserve(refArg.intermediateHops.size() + 1);
                chainStructs.push_back(rootCur);
                for (std::size_t m = 0; m < refArg.intermediateHops.size(); ++m) {
                    const auto& hop = refArg.intermediateHops[m];
                    const auto nextTid = (m + 1 < refArg.intermediateHops.size())
                        ? refArg.intermediateHops[m + 1].ownerType.id
                        : writeback.targetOwnerType.id;
                    const auto sub = emitValue(
                        Opcode::LoadStructField, semantic::PrimitiveType::Struct,
                        {chainStructs.back()}, expression.span);
                    auto& sl = block(*currentBlockId_).instructions.back();
                    sl.typeId = hop.ownerType.id;
                    sl.fieldIndex = hop.field.index;
                    sl.resultTypeId = nextTid;
                    sl.symbolName = semantic::canonicalTypeName(hop.ownerType);
                    chainStructs.push_back(sub);
                }

                if (writeback.assignHandle) {
                    const auto oldVal = emitValue(
                        Opcode::LoadStructField,
                        writeback.targetField.type,
                        {chainStructs.back()}, expression.span);
                    auto& oldLoad = block(*currentBlockId_).instructions.back();
                    oldLoad.typeId = writeback.targetOwnerType.id;
                    oldLoad.fieldIndex = writeback.targetField.index;
                    oldLoad.resultTypeId = semantic::isExactType(
                            writeback.targetField.type)
                        ? semantic::stableTypeId(writeback.targetField.typeName)
                        : 0;
                    oldLoad.symbolName = semantic::canonicalTypeName(
                        writeback.targetOwnerType);
                    value = applyAssignHandle(oldVal);
                }

                ValueId updated = emitValue(
                    Opcode::StoreStructField,
                    semantic::PrimitiveType::Struct,
                    {chainStructs.back(), value}, expression.span);
                {
                    auto& store = block(*currentBlockId_).instructions.back();
                    store.typeId = writeback.targetOwnerType.id;
                    store.resultTypeId = writeback.targetOwnerType.id;
                    store.fieldIndex = writeback.targetField.index;
                    store.symbolName = semantic::canonicalTypeName(
                        writeback.targetOwnerType);
                }
                for (std::size_t rev = refArg.intermediateHops.size(); rev > 0; --rev) {
                    const std::size_t m = rev - 1u;
                    const auto& hop = refArg.intermediateHops[m];
                    updated = emitValue(
                        Opcode::StoreStructField,
                        semantic::PrimitiveType::Struct,
                        {chainStructs[m], updated}, expression.span);
                    auto& hs = block(*currentBlockId_).instructions.back();
                    hs.typeId = hop.ownerType.id;
                    hs.resultTypeId = hop.ownerType.id;
                    hs.fieldIndex = hop.field.index;
                    hs.symbolName = semantic::canonicalTypeName(hop.ownerType);
                }

                if (refArg.structRootKind == semantic::StructAssignmentRootKind::StaticField) {
                    const auto setName = "$intrinsic::static_set::" +
                        std::to_string(refArg.structRootField.id);
                    Instruction setCall;
                    setCall.resultType = semantic::PrimitiveType::Void;
                    setCall.opcode = Opcode::Call;
                    setCall.operands = {updated};
                    setCall.parameterTypes = {semantic::PrimitiveType::Struct};
                    setCall.parameterTypeIds = {rootStructTypeId};
                    setCall.symbolId = semantic::stableTypeId(setName);
                    setCall.symbolName = setName;
                    setCall.sourceSpan = expression.span;
                    block(*currentBlockId_).instructions.push_back(std::move(setCall));
                } else if (refArg.structRootKind == semantic::StructAssignmentRootKind::ObjectField) {
                    Instruction storeObj;
                    storeObj.resultType = semantic::PrimitiveType::Void;
                    storeObj.opcode = Opcode::StoreField;
                    storeObj.operands = {writeback.targetReceiver, updated};
                    storeObj.typeId = refArg.structRootOwnerType.id;
                    storeObj.fieldIndex = refArg.structRootField.index;
                    storeObj.symbolName = semantic::canonicalTypeName(refArg.structRootOwnerType);
                    storeObj.sourceSpan = expression.span;
                    block(*currentBlockId_).instructions.push_back(std::move(storeObj));
                } else if (refArg.structRootKind == semantic::StructAssignmentRootKind::ArrayElement) {
                    Instruction storeElem;
                    storeElem.resultType = semantic::PrimitiveType::Void;
                    storeElem.opcode = Opcode::StoreElement;
                    storeElem.operands = {writeback.targetReceiver, writeback.targetIndex, updated};
                    storeElem.elementType = semantic::PrimitiveType::Struct;
                    storeElem.elementTypeId = rootStructTypeId;
                    storeElem.sourceSpan = expression.span;
                    block(*currentBlockId_).instructions.push_back(std::move(storeElem));
                } else if (refArg.wrappedStructRootVariable) {
                    Instruction wb;
                    wb.resultType = semantic::PrimitiveType::Void;
                    wb.opcode = Opcode::StoreField;
                    wb.operands = {writeback.structRootWrapper, updated};
                    wb.typeId = refArg.structRootWrapperType.id;
                    wb.fieldIndex = refArg.structRootWrapperValueField.index;
                    wb.symbolName = semantic::canonicalTypeName(refArg.structRootWrapperType);
                    wb.sourceSpan = expression.span;
                    block(*currentBlockId_).instructions.push_back(std::move(wb));
                } else {
                    emitStoreLocal(
                        writeback.variable.index, updated, expression.span);
                }
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
        if (call.staticConstructor &&
            (!currentFunction_ || currentFunction_->symbolId != call.staticConstructor->id)) {
            (void)emitCallInstruction(
                *call.staticConstructor,
                semantic::PrimitiveType::Void,
                {},
                {},
                expression.span,
                false,
                std::numeric_limits<std::uint32_t>::max(),
                false,
                0,
                std::numeric_limits<std::uint32_t>::max());
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
        if (switchExpression.valueVariable) {
            emitStoreLocal(
                switchExpression.valueVariable->index,
                input,
                switchExpression.expression->span);
        }
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
        bool checksExhausted = false;
        for (std::size_t index = 0;
             index < switchExpression.arms.size(); ++index) {
            const auto& arm = switchExpression.arms[index];
            if (arm.discard) continue;
            if (!arm.guard && arm.pattern && switchExpression.valueVariable &&
                (arm.pattern->kind == semantic::BoundPatternKind::Discard ||
                 arm.pattern->kind == semantic::BoundPatternKind::Var)) {
                if (arm.pattern->kind == semantic::BoundPatternKind::Var &&
                    arm.pattern->variable) {
                    const auto curVal = emitLoadPatternVar(
                        *switchExpression.valueVariable, arm.span);
                    emitStorePatternVar(*arm.pattern->variable, curVal, arm.span);
                }
                emitJump(armBlocks[index], {}, arm.span);
                checksExhausted = true;
                break;
            }
            const auto nextCheck = createBlock();
            if (arm.pattern && switchExpression.valueVariable) {
                if (arm.guard) {
                    const auto guardBlock = createBlock();
                    emitPatternBranch(
                        emitPatternBranch,
                        *arm.pattern,
                        *switchExpression.valueVariable,
                        guardBlock,
                        nextCheck);
                    setCurrentBlock(guardBlock);
                    emitBranch(
                        lowerExpression(*arm.guard),
                        armBlocks[index],
                        nextCheck,
                        {},
                        {},
                        arm.span);
                } else {
                    emitPatternBranch(
                        emitPatternBranch,
                        *arm.pattern,
                        *switchExpression.valueVariable,
                        armBlocks[index],
                        nextCheck);
                }
                setCurrentBlock(nextCheck);
                continue;
            }

            const auto armInput = switchExpression.valueVariable
                ? emitLoadPatternVar(*switchExpression.valueVariable, arm.span)
                : input;
            ValueId matched = -1;
            if (arm.patternType != semantic::PrimitiveType::Error) {
                matched = emitValue(
                    Opcode::IsType, semantic::PrimitiveType::Bool,
                    {armInput}, arm.span);
                auto& test = block(*currentBlockId_).instructions.back();
                test.elementType = arm.patternType;
                test.elementTypeId = arm.patternTypeId;
                test.parameterTypes = {switchExpression.expression->type};
                test.symbolName = arm.patternTypeName;
                if (arm.patternVariable) {
                    const auto cast = emitValue(
                        Opcode::AsType, arm.patternType, {armInput}, arm.span);
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
                    {armInput, label}, arm.span);
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
        if (!checksExhausted) {
            if (discardIndex) {
                const auto& discard = switchExpression.arms[*discardIndex];
                if (discard.guard) {
                    const auto fallbackBlock = createBlock();
                    emitBranch(
                        lowerExpression(*discard.guard), armBlocks[*discardIndex],
                        fallbackBlock, {}, {}, discard.span);
                    setCurrentBlock(fallbackBlock);
                    const auto defVal = emitDefaultValueForType(
                        expression.type, expression.typeName, expression.span);
                    emitJump(mergeBlock, {defVal}, expression.span);
                } else {
                    emitJump(armBlocks[*discardIndex], {}, expression.span);
                }
            } else {
                const auto defVal = emitDefaultValueForType(
                    expression.type, expression.typeName, expression.span);
                emitJump(mergeBlock, {defVal}, expression.span);
            }
        }
        for (std::size_t index = 0;
             index < switchExpression.arms.size(); ++index) {
            if (block(armBlocks[index]).terminator.kind != TerminatorKind::None) {
                continue;
            }
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
        const semantic::BoundPattern* activePat = typeTest.borrowedPattern
            ? typeTest.borrowedPattern
            : typeTest.pattern.get();
        if (!typeTest.safeCast && activePat && typeTest.patternInputVariable) {
            if (!typeTest.inputAlreadyInVariable && typeTest.expression) {
                const auto evaluated = lowerExpression(*typeTest.expression);
                emitStoreLocal(
                    typeTest.patternInputVariable->index,
                    evaluated,
                    typeTest.expression->span);
            }
            const auto trueBlock = createBlock();
            const auto falseBlock = createBlock();
            const auto mergeBlock = createBlock();
            const auto mergeValue = addBlockParameter(
                mergeBlock, semantic::PrimitiveType::Bool, 0);
            emitPatternBranch(
                emitPatternBranch,
                *activePat,
                *typeTest.patternInputVariable,
                trueBlock,
                falseBlock);
            setCurrentBlock(trueBlock);
            const auto trueVal = emitValue(
                Opcode::ConstantBool, semantic::PrimitiveType::Bool, {}, expression.span);
            block(*currentBlockId_).instructions.back().boolImmediate = true;
            emitJump(mergeBlock, {trueVal}, expression.span);

            setCurrentBlock(falseBlock);
            const auto falseVal = emitValue(
                Opcode::ConstantBool, semantic::PrimitiveType::Bool, {}, expression.span);
            block(*currentBlockId_).instructions.back().boolImmediate = false;
            emitJump(mergeBlock, {falseVal}, expression.span);

            setCurrentBlock(mergeBlock);
            return mergeValue;
        }
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
            emitStorePatternVar(
                *typeTest.patternVariable, cast, expression.span);
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
    if (currentStaticConstructorGuardId_.has_value()) {
        const auto trueVal = emitValue(
            Opcode::ConstantBool, semantic::PrimitiveType::Bool, {}, sourceSpan);
        block(*currentBlockId_).instructions.back().boolImmediate = true;
        const auto setName =
            "$intrinsic::static_set::" + std::to_string(*currentStaticConstructorGuardId_);
        Instruction setCall;
        setCall.resultType = semantic::PrimitiveType::Void;
        setCall.opcode = Opcode::Call;
        setCall.operands = {trueVal};
        setCall.parameterTypes = {semantic::PrimitiveType::Bool};
        setCall.parameterTypeIds = {0};
        setCall.symbolId = semantic::stableTypeId(setName);
        setCall.symbolName = setName;
        setCall.sourceSpan = sourceSpan;
        block(*currentBlockId_).instructions.push_back(std::move(setCall));
    }
    if (currentAsyncFunction_) {
        Instruction resolveInst;
        resolveInst.result = nextValueId_++;
        resolveInst.resultType = currentFunction_->returnType;
        resolveInst.resultTypeId = currentFunction_->returnTypeId;
        resolveInst.opcode = Opcode::AsyncResolve;
        if (value.has_value()) {
            resolveInst.operands = {*value};
        }
        resolveInst.typeId = currentFunction_->returnTypeId;
        resolveInst.symbolName = currentAsyncReturnTypeName_;
        resolveInst.sourceSpan = sourceSpan;
        const ValueId wrappedVal = resolveInst.result;
        block(*currentBlockId_).instructions.push_back(std::move(resolveInst));
        auto& terminator = block(*currentBlockId_).terminator;
        terminator.kind = TerminatorKind::ReturnValue;
        terminator.value = wrappedVal;
        terminator.sourceSpan = sourceSpan;
        return;
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
