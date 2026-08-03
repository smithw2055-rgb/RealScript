#include "realscript/bytecode/Bytecode.h"

namespace realscript::bytecode {

const char* opcodeName(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::LoadParameter: return "param";
    case Opcode::ConstantInt: return "const.int";
    case Opcode::ConstantDouble: return "const.f64";
    case Opcode::ConstantBool: return "const.bool";
    case Opcode::ConstantString: return "const.string";
    case Opcode::ConstantNull: return "const.null";
    case Opcode::LoadLocal: return "load.local";
    case Opcode::StoreLocal: return "store.local";
    case Opcode::ConvertNullToString: return "conv.null.string";
    case Opcode::ConvertNullToObject: return "conv.null.object";
    case Opcode::ConvertNullToArray: return "conv.null.array";
    case Opcode::ConvertIntToLong: return "conv.i32.i64";
    case Opcode::ConvertIntToDouble: return "conv.i32.f64";
    case Opcode::ConvertLongToDouble: return "conv.i64.f64";
    case Opcode::ConvertNumeric: return "conv.numeric";
    case Opcode::IsType: return "is.type";
    case Opcode::AsType: return "as.type";
    case Opcode::ConstantTypeId: return "const.typeid";
    case Opcode::NewObject: return "new.object";
    case Opcode::NewStruct: return "new.struct";
    case Opcode::NewArray: return "new.array";
    case Opcode::CheckNotNull: return "check.notnull";
    case Opcode::ArrayLength: return "array.length";
    case Opcode::LoadElement: return "load.element";
    case Opcode::StoreElement: return "store.element";
    case Opcode::LoadField: return "load.field";
    case Opcode::StoreField: return "store.field";
    case Opcode::LoadStructField: return "load.struct.field";
    case Opcode::StoreStructField: return "store.struct.field";
    case Opcode::Call: return "call";
    case Opcode::NegateInt: return "neg.i32";
    case Opcode::NegateLong: return "neg.i64";
    case Opcode::NegateDouble: return "neg.f64";
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
    case Opcode::AddLong: return "add.i64";
    case Opcode::SubtractLong: return "sub.i64";
    case Opcode::MultiplyLong: return "mul.i64";
    case Opcode::DivideLong: return "div.checked.i64";
    case Opcode::RemainderLong: return "rem.checked.i64";
    case Opcode::LessLong: return "lt.i64";
    case Opcode::LessOrEqualLong: return "le.i64";
    case Opcode::GreaterLong: return "gt.i64";
    case Opcode::GreaterOrEqualLong: return "ge.i64";
    case Opcode::AddDouble: return "add.f64";
    case Opcode::SubtractDouble: return "sub.f64";
    case Opcode::MultiplyDouble: return "mul.f64";
    case Opcode::DivideDouble: return "div.f64";
    case Opcode::LessDouble: return "lt.f64";
    case Opcode::LessOrEqualDouble: return "le.f64";
    case Opcode::GreaterDouble: return "gt.f64";
    case Opcode::GreaterOrEqualDouble: return "ge.f64";
    case Opcode::NewDelegate: return "new.delegate";
    case Opcode::InvokeDelegate: return "invoke.delegate";
    case Opcode::CombineDelegate: return "combine.delegate";
    case Opcode::RemoveDelegate: return "remove.delegate";
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
    case TerminatorKind::Throw: return "throw";
    }
    return "unknown";
}

} // namespace realscript::bytecode
