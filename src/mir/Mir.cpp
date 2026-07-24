#include "realscript/mir/Mir.h"

#include <sstream>
#include <stdexcept>

namespace realscript::mir {

const char* opcodeName(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::Parameter: return "param";
    case Opcode::ConstantInt: return "const.i32";
    case Opcode::ConstantBool: return "const.bool";
    case Opcode::ConstantString: return "const.string";
    case Opcode::NegateInt: return "neg.i32";
    case Opcode::LogicalNot: return "not.bool";
    case Opcode::AddInt: return "add.i32";
    case Opcode::SubtractInt: return "sub.i32";
    case Opcode::MultiplyInt: return "mul.i32";
    case Opcode::DivideInt: return "div.checked.i32";
    case Opcode::RemainderInt: return "rem.checked.i32";
    case Opcode::Equal: return "eq";
    case Opcode::NotEqual: return "ne";
    case Opcode::LessInt: return "lt.i32";
    case Opcode::LessOrEqualInt: return "le.i32";
    case Opcode::GreaterInt: return "gt.i32";
    case Opcode::GreaterOrEqualInt: return "ge.i32";
    case Opcode::LogicalAnd: return "and.bool";
    case Opcode::LogicalOr: return "or.bool";
    case Opcode::ReturnValue: return "ret";
    case Opcode::ReturnVoid: return "ret.void";
    }
    return "unknown";
}

Module Lowerer::lower(const semantic::SemanticModel& model) {
    Module result;
    result.name = model.moduleName;
    for (const auto& function : model.functions) {
        result.functions.push_back(lowerFunction(function));
    }
    return result;
}

Function Lowerer::lowerFunction(const semantic::BoundFunction& function) {
    Function result;
    result.name = function.symbol.name;
    result.returnType = function.symbol.returnType;
    for (const auto& parameter : function.symbol.parameters) {
        result.parameterTypes.push_back(parameter.type);
    }
    result.blocks.push_back({0, {}});

    currentFunction_ = &result;
    currentBlock_ = &result.blocks.front();
    variableValues_.assign(function.variableCount, -1);
    nextValueId_ = 0;

    for (std::size_t i = 0; i < function.symbol.parameters.size(); ++i) {
        const auto& parameter = function.symbol.parameters[i];
        Instruction instruction;
        instruction.result = nextValueId_++;
        instruction.resultType = parameter.type;
        instruction.opcode = Opcode::Parameter;
        instruction.integerImmediate = static_cast<std::int64_t>(i);
        currentBlock_->instructions.push_back(instruction);
        variableValues_[parameter.index] = instruction.result;
    }

    lowerStatement(*function.body);

    const bool hasTerminator = !currentBlock_->instructions.empty() &&
        (currentBlock_->instructions.back().opcode == Opcode::ReturnValue ||
         currentBlock_->instructions.back().opcode == Opcode::ReturnVoid);
    if (!hasTerminator && function.symbol.returnType == semantic::PrimitiveType::Void) {
        emitTerminator(Opcode::ReturnVoid, {}, function.body->span);
    }

    currentFunction_ = nullptr;
    currentBlock_ = nullptr;
    return result;
}

void Lowerer::lowerStatement(const semantic::BoundStatement& statement) {
    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement: {
        const auto& block = static_cast<const semantic::BoundBlockStatement&>(statement);
        for (const auto& child : block.statements) {
            lowerStatement(*child);
        }
        return;
    }
    case semantic::BoundNodeKind::ReturnStatement: {
        const auto& returnStatement = static_cast<const semantic::BoundReturnStatement&>(statement);
        if (returnStatement.expression) {
            emitTerminator(
                Opcode::ReturnValue,
                {lowerExpression(*returnStatement.expression)},
                statement.span);
        } else {
            emitTerminator(Opcode::ReturnVoid, {}, statement.span);
        }
        return;
    }
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& declaration =
            static_cast<const semantic::BoundVariableDeclarationStatement&>(statement);
        const auto value = lowerExpression(*declaration.initializer);
        variableValues_.at(declaration.variable.index) = value;
        return;
    }
    case semantic::BoundNodeKind::ExpressionStatement: {
        const auto& expressionStatement =
            static_cast<const semantic::BoundExpressionStatement&>(statement);
        (void)lowerExpression(*expressionStatement.expression);
        return;
    }
    default:
        throw std::logic_error("unsupported bound statement in Phase 1A MIR lowerer");
    }
}

ValueId Lowerer::lowerExpression(const semantic::BoundExpression& expression) {
    switch (expression.kind()) {
    case semantic::BoundNodeKind::LiteralExpression: {
        const auto& literal = static_cast<const semantic::BoundLiteralExpression&>(expression);
        if (literal.type == semantic::PrimitiveType::Int) {
            const auto value = emitValue(Opcode::ConstantInt, literal.type, {}, expression.span);
            currentBlock_->instructions.back().integerImmediate = std::get<std::int64_t>(literal.value);
            return value;
        }
        if (literal.type == semantic::PrimitiveType::Bool) {
            const auto value = emitValue(Opcode::ConstantBool, literal.type, {}, expression.span);
            currentBlock_->instructions.back().boolImmediate = std::get<bool>(literal.value);
            return value;
        }
        if (literal.type == semantic::PrimitiveType::String) {
            const auto value = emitValue(Opcode::ConstantString, literal.type, {}, expression.span);
            currentBlock_->instructions.back().stringImmediate = std::get<std::string>(literal.value);
            return value;
        }
        throw std::logic_error("unsupported literal type in Phase 1A MIR lowerer");
    }
    case semantic::BoundNodeKind::VariableExpression: {
        const auto& variable = static_cast<const semantic::BoundVariableExpression&>(expression);
        const auto value = variableValues_.at(variable.variable.index);
        if (value < 0) {
            throw std::logic_error("variable reached MIR lowering without a value");
        }
        return value;
    }
    case semantic::BoundNodeKind::UnaryExpression: {
        const auto& unary = static_cast<const semantic::BoundUnaryExpression&>(expression);
        const auto operand = lowerExpression(*unary.operand);
        switch (unary.operatorKind) {
        case semantic::BoundUnaryOperatorKind::Identity:
            return operand;
        case semantic::BoundUnaryOperatorKind::Negation:
            return emitValue(Opcode::NegateInt, unary.type, {operand}, expression.span);
        case semantic::BoundUnaryOperatorKind::LogicalNegation:
            return emitValue(Opcode::LogicalNot, unary.type, {operand}, expression.span);
        }
        break;
    }
    case semantic::BoundNodeKind::BinaryExpression: {
        const auto& binary = static_cast<const semantic::BoundBinaryExpression&>(expression);
        const auto left = lowerExpression(*binary.left);
        const auto right = lowerExpression(*binary.right);
        Opcode opcode;
        switch (binary.operatorKind) {
        case semantic::BoundBinaryOperatorKind::Addition: opcode = Opcode::AddInt; break;
        case semantic::BoundBinaryOperatorKind::Subtraction: opcode = Opcode::SubtractInt; break;
        case semantic::BoundBinaryOperatorKind::Multiplication: opcode = Opcode::MultiplyInt; break;
        case semantic::BoundBinaryOperatorKind::Division: opcode = Opcode::DivideInt; break;
        case semantic::BoundBinaryOperatorKind::Remainder: opcode = Opcode::RemainderInt; break;
        case semantic::BoundBinaryOperatorKind::Equals: opcode = Opcode::Equal; break;
        case semantic::BoundBinaryOperatorKind::NotEquals: opcode = Opcode::NotEqual; break;
        case semantic::BoundBinaryOperatorKind::Less: opcode = Opcode::LessInt; break;
        case semantic::BoundBinaryOperatorKind::LessOrEquals: opcode = Opcode::LessOrEqualInt; break;
        case semantic::BoundBinaryOperatorKind::Greater: opcode = Opcode::GreaterInt; break;
        case semantic::BoundBinaryOperatorKind::GreaterOrEquals: opcode = Opcode::GreaterOrEqualInt; break;
        case semantic::BoundBinaryOperatorKind::LogicalAnd: opcode = Opcode::LogicalAnd; break;
        case semantic::BoundBinaryOperatorKind::LogicalOr: opcode = Opcode::LogicalOr; break;
        }
        return emitValue(opcode, binary.type, {left, right}, expression.span);
    }
    default:
        break;
    }
    throw std::logic_error("unsupported bound expression in Phase 1A MIR lowerer");
}

ValueId Lowerer::emitValue(
    Opcode opcode,
    semantic::PrimitiveType type,
    std::vector<ValueId> operands,
    text::TextSpan sourceSpan) {
    Instruction instruction;
    instruction.result = nextValueId_++;
    instruction.resultType = type;
    instruction.opcode = opcode;
    instruction.operands = std::move(operands);
    instruction.sourceSpan = sourceSpan;
    currentBlock_->instructions.push_back(std::move(instruction));
    return currentBlock_->instructions.back().result;
}

void Lowerer::emitTerminator(
    Opcode opcode,
    std::vector<ValueId> operands,
    text::TextSpan sourceSpan) {
    Instruction instruction;
    instruction.opcode = opcode;
    instruction.operands = std::move(operands);
    instruction.sourceSpan = sourceSpan;
    currentBlock_->instructions.push_back(std::move(instruction));
}

std::string printModule(const Module& module) {
    std::ostringstream out;
    if (!module.name.empty()) {
        out << "module " << module.name << "\n\n";
    }
    for (const auto& function : module.functions) {
        out << "func @" << function.name << '(';
        for (std::size_t i = 0; i < function.parameterTypes.size(); ++i) {
            if (i != 0) out << ", ";
            out << semantic::primitiveTypeName(function.parameterTypes[i]);
        }
        out << ") -> " << semantic::primitiveTypeName(function.returnType) << " {\n";

        for (const auto& block : function.blocks) {
            out << "bb" << block.id << ":\n";
            for (const auto& instruction : block.instructions) {
                out << "  ";
                if (instruction.result >= 0) {
                    out << '%' << instruction.result << ':'
                        << semantic::primitiveTypeName(instruction.resultType) << " = ";
                }
                out << opcodeName(instruction.opcode);
                if (instruction.opcode == Opcode::Parameter ||
                    instruction.opcode == Opcode::ConstantInt) {
                    out << ' ' << instruction.integerImmediate;
                } else if (instruction.opcode == Opcode::ConstantBool) {
                    out << ' ' << (instruction.boolImmediate ? "true" : "false");
                } else if (instruction.opcode == Opcode::ConstantString) {
                    out << " \"" << instruction.stringImmediate << "\"";
                } else {
                    for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
                        out << (i == 0 ? " " : ", ") << '%' << instruction.operands[i];
                    }
                }
                out << '\n';
            }
        }
        out << "}\n\n";
    }
    return out.str();
}

} // namespace realscript::mir
