#include "realscript/mir/Mir.h"

#include <stdexcept>
#include <utility>

namespace realscript::mir {

ValueId Lowerer::lowerExpression(const semantic::BoundExpression& expression) {
    switch (expression.kind()) {
    case semantic::BoundNodeKind::LiteralExpression: {
        const auto& literal =
            static_cast<const semantic::BoundLiteralExpression&>(expression);
        if (literal.type == semantic::PrimitiveType::Int) {
            const auto value = emitValue(
                Opcode::ConstantInt,
                literal.type,
                {},
                expression.span);
            block(*currentBlockId_).instructions.back().integerImmediate =
                std::get<std::int64_t>(literal.value);
            return value;
        }
        if (literal.type == semantic::PrimitiveType::Bool) {
            const auto value = emitValue(
                Opcode::ConstantBool,
                literal.type,
                {},
                expression.span);
            block(*currentBlockId_).instructions.back().boolImmediate =
                std::get<bool>(literal.value);
            return value;
        }
        if (literal.type == semantic::PrimitiveType::String) {
            const auto value = emitValue(
                Opcode::ConstantString,
                literal.type,
                {},
                expression.span);
            block(*currentBlockId_).instructions.back().stringImmediate =
                std::get<std::string>(literal.value);
            return value;
        }
        if (literal.type == semantic::PrimitiveType::Null) {
            return emitValue(
                Opcode::ConstantNull,
                literal.type,
                {},
                expression.span);
        }
        throw std::logic_error(
            "unsupported literal type in Phase 1C MIR lowerer");
    }
    case semantic::BoundNodeKind::VariableExpression: {
        const auto& variable =
            static_cast<const semantic::BoundVariableExpression&>(expression);
        const auto value = emitValue(
            Opcode::LoadLocal,
            variable.type,
            {},
            expression.span);
        block(*currentBlockId_).instructions.back().localIndex =
            variable.variable.index;
        return value;
    }
    case semantic::BoundNodeKind::AssignmentExpression: {
        const auto& assignment =
            static_cast<const semantic::BoundAssignmentExpression&>(expression);
        const auto value = lowerExpression(*assignment.expression);
        emitStoreLocal(
            assignment.variable.index,
            value,
            expression.span);
        return value;
    }
    case semantic::BoundNodeKind::ConversionExpression: {
        const auto& conversion =
            static_cast<const semantic::BoundConversionExpression&>(expression);
        const auto operand = lowerExpression(*conversion.expression);
        switch (conversion.conversion) {
        case semantic::ConversionKind::Identity:
            return operand;
        case semantic::ConversionKind::NullToString:
            return emitValue(
                Opcode::ConvertNullToString,
                conversion.type,
                {operand},
                expression.span);
        case semantic::ConversionKind::None:
            break;
        }
        throw std::logic_error("invalid bound conversion reached MIR lowering");
    }
    case semantic::BoundNodeKind::CallExpression: {
        const auto& call =
            static_cast<const semantic::BoundCallExpression&>(expression);
        std::vector<ValueId> arguments;
        arguments.reserve(call.arguments.size());
        for (const auto& argument : call.arguments) {
            arguments.push_back(lowerExpression(*argument));
        }

        Instruction instruction;
        instruction.resultType = call.type;
        instruction.opcode = Opcode::Call;
        instruction.operands = std::move(arguments);
        instruction.symbolId = call.function.id;
        instruction.symbolName =
            call.function.moduleName + "::" + call.function.name;
        for (const auto& parameter : call.function.parameters) {
            instruction.parameterTypes.push_back(parameter.type);
        }
        instruction.sourceSpan = expression.span;

        if (call.type != semantic::PrimitiveType::Void) {
            instruction.result = nextValueId_++;
        }
        block(*currentBlockId_).instructions.push_back(std::move(instruction));
        return block(*currentBlockId_).instructions.back().result;
    }
    case semantic::BoundNodeKind::UnaryExpression: {
        const auto& unary =
            static_cast<const semantic::BoundUnaryExpression&>(expression);
        const auto operand = lowerExpression(*unary.operand);
        switch (unary.operatorKind) {
        case semantic::BoundUnaryOperatorKind::Identity:
            return operand;
        case semantic::BoundUnaryOperatorKind::Negation:
            return emitValue(
                Opcode::NegateInt,
                unary.type,
                {operand},
                expression.span);
        case semantic::BoundUnaryOperatorKind::LogicalNegation:
            return emitValue(
                Opcode::LogicalNot,
                unary.type,
                {operand},
                expression.span);
        }
        break;
    }
    case semantic::BoundNodeKind::BinaryExpression: {
        const auto& binary =
            static_cast<const semantic::BoundBinaryExpression&>(expression);
        if (binary.operatorKind ==
                semantic::BoundBinaryOperatorKind::LogicalAnd ||
            binary.operatorKind ==
                semantic::BoundBinaryOperatorKind::LogicalOr) {
            return lowerShortCircuit(binary);
        }

        const auto left = lowerExpression(*binary.left);
        const auto right = lowerExpression(*binary.right);
        Opcode opcode = Opcode::AddInt;
        switch (binary.operatorKind) {
        case semantic::BoundBinaryOperatorKind::Addition:
            opcode = Opcode::AddInt;
            break;
        case semantic::BoundBinaryOperatorKind::Subtraction:
            opcode = Opcode::SubtractInt;
            break;
        case semantic::BoundBinaryOperatorKind::Multiplication:
            opcode = Opcode::MultiplyInt;
            break;
        case semantic::BoundBinaryOperatorKind::Division:
            opcode = Opcode::DivideInt;
            break;
        case semantic::BoundBinaryOperatorKind::Remainder:
            opcode = Opcode::RemainderInt;
            break;
        case semantic::BoundBinaryOperatorKind::Equals:
            opcode = Opcode::Equal;
            break;
        case semantic::BoundBinaryOperatorKind::NotEquals:
            opcode = Opcode::NotEqual;
            break;
        case semantic::BoundBinaryOperatorKind::Less:
            opcode = Opcode::LessInt;
            break;
        case semantic::BoundBinaryOperatorKind::LessOrEquals:
            opcode = Opcode::LessOrEqualInt;
            break;
        case semantic::BoundBinaryOperatorKind::Greater:
            opcode = Opcode::GreaterInt;
            break;
        case semantic::BoundBinaryOperatorKind::GreaterOrEquals:
            opcode = Opcode::GreaterOrEqualInt;
            break;
        case semantic::BoundBinaryOperatorKind::LogicalAnd:
        case semantic::BoundBinaryOperatorKind::LogicalOr:
            throw std::logic_error(
                "short-circuit operator reached eager MIR lowering");
        default:
            throw std::logic_error(
                "unsupported binary operator reached MIR lowering");
        }
        return emitValue(
            opcode,
            binary.type,
            {left, right},
            expression.span);
    }
    default:
        break;
    }

    throw std::logic_error(
        "unsupported bound expression in Phase 1C MIR lowerer");
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
    semantic::PrimitiveType type) {
    const auto value = nextValueId_++;
    block(blockId).parameters.push_back({value, type});
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
        throw std::logic_error(
            "cannot emit a value without an open MIR block");
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
