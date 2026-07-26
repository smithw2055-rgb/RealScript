#include "realscript/bytecode/Bytecode.h"

namespace realscript::bytecode {

const char* opcodeName(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::LoadParameter: return "param";
    case Opcode::ConstantInt: return "const.i32";
    case Opcode::ConstantBool: return "const.bool";
    case Opcode::ConstantString: return "const.string";
    case Opcode::ConstantNull: return "const.null";
    case Opcode::LoadLocal: return "load.local";
    case Opcode::StoreLocal: return "store.local";
    case Opcode::ConvertNullToString: return "conv.null.string";
    case Opcode::ConvertNullToObject: return "conv.null.object";
    case Opcode::ConvertNullToArray: return "conv.null.array";
    case Opcode::NewObject: return "new.object";
    case Opcode::NewArray: return "new.array";
    case Opcode::CheckNotNull: return "check.notnull";
    case Opcode::ArrayLength: return "array.length";
    case Opcode::LoadElement: return "load.element";
    case Opcode::StoreElement: return "store.element";
    case Opcode::LoadField: return "load.field";
    case Opcode::StoreField: return "store.field";
    case Opcode::Call: return "call";
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
    }
    return "unknown";
}

const char* terminatorName(TerminatorKind kind) noexcept {
    switch (kind) {
    case TerminatorKind::None: return "none";
    case TerminatorKind::Jump: return "jmp";
    case TerminatorKind::Branch: return "br";
    case TerminatorKind::ReturnValue: return "ret";
    case TerminatorKind::ReturnVoid: return "ret.void";
    }
    return "unknown";
}

} // namespace realscript::bytecode
