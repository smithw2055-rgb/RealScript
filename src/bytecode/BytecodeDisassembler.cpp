#include "realscript/bytecode/Bytecode.h"

#include <iomanip>
#include <sstream>

namespace realscript::bytecode {
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

void printRegisters(
    std::ostringstream& out,
    const std::vector<Register>& registers) {
    if (registers.empty()) {
        return;
    }
    out << '(';
    for (std::size_t index = 0; index < registers.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << 'r' << registers[index];
    }
    out << ')';
}

} // namespace

std::string disassembleModule(const Module& module) {
    std::ostringstream out;
    out << "rsbc " << module.version.major << '.' << module.version.minor
        << " module " << module.name << "\n";

    if (!module.types.empty()) {
        out << "\ntypes:\n";
        for (std::size_t index = 0; index < module.types.size(); ++index) {
            const auto& type = module.types[index];
            out << "  type" << index << " @"
                << semantic::canonicalTypeName(type) << "[0x"
                << std::hex << type.id << std::dec << "] {";
            for (std::size_t fieldIndex = 0; fieldIndex < type.fields.size(); ++fieldIndex) {
                if (fieldIndex != 0) out << ", ";
                const auto& field = type.fields[fieldIndex];
                out << field.name << ':'
                    << (field.type == semantic::PrimitiveType::Object
                        ? field.typeName
                        : semantic::primitiveTypeName(field.type));
            }
            out << "}\n";
        }
    }

    if (!module.functionReferences.empty()) {
        out << "\nreferences:\n";
        for (std::size_t index = 0; index < module.functionReferences.size(); ++index) {
            const auto& reference = module.functionReferences[index];
            out << "  ref" << index << " @" << reference.name << "[0x"
                << std::hex << reference.symbolId << std::dec << "](";
            for (std::size_t parameter = 0;
                 parameter < reference.parameterTypes.size();
                 ++parameter) {
                if (parameter != 0) {
                    out << ", ";
                }
                printSignatureType(
                    out,
                    reference.parameterTypes[parameter],
                    parameter < reference.parameterTypeIds.size()
                        ? reference.parameterTypeIds[parameter]
                        : 0);
            }
            out << ") -> " << semantic::primitiveTypeName(reference.returnType)
                << '\n';
        }
    }

    for (const auto& function : module.functions) {
        out << "\nfunc @" << function.name << "[0x" << std::hex
            << function.symbolId << std::dec << "](";
        for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
            if (index != 0) {
                out << ", ";
            }
            printSignatureType(
                out,
                function.parameterTypes[index],
                index < function.parameterTypeIds.size()
                    ? function.parameterTypeIds[index]
                    : 0);
        }
        out << ") -> " << semantic::primitiveTypeName(function.returnType)
            << " {\n";

        out << "  registers [";
        for (std::size_t index = 0; index < function.registerTypes.size(); ++index) {
            if (index != 0) {
                out << ", ";
            }
            out << 'r' << index << ':'
                << semantic::primitiveTypeName(function.registerTypes[index]);
        }
        out << "]\n  locals [";
        for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
            if (index != 0) {
                out << ", ";
            }
            out << index << ':'
                << semantic::primitiveTypeName(function.localTypes[index]);
        }
        out << "]\n";

        for (const auto& block : function.blocks) {
            out << "bb" << block.id;
            if (!block.parameters.empty()) {
                out << '(';
                for (std::size_t index = 0; index < block.parameters.size(); ++index) {
                    if (index != 0) {
                        out << ", ";
                    }
                    out << 'r' << block.parameters[index].target << ':'
                        << semantic::primitiveTypeName(block.parameters[index].type);
                }
                out << ')';
            }
            out << ":\n";

            for (const auto& instruction : block.instructions) {
                out << "  ";
                if (instruction.result != InvalidRegister) {
                    out << 'r' << instruction.result << ':'
                        << semantic::primitiveTypeName(
                            function.registerTypes[instruction.result])
                        << " = ";
                }
                out << opcodeName(instruction.opcode);
                switch (instruction.opcode) {
                case Opcode::LoadParameter:
                case Opcode::LoadLocal:
                    out << ' ' << instruction.index;
                    break;
                case Opcode::StoreLocal:
                    out << ' ' << instruction.index << ", r"
                        << instruction.operands.front();
                    break;
                case Opcode::ConstantInt:
                    out << ' ' << instruction.integerImmediate;
                    break;
                case Opcode::ConstantBool:
                    out << ' ' << (instruction.boolImmediate ? "true" : "false");
                    break;
                case Opcode::ConstantString:
                    out << " \"" << instruction.stringImmediate << "\"";
                    break;
                case Opcode::ConstantNull:
                    break;
                case Opcode::NewObject:
                    out << " type" << instruction.typeIndex;
                    break;
                case Opcode::CheckNotNull:
                    out << " type" << instruction.typeIndex << ", r"
                        << instruction.operands.front();
                    break;
                case Opcode::LoadField:
                    out << " type" << instruction.typeIndex << '.'
                        << instruction.index << ", r"
                        << instruction.operands.front();
                    break;
                case Opcode::StoreField:
                    out << " type" << instruction.typeIndex << '.'
                        << instruction.index << ", r"
                        << instruction.operands[0] << ", r"
                        << instruction.operands[1];
                    break;
                case Opcode::Call:
                    if (instruction.index < module.functionReferences.size()) {
                        const auto& reference =
                            module.functionReferences[instruction.index];
                        out << " ref" << instruction.index << " @"
                            << reference.name;
                    } else {
                        out << " ref" << instruction.index;
                    }
                    printRegisters(out, instruction.operands);
                    break;
                default:
                    for (std::size_t index = 0;
                         index < instruction.operands.size();
                         ++index) {
                        out << (index == 0 ? " r" : ", r")
                            << instruction.operands[index];
                    }
                    break;
                }
                out << '\n';
            }

            out << "  " << terminatorName(block.terminator.kind);
            switch (block.terminator.kind) {
            case TerminatorKind::Jump:
                out << " bb" << block.terminator.target;
                printRegisters(out, block.terminator.arguments);
                break;
            case TerminatorKind::Branch:
                out << " r" << block.terminator.condition << ", bb"
                    << block.terminator.target;
                printRegisters(out, block.terminator.arguments);
                out << ", bb" << block.terminator.falseTarget;
                printRegisters(out, block.terminator.falseArguments);
                break;
            case TerminatorKind::ReturnValue:
                out << " r" << block.terminator.value;
                break;
            case TerminatorKind::None:
            case TerminatorKind::ReturnVoid:
                break;
            }
            out << '\n';
        }
        out << "}\n";
    }
    return out.str();
}

std::string bytesToHex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) {
            if (index % 16 == 0) {
                out << '\n';
            } else {
                out << ' ';
            }
        }
        out << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    if (!bytes.empty()) {
        out << '\n';
    }
    return out.str();
}

} // namespace realscript::bytecode
