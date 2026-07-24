#include "realscript/mir/Mir.h"

#include <sstream>

namespace realscript::mir {

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
        out << "  locals [";
        for (std::size_t i = 0; i < function.localTypes.size(); ++i) {
            if (i != 0) out << ", ";
            out << i << ':' << semantic::primitiveTypeName(function.localTypes[i]);
        }
        out << "]\n";

        for (const auto& basicBlock : function.blocks) {
            out << "bb" << basicBlock.id;
            if (!basicBlock.parameters.empty()) {
                out << '(';
                for (std::size_t i = 0; i < basicBlock.parameters.size(); ++i) {
                    if (i != 0) out << ", ";
                    out << '%' << basicBlock.parameters[i].value << ':'
                        << semantic::primitiveTypeName(basicBlock.parameters[i].type);
                }
                out << ')';
            }
            out << ":\n";

            for (const auto& instruction : basicBlock.instructions) {
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
                } else if (instruction.opcode == Opcode::LoadLocal) {
                    out << ' ' << instruction.localIndex;
                } else if (instruction.opcode == Opcode::StoreLocal) {
                    out << ' ' << instruction.localIndex << ", %" << instruction.operands.front();
                } else {
                    for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
                        out << (i == 0 ? " " : ", ") << '%' << instruction.operands[i];
                    }
                }
                out << '\n';
            }

            const auto& terminator = basicBlock.terminator;
            out << "  " << terminatorName(terminator.kind);
            if (terminator.kind == TerminatorKind::Jump) {
                out << " bb" << terminator.target;
                if (!terminator.arguments.empty()) {
                    out << '(';
                    for (std::size_t i = 0; i < terminator.arguments.size(); ++i) {
                        if (i != 0) out << ", ";
                        out << '%' << terminator.arguments[i];
                    }
                    out << ')';
                }
            } else if (terminator.kind == TerminatorKind::Branch) {
                out << " %" << terminator.condition << ", bb" << terminator.target;
                if (!terminator.arguments.empty()) {
                    out << '(';
                    for (std::size_t i = 0; i < terminator.arguments.size(); ++i) {
                        if (i != 0) out << ", ";
                        out << '%' << terminator.arguments[i];
                    }
                    out << ')';
                }
                out << ", bb" << terminator.falseTarget;
                if (!terminator.falseArguments.empty()) {
                    out << '(';
                    for (std::size_t i = 0; i < terminator.falseArguments.size(); ++i) {
                        if (i != 0) out << ", ";
                        out << '%' << terminator.falseArguments[i];
                    }
                    out << ')';
                }
            } else if (terminator.kind == TerminatorKind::ReturnValue) {
                out << " %" << terminator.value;
            }
            out << '\n';
        }
        out << "}\n\n";
    }
    return out.str();
}


} // namespace realscript::mir
