#include "realscript/mir/Mir.h"

#include <iomanip>
#include <sstream>

namespace realscript::mir {
namespace {

void printSignatureType(
    std::ostringstream& out,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) {
    out << semantic::primitiveTypeName(type);
    if (type == semantic::PrimitiveType::Object && typeId != 0) {
        out << "[0x" << std::hex << typeId << std::dec << ']';
    }
}

void printArguments(
    std::ostringstream& out,
    const std::vector<ValueId>& arguments) {
    if (arguments.empty()) {
        return;
    }
    out << '(';
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << '%' << arguments[i];
    }
    out << ')';
}

} // namespace

std::string printModule(const Module& module) {
    std::ostringstream out;
    if (!module.name.empty()) {
        out << "module " << module.name << "\n\n";
    }

    for (const auto& function : module.functions) {
        out << "func @" << function.name << "(";
        for (std::size_t i = 0; i < function.parameterTypes.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            printSignatureType(
                out,
                function.parameterTypes[i],
                i < function.parameterTypeIds.size()
                    ? function.parameterTypeIds[i]
                    : 0);
        }
        out << ") -> " << semantic::primitiveTypeName(function.returnType)
            << " {\n";

        out << "  locals [";
        for (std::size_t i = 0; i < function.localTypes.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << i << ':' << semantic::primitiveTypeName(function.localTypes[i]);
        }
        out << "]\n";

        for (const auto& basicBlock : function.blocks) {
            out << "bb" << basicBlock.id;
            if (!basicBlock.parameters.empty()) {
                out << '(';
                for (std::size_t i = 0; i < basicBlock.parameters.size(); ++i) {
                    if (i != 0) {
                        out << ", ";
                    }
                    out << '%' << basicBlock.parameters[i].value << ':'
                        << semantic::primitiveTypeName(
                            basicBlock.parameters[i].type);
                }
                out << ')';
            }
            out << ":\n";

            for (const auto& instruction : basicBlock.instructions) {
                out << "  ";
                if (instruction.result >= 0) {
                    out << '%' << instruction.result << ':'
                        << semantic::primitiveTypeName(instruction.resultType)
                        << " = ";
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
                    out << ' ' << instruction.localIndex << ", %"
                        << instruction.operands.front();
                } else if (instruction.opcode == Opcode::NewObject) {
                    out << " @" << instruction.symbolName << "[0x"
                        << std::hex << instruction.typeId << std::dec << "]";
                } else if (instruction.opcode == Opcode::LoadField) {
                    out << " @" << instruction.symbolName << '.'
                        << instruction.fieldIndex << ", %"
                        << instruction.operands.front();
                } else if (instruction.opcode == Opcode::StoreField) {
                    out << " @" << instruction.symbolName << '.'
                        << instruction.fieldIndex << ", %"
                        << instruction.operands[0] << ", %"
                        << instruction.operands[1];
                } else if (instruction.opcode == Opcode::CheckNotNull) {
                    out << " @" << instruction.symbolName << ", %"
                        << instruction.operands.front();
                } else if (instruction.opcode == Opcode::Call) {
                    out << " @" << instruction.symbolName << "[0x"
                        << std::hex << instruction.symbolId << std::dec << "](";
                    for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
                        if (i != 0) {
                            out << ", ";
                        }
                        out << '%' << instruction.operands[i];
                    }
                    out << ')';
                } else {
                    for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
                        out << (i == 0 ? " %" : ", %")
                            << instruction.operands[i];
                    }
                }
                out << '\n';
            }

            const auto& terminator = basicBlock.terminator;
            out << "  " << terminatorName(terminator.kind);
            if (terminator.kind == TerminatorKind::Jump) {
                out << " bb" << terminator.target;
                printArguments(out, terminator.arguments);
            } else if (terminator.kind == TerminatorKind::Branch) {
                out << " %" << terminator.condition << ", bb"
                    << terminator.target;
                printArguments(out, terminator.arguments);
                out << ", bb" << terminator.falseTarget;
                printArguments(out, terminator.falseArguments);
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
