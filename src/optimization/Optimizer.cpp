#include "realscript/optimization/Optimizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <iterator>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <variant>

namespace realscript::optimization {
namespace {

struct NullConstant {
    friend constexpr bool operator==(NullConstant, NullConstant) noexcept { return true; }
};
using ConstantData = std::variant<NullConstant, bool, std::int64_t, double, std::string>;

struct Constant {
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
    ConstantData data;
};

bool isValueInstruction(const mir::Instruction& instruction) noexcept {
    if (instruction.opcode == mir::Opcode::StoreLocal ||
        instruction.opcode == mir::Opcode::StoreField ||
        instruction.opcode == mir::Opcode::StoreElement ||
        instruction.opcode == mir::Opcode::AsyncEnter ||
        instruction.opcode == mir::Opcode::AwaitSuspend) {
        return false;
    }
    return (instruction.opcode != mir::Opcode::Call &&
            instruction.opcode != mir::Opcode::InvokeDelegate &&
            instruction.opcode != mir::Opcode::AwaitResume) ||
        instruction.resultType != semantic::PrimitiveType::Void;
}

bool isRemovableWhenUnused(const mir::Instruction& instruction) noexcept {
    switch (instruction.opcode) {
    case mir::Opcode::Parameter:
    case mir::Opcode::ConstantInt:
    case mir::Opcode::ConstantDouble:
    case mir::Opcode::ConstantBool:
    case mir::Opcode::ConstantString:
    case mir::Opcode::ConstantNull:
    case mir::Opcode::LoadLocal:
    case mir::Opcode::ConvertNullToString:
    case mir::Opcode::ConvertNullToObject:
    case mir::Opcode::ConvertNullToArray:
    case mir::Opcode::ConvertIntToLong:
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
    case mir::Opcode::LogicalNot:
    case mir::Opcode::Equal:
    case mir::Opcode::NotEqual:
    case mir::Opcode::LessInt:
    case mir::Opcode::LessOrEqualInt:
    case mir::Opcode::GreaterInt:
    case mir::Opcode::GreaterOrEqualInt:
    case mir::Opcode::LessLong:
    case mir::Opcode::LessOrEqualLong:
    case mir::Opcode::GreaterLong:
    case mir::Opcode::GreaterOrEqualLong:
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble:
    case mir::Opcode::NegateDouble:
    case mir::Opcode::LessDouble:
    case mir::Opcode::LessOrEqualDouble:
    case mir::Opcode::GreaterDouble:
    case mir::Opcode::GreaterOrEqualDouble:
    case mir::Opcode::NewStruct:
    case mir::Opcode::LoadStructField:
    case mir::Opcode::StoreStructField:
        return true;
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt:
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong:
    case mir::Opcode::NegateInt:
    case mir::Opcode::NegateLong:
    case mir::Opcode::ConvertNumeric:
        return !instruction.checkedArithmetic;
    default:
        return false;
    }
}

std::optional<Constant> constantFromInstruction(
    const mir::Instruction& instruction) {
    Constant result;
    result.type = instruction.resultType;
    result.typeId = instruction.resultTypeId;
    switch (instruction.opcode) {
    case mir::Opcode::ConstantInt:
        result.data = instruction.integerImmediate;
        return result;
    case mir::Opcode::ConstantDouble:
        result.data = instruction.doubleImmediate;
        return result;
    case mir::Opcode::ConstantBool:
        result.data = instruction.boolImmediate;
        return result;
    case mir::Opcode::ConstantString:
        result.data = instruction.stringImmediate;
        return result;
    case mir::Opcode::ConstantNull:
        result.data = NullConstant{};
        return result;
    default:
        return std::nullopt;
    }
}

mir::Instruction makeConstant(
    const mir::Instruction& original,
    Constant constant) {
    mir::Instruction result;
    result.result = original.result;
    result.resultType = original.resultType;
    result.resultTypeId = original.resultTypeId;
    result.sourceSpan = original.sourceSpan;
    if (std::holds_alternative<NullConstant>(constant.data)) {
        result.opcode = mir::Opcode::ConstantNull;
    } else if (const auto* value = std::get_if<bool>(&constant.data)) {
        result.opcode = mir::Opcode::ConstantBool;
        result.boolImmediate = *value;
    } else if (const auto* value = std::get_if<std::int64_t>(&constant.data)) {
        result.opcode = mir::Opcode::ConstantInt;
        result.integerImmediate = *value;
    } else if (const auto* value = std::get_if<double>(&constant.data)) {
        result.opcode = mir::Opcode::ConstantDouble;
        result.doubleImmediate = *value;
    } else {
        result.opcode = mir::Opcode::ConstantString;
        result.stringImmediate = std::get<std::string>(std::move(constant.data));
    }
    return result;
}

template <typename T, typename Operation>
std::optional<T> checkedBinary(T left, T right, Operation operation) {
#if defined(__GNUC__) || defined(__clang__)
    (void)operation;
    T result{};
    if constexpr (std::is_same_v<Operation, std::plus<T>>) {
        if (__builtin_add_overflow(left, right, &result)) return std::nullopt;
    } else if constexpr (std::is_same_v<Operation, std::minus<T>>) {
        if (__builtin_sub_overflow(left, right, &result)) return std::nullopt;
    } else {
        if (__builtin_mul_overflow(left, right, &result)) return std::nullopt;
    }
    return result;
#else
    if constexpr (std::is_same_v<Operation, std::plus<T>>) {
        if ((right > 0 && left > std::numeric_limits<T>::max() - right) ||
            (right < 0 && left < std::numeric_limits<T>::min() - right)) {
            return std::nullopt;
        }
    } else if constexpr (std::is_same_v<Operation, std::minus<T>>) {
        if ((right < 0 && left > std::numeric_limits<T>::max() + right) ||
            (right > 0 && left < std::numeric_limits<T>::min() + right)) {
            return std::nullopt;
        }
    } else {
        if (left != 0 &&
            (right == std::numeric_limits<T>::min() && left == -1)) {
            return std::nullopt;
        }
        const long double product = static_cast<long double>(left) *
            static_cast<long double>(right);
        if (product > static_cast<long double>(std::numeric_limits<T>::max()) ||
            product < static_cast<long double>(std::numeric_limits<T>::min())) {
            return std::nullopt;
        }
    }
    return operation(left, right);
#endif
}

bool constantEqual(const Constant& left, const Constant& right) {
    if (left.type != right.type || left.typeId != right.typeId ||
        left.data.index() != right.data.index()) {
        return false;
    }
    if (const auto* l = std::get_if<double>(&left.data)) {
        const auto r = std::get<double>(right.data);
        if (std::isnan(*l) || std::isnan(r)) return false;
        return *l == r;
    }
    return left.data == right.data;
}

std::optional<Constant> foldInstruction(
    const mir::Instruction& instruction,
    const std::unordered_map<mir::ValueId, Constant>& constants) {
    const auto operand = [&](std::size_t index) -> const Constant* {
        if (index >= instruction.operands.size()) return nullptr;
        const auto found = constants.find(instruction.operands[index]);
        return found == constants.end() ? nullptr : &found->second;
    };
    const auto unaryInt = [&](auto operation) -> std::optional<Constant> {
        const auto* value = operand(0);
        if (!value) return std::nullopt;
        const auto* integer = std::get_if<std::int64_t>(&value->data);
        if (!integer) return std::nullopt;
        Constant result{instruction.resultType, instruction.resultTypeId, std::int64_t{0}};
        const auto folded = operation(*integer);
        if (!folded) return std::nullopt;
        result.data = *folded;
        return result;
    };
    const auto binaryInt = [&](auto operation) -> std::optional<Constant> {
        const auto* left = operand(0);
        const auto* right = operand(1);
        if (!left || !right) return std::nullopt;
        const auto* l = std::get_if<std::int64_t>(&left->data);
        const auto* r = std::get_if<std::int64_t>(&right->data);
        if (!l || !r) return std::nullopt;
        const auto folded = operation(*l, *r);
        if (!folded) return std::nullopt;
        return Constant{instruction.resultType, instruction.resultTypeId, *folded};
    };
    const auto compareInt = [&](auto comparison) -> std::optional<Constant> {
        const auto* left = operand(0);
        const auto* right = operand(1);
        if (!left || !right) return std::nullopt;
        const auto* l = std::get_if<std::int64_t>(&left->data);
        const auto* r = std::get_if<std::int64_t>(&right->data);
        if (!l || !r) return std::nullopt;
        return Constant{semantic::PrimitiveType::Bool, 0, comparison(*l, *r)};
    };
    const auto binaryDouble = [&](auto operation) -> std::optional<Constant> {
        const auto* left = operand(0);
        const auto* right = operand(1);
        if (!left || !right) return std::nullopt;
        const auto* l = std::get_if<double>(&left->data);
        const auto* r = std::get_if<double>(&right->data);
        if (!l || !r) return std::nullopt;
        return Constant{semantic::PrimitiveType::Double, 0, operation(*l, *r)};
    };
    const auto compareDouble = [&](auto comparison) -> std::optional<Constant> {
        const auto* left = operand(0);
        const auto* right = operand(1);
        if (!left || !right) return std::nullopt;
        const auto* l = std::get_if<double>(&left->data);
        const auto* r = std::get_if<double>(&right->data);
        if (!l || !r) return std::nullopt;
        return Constant{semantic::PrimitiveType::Bool, 0, comparison(*l, *r)};
    };

    switch (instruction.opcode) {
    case mir::Opcode::ConvertIntToLong: {
        const auto* value = operand(0);
        if (!value) return std::nullopt;
        const auto* integer = std::get_if<std::int64_t>(&value->data);
        if (!integer) return std::nullopt;
        return Constant{semantic::PrimitiveType::Long, 0, *integer};
    }
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble: {
        const auto* value = operand(0);
        if (!value) return std::nullopt;
        const auto* integer = std::get_if<std::int64_t>(&value->data);
        if (!integer) return std::nullopt;
        return Constant{semantic::PrimitiveType::Double, 0,
            static_cast<double>(*integer)};
    }
    case mir::Opcode::LogicalNot: {
        const auto* value = operand(0);
        if (!value) return std::nullopt;
        const auto* boolean = std::get_if<bool>(&value->data);
        if (!boolean) return std::nullopt;
        return Constant{semantic::PrimitiveType::Bool, 0, !*boolean};
    }
    case mir::Opcode::NegateInt: {
        return unaryInt([](std::int64_t value) -> std::optional<std::int64_t> {
            const auto narrowed = static_cast<std::int32_t>(value);
            if (narrowed == std::numeric_limits<std::int32_t>::min()) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(-narrowed);
        });
    }
    case mir::Opcode::NegateLong:
        return unaryInt([](std::int64_t value) -> std::optional<std::int64_t> {
            if (value == std::numeric_limits<std::int64_t>::min()) {
                return std::nullopt;
            }
            return -value;
        });
    case mir::Opcode::NegateDouble: {
        const auto* value = operand(0);
        if (!value) return std::nullopt;
        const auto* number = std::get_if<double>(&value->data);
        if (!number) return std::nullopt;
        return Constant{semantic::PrimitiveType::Double, 0, -*number};
    }
    case mir::Opcode::AddInt:
        return binaryInt([](std::int64_t left, std::int64_t right) {
            const auto result = checkedBinary<std::int32_t>(
                static_cast<std::int32_t>(left),
                static_cast<std::int32_t>(right),
                std::plus<std::int32_t>{});
            return result ? std::optional<std::int64_t>(*result) : std::nullopt;
        });
    case mir::Opcode::SubtractInt:
        return binaryInt([](std::int64_t left, std::int64_t right) {
            const auto result = checkedBinary<std::int32_t>(
                static_cast<std::int32_t>(left),
                static_cast<std::int32_t>(right),
                std::minus<std::int32_t>{});
            return result ? std::optional<std::int64_t>(*result) : std::nullopt;
        });
    case mir::Opcode::MultiplyInt:
        return binaryInt([](std::int64_t left, std::int64_t right) {
            const auto result = checkedBinary<std::int32_t>(
                static_cast<std::int32_t>(left),
                static_cast<std::int32_t>(right),
                std::multiplies<std::int32_t>{});
            return result ? std::optional<std::int64_t>(*result) : std::nullopt;
        });
    case mir::Opcode::DivideInt:
    case mir::Opcode::RemainderInt:
        return binaryInt([&](std::int64_t left, std::int64_t right)
            -> std::optional<std::int64_t> {
            const auto l = static_cast<std::int32_t>(left);
            const auto r = static_cast<std::int32_t>(right);
            if (r == 0 || (l == std::numeric_limits<std::int32_t>::min() && r == -1)) {
                return std::nullopt;
            }
            return instruction.opcode == mir::Opcode::DivideInt
                ? static_cast<std::int64_t>(l / r)
                : static_cast<std::int64_t>(l % r);
        });
    case mir::Opcode::AddLong:
        return binaryInt([](auto left, auto right) {
            return checkedBinary<std::int64_t>(left, right, std::plus<std::int64_t>{});
        });
    case mir::Opcode::SubtractLong:
        return binaryInt([](auto left, auto right) {
            return checkedBinary<std::int64_t>(left, right, std::minus<std::int64_t>{});
        });
    case mir::Opcode::MultiplyLong:
        return binaryInt([](auto left, auto right) {
            return checkedBinary<std::int64_t>(left, right, std::multiplies<std::int64_t>{});
        });
    case mir::Opcode::DivideLong:
    case mir::Opcode::RemainderLong:
        return binaryInt([&](std::int64_t left, std::int64_t right)
            -> std::optional<std::int64_t> {
            if (right == 0 ||
                (left == std::numeric_limits<std::int64_t>::min() && right == -1)) {
                return std::nullopt;
            }
            return instruction.opcode == mir::Opcode::DivideLong
                ? left / right
                : left % right;
        });
    case mir::Opcode::AddDouble:
        return binaryDouble([](double left, double right) { return left + right; });
    case mir::Opcode::SubtractDouble:
        return binaryDouble([](double left, double right) { return left - right; });
    case mir::Opcode::MultiplyDouble:
        return binaryDouble([](double left, double right) { return left * right; });
    case mir::Opcode::DivideDouble:
        return binaryDouble([](double left, double right) { return left / right; });
    case mir::Opcode::LessInt:
    case mir::Opcode::LessLong:
        return compareInt(std::less<std::int64_t>{});
    case mir::Opcode::LessOrEqualInt:
    case mir::Opcode::LessOrEqualLong:
        return compareInt(std::less_equal<std::int64_t>{});
    case mir::Opcode::GreaterInt:
    case mir::Opcode::GreaterLong:
        return compareInt(std::greater<std::int64_t>{});
    case mir::Opcode::GreaterOrEqualInt:
    case mir::Opcode::GreaterOrEqualLong:
        return compareInt(std::greater_equal<std::int64_t>{});
    case mir::Opcode::LessDouble:
        return compareDouble(std::less<double>{});
    case mir::Opcode::LessOrEqualDouble:
        return compareDouble(std::less_equal<double>{});
    case mir::Opcode::GreaterDouble:
        return compareDouble(std::greater<double>{});
    case mir::Opcode::GreaterOrEqualDouble:
        return compareDouble(std::greater_equal<double>{});
    case mir::Opcode::Equal:
    case mir::Opcode::NotEqual: {
        const auto* left = operand(0);
        const auto* right = operand(1);
        if (!left || !right) return std::nullopt;
        const auto equal = constantEqual(*left, *right);
        return Constant{semantic::PrimitiveType::Bool, 0,
            instruction.opcode == mir::Opcode::Equal ? equal : !equal};
    }
    default:
        return std::nullopt;
    }
}

bool foldConstants(
    mir::Function& function,
    Statistics& statistics,
    bool enableStructFieldForwarding = true) {
    bool changed = false;
    std::unordered_map<mir::ValueId, Constant> constants;
    std::unordered_map<mir::ValueId, std::unordered_map<std::size_t, Constant>> structFieldConstants;
    std::unordered_map<mir::ValueId, std::unordered_map<std::size_t, mir::ValueId>> blockStructFieldValues;
    std::unordered_map<mir::BlockId, std::size_t> predCount;
    if (!function.blocks.empty()) {
        ++predCount[function.blocks.front().id];
    }
    for (const auto& b : function.blocks) {
        if (b.terminator.kind == mir::TerminatorKind::Jump) {
            ++predCount[b.terminator.target];
        } else if (b.terminator.kind == mir::TerminatorKind::Branch) {
            ++predCount[b.terminator.target];
            if (b.terminator.falseTarget != b.terminator.target) {
                ++predCount[b.terminator.falseTarget];
            }
        }
        for (const auto& h : function.exceptionHandlers) {
            for (const auto pb : h.protectedBlocks) {
                if (pb == b.id) ++predCount[h.handlerBlock];
            }
        }
    }
    std::unordered_map<mir::BlockId, std::unordered_map<std::size_t, Constant>> outLocalConstants;
    std::unordered_map<mir::BlockId, std::unordered_map<std::size_t, std::unordered_map<std::size_t, Constant>>> outLocalStructFields;
    std::unordered_map<mir::BlockId, std::unordered_map<std::size_t, std::unordered_map<std::size_t, mir::ValueId>>> outLocalStructFieldValues;
    for (auto& block : function.blocks) {
        std::unordered_map<std::size_t, Constant> localConstants;
        std::unordered_map<std::size_t, std::unordered_map<std::size_t, Constant>> localStructFields;
        std::unordered_map<std::size_t, std::unordered_map<std::size_t, mir::ValueId>> localStructFieldValues;
        if (predCount[block.id] == 1) {
            if (const auto it = outLocalConstants.find(block.id); it != outLocalConstants.end()) {
                localConstants = it->second;
            }
            if (const auto it = outLocalStructFields.find(block.id); it != outLocalStructFields.end()) {
                localStructFields = it->second;
            }
            if (const auto it = outLocalStructFieldValues.find(block.id); it != outLocalStructFieldValues.end()) {
                localStructFieldValues = it->second;
            }
        }
        std::unordered_map<mir::ValueId, mir::ValueId> valueReplacements;

        const auto resolveReplacement = [&](mir::ValueId v) {
            while (v >= 0) {
                const auto it = valueReplacements.find(v);
                if (it == valueReplacements.end() || it->second == v) break;
                v = it->second;
            }
            return v;
        };

        for (auto& instruction : block.instructions) {
            for (auto& op : instruction.operands) {
                const auto replaced = resolveReplacement(op);
                if (replaced != op) {
                    op = replaced;
                    changed = true;
                }
            }
            if (instruction.opcode == mir::Opcode::StoreLocal &&
                instruction.operands.size() == 1) {
                const auto srcVal = instruction.operands.front();
                const auto found = constants.find(srcVal);
                if (found != constants.end()) {
                    localConstants[instruction.localIndex] = found->second;
                } else {
                    localConstants.erase(instruction.localIndex);
                }
                const auto foundStructConst = structFieldConstants.find(srcVal);
                if (foundStructConst != structFieldConstants.end()) {
                    localStructFields[instruction.localIndex] = foundStructConst->second;
                } else {
                    localStructFields.erase(instruction.localIndex);
                }
                const auto foundStructVals = blockStructFieldValues.find(srcVal);
                if (foundStructVals != blockStructFieldValues.end()) {
                    localStructFieldValues[instruction.localIndex] = foundStructVals->second;
                } else {
                    localStructFieldValues.erase(instruction.localIndex);
                }
                continue;
            }
            if (instruction.opcode == mir::Opcode::Call ||
                instruction.opcode == mir::Opcode::InvokeDelegate ||
                instruction.opcode == mir::Opcode::AwaitSuspend ||
                instruction.opcode == mir::Opcode::AwaitResume ||
                instruction.opcode == mir::Opcode::StoreField ||
                instruction.opcode == mir::Opcode::StoreElement) {
                localConstants.clear();
                localStructFields.clear();
                localStructFieldValues.clear();
            }
            if (enableStructFieldForwarding &&
                instruction.opcode == mir::Opcode::NewStruct) {
                structFieldConstants[instruction.result].clear();
                blockStructFieldValues[instruction.result].clear();
            }
            if (enableStructFieldForwarding &&
                instruction.opcode == mir::Opcode::StoreStructField &&
                instruction.operands.size() == 2) {
                const auto baseStruct = instruction.operands[0];
                const auto fieldVal = instruction.operands[1];
                auto& fieldConstMap = structFieldConstants[instruction.result];
                if (const auto baseIt = structFieldConstants.find(baseStruct);
                    baseIt != structFieldConstants.end()) {
                    fieldConstMap = baseIt->second;
                }
                if (const auto constIt = constants.find(fieldVal);
                    constIt != constants.end()) {
                    fieldConstMap[instruction.fieldIndex] = constIt->second;
                } else {
                    fieldConstMap.erase(instruction.fieldIndex);
                }
                auto& fieldValMap = blockStructFieldValues[instruction.result];
                if (const auto baseValIt = blockStructFieldValues.find(baseStruct);
                    baseValIt != blockStructFieldValues.end()) {
                    fieldValMap = baseValIt->second;
                }
                fieldValMap[instruction.fieldIndex] = fieldVal;
            }
            if (instruction.opcode == mir::Opcode::LoadLocal) {
                const auto found = localConstants.find(instruction.localIndex);
                if (found != localConstants.end()) {
                    auto constant = found->second;
                    constant.type = instruction.resultType;
                    constant.typeId = instruction.resultTypeId;
                    instruction = makeConstant(instruction, std::move(constant));
                    ++statistics.localLoadsFolded;
                    changed = true;
                } else if (enableStructFieldForwarding) {
                    if (const auto foundStruct = localStructFields.find(instruction.localIndex);
                        foundStruct != localStructFields.end()) {
                        structFieldConstants[instruction.result] = foundStruct->second;
                    }
                    if (const auto foundStructVal = localStructFieldValues.find(instruction.localIndex);
                        foundStructVal != localStructFieldValues.end()) {
                        blockStructFieldValues[instruction.result] = foundStructVal->second;
                    }
                }
            }
            if (enableStructFieldForwarding &&
                instruction.opcode == mir::Opcode::LoadStructField &&
                instruction.operands.size() == 1) {
                const auto baseStruct = instruction.operands[0];
                bool foldedField = false;
                if (const auto structIt = structFieldConstants.find(baseStruct);
                    structIt != structFieldConstants.end()) {
                    if (const auto fieldIt = structIt->second.find(instruction.fieldIndex);
                        fieldIt != structIt->second.end() &&
                        fieldIt->second.type == instruction.resultType) {
                        auto constant = fieldIt->second;
                        constant.type = instruction.resultType;
                        constant.typeId = instruction.resultTypeId;
                        instruction = makeConstant(instruction, std::move(constant));
                        ++statistics.constantsFolded;
                        ++statistics.structFieldsForwarded;
                        changed = true;
                        foldedField = true;
                    }
                }
                if (!foldedField) {
                    if (const auto structValIt = blockStructFieldValues.find(baseStruct);
                        structValIt != blockStructFieldValues.end()) {
                        if (const auto fieldValIt = structValIt->second.find(instruction.fieldIndex);
                            fieldValIt != structValIt->second.end() &&
                            fieldValIt->second >= 0) {
                            valueReplacements[instruction.result] =
                                resolveReplacement(fieldValIt->second);
                            ++statistics.structFieldsForwarded;
                            changed = true;
                        }
                    }
                }
            }
            if (const auto folded = foldInstruction(instruction, constants)) {
                instruction = makeConstant(instruction, *folded);
                ++statistics.constantsFolded;
                changed = true;
            }
            if (isValueInstruction(instruction)) {
                if (const auto constant = constantFromInstruction(instruction)) {
                    constants[instruction.result] = *constant;
                } else {
                    constants.erase(instruction.result);
                }
            }
        }
        if (!valueReplacements.empty()) {
            block.terminator.condition = resolveReplacement(block.terminator.condition);
            block.terminator.value = resolveReplacement(block.terminator.value);
            for (auto& arg : block.terminator.arguments) arg = resolveReplacement(arg);
            for (auto& arg : block.terminator.falseArguments) arg = resolveReplacement(arg);
        }
        if (block.terminator.kind == mir::TerminatorKind::Branch) {
            const auto found = constants.find(block.terminator.condition);
            if (found != constants.end()) {
                if (const auto* condition = std::get_if<bool>(&found->second.data)) {
                    const bool takeTrue = *condition;
                    block.terminator.kind = mir::TerminatorKind::Jump;
                    block.terminator.target = takeTrue
                        ? block.terminator.target
                        : block.terminator.falseTarget;
                    block.terminator.arguments = takeTrue
                        ? block.terminator.arguments
                        : block.terminator.falseArguments;
                    block.terminator.falseTarget = 0;
                    block.terminator.falseArguments.clear();
                    block.terminator.condition = -1;
                    ++statistics.branchesFolded;
                    changed = true;
                }
            }
        }
        if (block.terminator.kind == mir::TerminatorKind::Jump &&
            predCount[block.terminator.target] == 1) {
            outLocalConstants[block.terminator.target] = localConstants;
            outLocalStructFields[block.terminator.target] = localStructFields;
            outLocalStructFieldValues[block.terminator.target] = localStructFieldValues;
        }
    }
    return changed;
}

bool removeUnreachableBlocks(mir::Function& function, Statistics& statistics) {
    if (function.blocks.empty()) return false;
    std::unordered_map<mir::BlockId, const mir::BasicBlock*> blocks;
    for (const auto& block : function.blocks) blocks.emplace(block.id, &block);
    std::unordered_set<mir::BlockId> reachable;
    std::deque<mir::BlockId> queue;
    if (blocks.find(0) != blocks.end()) {
        reachable.insert(0);
        queue.push_back(0);
    }
    while (!queue.empty()) {
        const auto id = queue.front();
        queue.pop_front();
        const auto* block = blocks.at(id);
        const auto add = [&](mir::BlockId target) {
            if (blocks.find(target) != blocks.end() && reachable.insert(target).second) {
                queue.push_back(target);
            }
        };
        if (block->terminator.kind == mir::TerminatorKind::Jump) {
            add(block->terminator.target);
        } else if (block->terminator.kind == mir::TerminatorKind::Branch) {
            add(block->terminator.target);
            add(block->terminator.falseTarget);
        }
        for (const auto& handler : function.exceptionHandlers) {
            if (std::find(handler.protectedBlocks.begin(),
                    handler.protectedBlocks.end(), id) !=
                handler.protectedBlocks.end()) {
                add(handler.handlerBlock);
            }
        }
    }
    const auto before = function.blocks.size();
    function.blocks.erase(
        std::remove_if(function.blocks.begin(), function.blocks.end(),
            [&](const auto& block) {
                return reachable.find(block.id) == reachable.end();
            }),
        function.blocks.end());
    for (auto& handler : function.exceptionHandlers) {
        handler.protectedBlocks.erase(
            std::remove_if(handler.protectedBlocks.begin(),
                handler.protectedBlocks.end(),
                [&](mir::BlockId block) {
                    return reachable.find(block) == reachable.end();
                }),
            handler.protectedBlocks.end());
    }
    function.exceptionHandlers.erase(
        std::remove_if(function.exceptionHandlers.begin(),
            function.exceptionHandlers.end(),
            [](const mir::ExceptionHandler& handler) {
                return handler.protectedBlocks.empty();
            }),
        function.exceptionHandlers.end());
    const auto removed = before - function.blocks.size();
    statistics.blocksRemoved += removed;
    return removed != 0;
}

bool eliminateDeadInstructions(mir::Function& function, Statistics& statistics) {
    std::unordered_set<mir::ValueId> used;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (!isRemovableWhenUnused(instruction)) {
                used.insert(instruction.operands.begin(), instruction.operands.end());
            }
        }
        const auto& terminator = block.terminator;
        if (terminator.condition >= 0) used.insert(terminator.condition);
        if (terminator.value >= 0) used.insert(terminator.value);
        used.insert(terminator.arguments.begin(), terminator.arguments.end());
        used.insert(terminator.falseArguments.begin(), terminator.falseArguments.end());
    }

    // Follow pure value dependencies to a fixed point. Block storage order is
    // not guaranteed to be reverse dominance order, especially after CFG
    // simplification, so a single reverse traversal can remove a producer that
    // is consumed by a live value in another block.
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (!isValueInstruction(instruction) ||
                    used.find(instruction.result) == used.end()) {
                    continue;
                }
                for (const auto operand : instruction.operands) {
                    expanded |= used.insert(operand).second;
                }
            }
        }
    }

    bool changed = false;
    for (auto& block : function.blocks) {
        const auto before = block.instructions.size();
        block.instructions.erase(
            std::remove_if(
                block.instructions.begin(),
                block.instructions.end(),
                [&](const mir::Instruction& instruction) {
                    return isValueInstruction(instruction) &&
                        isRemovableWhenUnused(instruction) &&
                        used.find(instruction.result) == used.end();
                }),
            block.instructions.end());
        const auto removed = before - block.instructions.size();
        statistics.instructionsRemoved += removed;
        changed |= removed != 0;
    }
    return changed;
}

void compactValueIds(mir::Function& function) {
    std::unordered_map<mir::ValueId, mir::ValueId> values;
    mir::ValueId next = 0;
    for (auto& block : function.blocks) {
        for (auto& parameter : block.parameters) {
            values.emplace(parameter.value, next);
            parameter.value = next++;
        }
        for (auto& instruction : block.instructions) {
            if (isValueInstruction(instruction)) {
                values.emplace(instruction.result, next);
                instruction.result = next++;
            }
        }
    }
    const auto remap = [&](mir::ValueId& value) {
        if (value < 0) return;
        const auto found = values.find(value);
        value = found == values.end() ? -1 : found->second;
    };
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            for (auto& operand : instruction.operands) remap(operand);
        }
        remap(block.terminator.condition);
        remap(block.terminator.value);
        for (auto& argument : block.terminator.arguments) remap(argument);
        for (auto& argument : block.terminator.falseArguments) remap(argument);
    }
}

void refreshDebugInfo(
    mir::Function& function,
    const std::vector<debug::SourceFileInfo>& files,
    bool preserve) {
    function.debugInfo.sequencePoints.clear();
    if (!preserve) return;
    const debug::SourceFileInfo* file = nullptr;
    for (const auto& candidate : files) {
        if (candidate.id == function.debugInfo.sourceFileId) {
            file = &candidate;
            break;
        }
    }
    if (!file) return;
    const auto append = [&](
        mir::BlockId blockId,
        std::uint32_t instructionIndex,
        bool terminator,
        text::TextSpan span) {
        if (span.empty()) return;
        debug::SequencePoint point;
        point.blockId = blockId;
        point.instructionIndex = instructionIndex;
        point.terminator = terminator;
        point.range = debug::makeSourceRange(*file, span);
        const auto duplicate = !function.debugInfo.sequencePoints.empty() &&
            function.debugInfo.sequencePoints.back().range.span.start ==
                point.range.span.start &&
            function.debugInfo.sequencePoints.back().range.span.length ==
                point.range.span.length;
        if (!duplicate) function.debugInfo.sequencePoints.push_back(std::move(point));
    };
    for (const auto& block : function.blocks) {
        for (std::uint32_t index = 0;
             index < static_cast<std::uint32_t>(block.instructions.size());
             ++index) {
            append(block.id, index, false, block.instructions[index].sourceSpan);
        }
        append(
            block.id,
            static_cast<std::uint32_t>(block.instructions.size()),
            true,
            block.terminator.sourceSpan);
    }
    debug::finalizeFunctionDebugInfo(function.debugInfo, files);
}

bool isLicmSafeOpcode(mir::Opcode opcode) noexcept {
    switch (opcode) {
    case mir::Opcode::ConstantNull:
    case mir::Opcode::ConstantBool:
    case mir::Opcode::ConstantInt:
    case mir::Opcode::ConstantDouble:
    case mir::Opcode::ConstantString:
    case mir::Opcode::ConvertIntToLong:
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
    case mir::Opcode::LogicalNot:
    case mir::Opcode::Equal:
    case mir::Opcode::NotEqual:
    case mir::Opcode::LessInt:
    case mir::Opcode::LessOrEqualInt:
    case mir::Opcode::GreaterInt:
    case mir::Opcode::GreaterOrEqualInt:
    case mir::Opcode::LessLong:
    case mir::Opcode::LessOrEqualLong:
    case mir::Opcode::GreaterLong:
    case mir::Opcode::GreaterOrEqualLong:
    case mir::Opcode::LessDouble:
    case mir::Opcode::LessOrEqualDouble:
    case mir::Opcode::GreaterDouble:
    case mir::Opcode::GreaterOrEqualDouble:
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble:
    case mir::Opcode::LoadStructField:
    case mir::Opcode::ConstantTypeId:
        return true;
    default:
        return false;
    }
}

bool hoistLoopInvariants(mir::Function& function, Statistics& statistics) {
    if (function.blocks.size() < 2 || !function.exceptionHandlers.empty()) {
        return false;
    }
    bool changed = false;
    std::unordered_map<mir::BlockId, std::size_t> blockIndexById;
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
        blockIndexById[function.blocks[i].id] = i;
    }
    std::vector<std::vector<std::size_t>> predecessors(function.blocks.size());
    for (std::size_t idx = 0; idx < function.blocks.size(); ++idx) {
        const auto& term = function.blocks[idx].terminator;
        if (term.kind == mir::TerminatorKind::Jump) {
            if (const auto it = blockIndexById.find(term.target); it != blockIndexById.end()) {
                predecessors[it->second].push_back(idx);
            }
        } else if (term.kind == mir::TerminatorKind::Branch) {
            if (const auto it = blockIndexById.find(term.target); it != blockIndexById.end()) {
                predecessors[it->second].push_back(idx);
            }
            if (term.falseTarget != term.target) {
                if (const auto it = blockIndexById.find(term.falseTarget);
                    it != blockIndexById.end()) {
                    predecessors[it->second].push_back(idx);
                }
            }
        }
    }

    for (std::size_t headerIdx = 1; headerIdx < function.blocks.size(); ++headerIdx) {
        const auto headerId = function.blocks[headerIdx].id;
        if (!function.blocks[headerIdx].parameters.empty()) continue;

        // Preheader must be headerIdx - 1 jumping unconditionally to headerId
        auto& preheader = function.blocks[headerIdx - 1];
        if (preheader.terminator.kind != mir::TerminatorKind::Jump ||
            preheader.terminator.target != headerId ||
            !preheader.terminator.arguments.empty()) {
            continue;
        }

        // Find back-edge blocks >= headerIdx jumping/branching to headerId and
        // construct the natural loop by walking predecessors backwards to headerIdx.
        std::size_t latchIdx = headerIdx;
        bool hasBackEdge = false;
        bool invalidPred = false;
        std::vector<bool> inNaturalLoop(function.blocks.size(), false);
        inNaturalLoop[headerIdx] = true;
        std::vector<std::size_t> worklist;
        for (const auto predIdx : predecessors[headerIdx]) {
            if (predIdx == headerIdx - 1) continue;
            if (predIdx < headerIdx) {
                invalidPred = true;
                break;
            }
            hasBackEdge = true;
            latchIdx = std::max(latchIdx, predIdx);
            if (!inNaturalLoop[predIdx]) {
                inNaturalLoop[predIdx] = true;
                worklist.push_back(predIdx);
            }
        }
        if (!hasBackEdge || invalidPred) continue;

        while (!worklist.empty() && !invalidPred) {
            const auto currIdx = worklist.back();
            worklist.pop_back();
            for (const auto predIdx : predecessors[currIdx]) {
                if (predIdx < headerIdx) {
                    invalidPred = true;
                    break;
                }
                if (!inNaturalLoop[predIdx]) {
                    inNaturalLoop[predIdx] = true;
                    latchIdx = std::max(latchIdx, predIdx);
                    worklist.push_back(predIdx);
                }
            }
        }
        if (invalidPred) continue;

        // Collect all ValueIds defined inside the natural loop
        // and the exact set of local variable indices written by StoreLocal.
        // In RealScript MIR, StoreField, StoreElement, and Call never alias
        // function-local slots (ref/out call writebacks emit explicit StoreLocal).
        std::unordered_set<mir::ValueId> loopDefinedValues;
        std::unordered_set<std::size_t> loopModifiedLocals;
        for (std::size_t idx = headerIdx; idx <= latchIdx; ++idx) {
            if (!inNaturalLoop[idx]) continue;
            for (const auto& param : function.blocks[idx].parameters) {
                loopDefinedValues.insert(param.value);
            }
            for (const auto& inst : function.blocks[idx].instructions) {
                if (isValueInstruction(inst)) {
                    loopDefinedValues.insert(inst.result);
                }
                if (inst.opcode == mir::Opcode::StoreLocal ||
                    inst.opcode == mir::Opcode::AwaitSuspend) {
                    loopModifiedLocals.insert(inst.localIndex);
                }
            }
        }

        for (std::size_t blkIdx = headerIdx; blkIdx <= latchIdx; ++blkIdx) {
            if (!inNaturalLoop[blkIdx]) continue;
            auto& loopBlock = function.blocks[blkIdx];
            std::vector<mir::Instruction> remaining;
            remaining.reserve(loopBlock.instructions.size());
            for (auto& inst : loopBlock.instructions) {
                bool canHoist = false;
                if (isValueInstruction(inst)) {
                    if (isLicmSafeOpcode(inst.opcode) ||
                        ((inst.opcode == mir::Opcode::AddInt ||
                          inst.opcode == mir::Opcode::SubtractInt ||
                          inst.opcode == mir::Opcode::MultiplyInt ||
                          inst.opcode == mir::Opcode::AddLong ||
                          inst.opcode == mir::Opcode::SubtractLong ||
                          inst.opcode == mir::Opcode::MultiplyLong) &&
                         !inst.checkedArithmetic) ||
                        (inst.opcode == mir::Opcode::LoadLocal &&
                         inst.resultType != semantic::PrimitiveType::Array &&
                         loopModifiedLocals.find(inst.localIndex) == loopModifiedLocals.end())) {
                        bool allOperandsOutside = true;
                        for (const auto op : inst.operands) {
                            if (loopDefinedValues.find(op) != loopDefinedValues.end()) {
                                allOperandsOutside = false;
                                break;
                            }
                        }
                        if (allOperandsOutside) {
                            canHoist = true;
                        }
                    }
                }
                if (canHoist) {
                    loopDefinedValues.erase(inst.result);
                    preheader.instructions.push_back(std::move(inst));
                    ++statistics.loopInvariantsHoisted;
                    changed = true;
                } else {
                    remaining.push_back(std::move(inst));
                }
            }
            loopBlock.instructions = std::move(remaining);
        }
    }
    return changed;
}

bool isInlineSafeInstruction(const mir::Instruction& inst) noexcept {
    switch (inst.opcode) {
    case mir::Opcode::ConstantNull:
    case mir::Opcode::ConstantBool:
    case mir::Opcode::ConstantInt:
    case mir::Opcode::ConstantDouble:
    case mir::Opcode::ConstantString:
    case mir::Opcode::ConvertIntToLong:
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
    case mir::Opcode::ConvertNullToString:
    case mir::Opcode::ConvertNullToObject:
    case mir::Opcode::ConvertNullToArray:
    case mir::Opcode::LogicalNot:
    case mir::Opcode::Equal:
    case mir::Opcode::NotEqual:
    case mir::Opcode::LessInt:
    case mir::Opcode::LessOrEqualInt:
    case mir::Opcode::GreaterInt:
    case mir::Opcode::GreaterOrEqualInt:
    case mir::Opcode::LessLong:
    case mir::Opcode::LessOrEqualLong:
    case mir::Opcode::GreaterLong:
    case mir::Opcode::GreaterOrEqualLong:
    case mir::Opcode::LessDouble:
    case mir::Opcode::LessOrEqualDouble:
    case mir::Opcode::GreaterDouble:
    case mir::Opcode::GreaterOrEqualDouble:
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble:
    case mir::Opcode::NegateDouble:
    case mir::Opcode::NewStruct:
    case mir::Opcode::LoadStructField:
    case mir::Opcode::StoreStructField:
    case mir::Opcode::CheckNotNull:
    case mir::Opcode::LoadField:
    case mir::Opcode::StoreField:
    case mir::Opcode::ArrayLength:
    case mir::Opcode::LoadElement:
    case mir::Opcode::StoreElement:
    case mir::Opcode::ConvertNumeric:
    case mir::Opcode::Parameter:
    case mir::Opcode::LoadLocal:
    case mir::Opcode::StoreLocal:
        return true;
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt:
    case mir::Opcode::DivideInt:
    case mir::Opcode::RemainderInt:
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong:
    case mir::Opcode::DivideLong:
    case mir::Opcode::RemainderLong:
    case mir::Opcode::NegateInt:
    case mir::Opcode::NegateLong:
        return true;
    default:
        return false;
    }
}

bool isPrioritySpanInlineFunction(const mir::Function& func) noexcept {
    const auto dot = func.name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= func.name.size()) {
        return false;
    }
    const auto spanPos = func.name.find(".Span__");
    const auto roSpanPos = func.name.find(".ReadOnlySpan__");
    const bool isSpanType =
        func.name.compare(0, 6, "Span__") == 0 ||
        func.name.compare(0, 14, "ReadOnlySpan__") == 0 ||
        (spanPos != std::string::npos && spanPos < dot) ||
        (roSpanPos != std::string::npos && roSpanPos < dot);
    if (!isSpanType) {
        return false;
    }
    const std::size_t methodStart = dot + 1;
    const std::size_t methodLen = func.name.size() - methodStart;
    return (methodLen == 3 && func.name.compare(methodStart, 3, "Get") == 0) ||
        (methodLen == 3 && func.name.compare(methodStart, 3, "Set") == 0) ||
        (methodLen == 5 && func.name.compare(methodStart, 5, "Slice") == 0) ||
        (methodLen == 9 && func.name.compare(methodStart, 9, "FromArray") == 0) ||
        (methodLen == 10 && func.name.compare(methodStart, 10, "get_Length") == 0) ||
        (methodLen == 11 && func.name.compare(methodStart, 11, "get_IsEmpty") == 0);
}

mir::ValueId findMaxValueId(const mir::Function& function) noexcept {
    mir::ValueId maxId = -1;
    for (const auto& block : function.blocks) {
        for (const auto& param : block.parameters) {
            maxId = std::max(maxId, param.value);
        }
        for (const auto& inst : block.instructions) {
            maxId = std::max(maxId, inst.result);
            for (const auto op : inst.operands) maxId = std::max(maxId, op);
        }
    }
    return maxId;
}

std::uint64_t computeMirFunctionBodyFingerprint(const mir::Function& fn) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    const auto mixU64 = [&](std::uint64_t v) noexcept {
        for (unsigned shift = 0; shift < 64u; shift += 8u) {
            h ^= static_cast<std::uint8_t>((v >> shift) & 0xFFu);
            h *= 1099511628211ULL;
        }
    };
    const auto mixStr = [&](const std::string& s) noexcept {
        mixU64(s.size());
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
    };
    mixU64(fn.symbolId);
    mixU64(static_cast<std::uint64_t>(fn.returnType));
    mixU64(fn.returnTypeId);
    mixU64(fn.parameterTypes.size());
    for (std::size_t i = 0; i < fn.parameterTypes.size(); ++i) {
        mixU64(static_cast<std::uint64_t>(fn.parameterTypes[i]));
        mixU64(i < fn.parameterTypeIds.size() ? fn.parameterTypeIds[i] : 0ULL);
    }
    mixU64(fn.blocks.size());
    for (const auto& blk : fn.blocks) {
        mixU64(blk.id);
        mixU64(blk.instructions.size());
        for (const auto& inst : blk.instructions) {
            mixU64(static_cast<std::uint64_t>(inst.opcode));
            mixU64(static_cast<std::uint64_t>(inst.result));
            mixU64(static_cast<std::uint64_t>(inst.resultType));
            mixU64(inst.resultTypeId);
            mixU64(inst.typeId);
            mixU64(inst.symbolId);
            mixU64(static_cast<std::uint64_t>(inst.integerImmediate));
            std::uint64_t dBits = 0;
            std::memcpy(&dBits, &inst.doubleImmediate, sizeof(double));
            mixU64(dBits);
            mixU64(inst.boolImmediate ? 1ULL : 0ULL);
            mixStr(inst.stringImmediate);
            mixU64(inst.fieldIndex);
            mixU64(inst.localIndex);
            mixU64(inst.checkedArithmetic ? 1ULL : 0ULL);
            mixU64(inst.operands.size());
            for (const auto op : inst.operands) {
                mixU64(static_cast<std::uint64_t>(op));
            }
        }
        mixU64(static_cast<std::uint64_t>(blk.terminator.kind));
        mixU64(static_cast<std::uint64_t>(blk.terminator.value));
        mixU64(static_cast<std::uint64_t>(blk.terminator.condition));
        mixU64(blk.terminator.target);
        mixU64(blk.terminator.falseTarget);
    }
    return h == 0ULL ? 1ULL : h;
}

bool inlineSmallFunctions(
    mir::Module& module,
    const Options& options,
    Statistics& statistics,
    const std::vector<mir::Module>* allModules = nullptr) {
    if (options.level != Level::Aggressive || !options.enableInlining) {
        return false;
    }
    std::unordered_set<semantic::SymbolId> moduleTypeIds;
    for (const auto& t : module.types) {
        if (t.id != 0) moduleTypeIds.insert(t.id);
    }
    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> importableValueTypes;
    if (allModules) {
        for (const auto& m : *allModules) {
            for (const auto& t : m.types) {
                if (t.id != 0 &&
                    (t.kind == semantic::TypeKind::Struct || t.kind == semantic::TypeKind::Enum)) {
                    importableValueTypes.emplace(t.id, &t);
                }
            }
        }
    }
    const auto isTypeAvailableOrImportable = [&](semantic::SymbolId tid) noexcept {
        if (tid == 0) return true;
        return moduleTypeIds.find(tid) != moduleTypeIds.end() ||
            importableValueTypes.find(tid) != importableValueTypes.end();
    };
    const std::function<void(semantic::SymbolId)> ensureTypeImported = [&](semantic::SymbolId tid) {
        if (tid == 0 || moduleTypeIds.find(tid) != moduleTypeIds.end()) return;
        const auto it = importableValueTypes.find(tid);
        if (it != importableValueTypes.end()) {
            moduleTypeIds.insert(tid);
            module.types.push_back(*it->second);
            for (const auto& field : it->second->fields) {
                if ((field.type == semantic::PrimitiveType::Struct ||
                     field.type == semantic::PrimitiveType::Enum) &&
                    !field.typeName.empty()) {
                    ensureTypeImported(semantic::stableTypeId(field.typeName));
                }
            }
        }
    };
    std::unordered_map<semantic::SymbolId, const mir::Function*> inlineCandidates;
    std::unordered_set<semantic::SymbolId> prioritySpanCandidates;
    const auto collectFromModule = [&](const mir::Module& candMod) {
        for (const auto& func : candMod.functions) {
            const bool prioritySpan = isPrioritySpanInlineFunction(func);
            if (func.blocks.empty() ||
                !func.exceptionHandlers.empty() ||
                func.name.find("..ctor") != std::string::npos ||
                func.name.find("..cctor") != std::string::npos ||
                func.name.find("$") != std::string::npos) {
                continue;
            }
            if (!prioritySpan && func.blocks.size() != 1) {
                continue;
            }
            if (prioritySpan && func.blocks.size() > 16) {
                continue;
            }
            const auto& entry = func.blocks.front();
            if (!entry.parameters.empty()) {
                continue;
            }
            if (func.blocks.size() == 1 &&
                entry.terminator.kind != mir::TerminatorKind::ReturnValue &&
                entry.terminator.kind != mir::TerminatorKind::ReturnVoid) {
                continue;
            }
            bool allSafe = true;
            for (const auto tid : func.localTypeIds) {
                if (!isTypeAvailableOrImportable(tid)) {
                    allSafe = false;
                    break;
                }
            }
            std::size_t materializedCount = 0;
            for (const auto& blk : func.blocks) {
                if (!allSafe) break;
                const auto tk = blk.terminator.kind;
                if (tk != mir::TerminatorKind::Jump &&
                    tk != mir::TerminatorKind::Branch &&
                    tk != mir::TerminatorKind::ReturnValue &&
                    tk != mir::TerminatorKind::ReturnVoid &&
                    tk != mir::TerminatorKind::Throw) {
                    allSafe = false;
                    break;
                }
                for (const auto& inst : blk.instructions) {
                    if (!isInlineSafeInstruction(inst) ||
                        !isTypeAvailableOrImportable(inst.typeId) ||
                        !isTypeAvailableOrImportable(inst.resultTypeId) ||
                        !isTypeAvailableOrImportable(inst.elementTypeId)) {
                        allSafe = false;
                        break;
                    }
                    if (inst.opcode != mir::Opcode::Parameter &&
                        inst.opcode != mir::Opcode::LoadLocal &&
                        inst.opcode != mir::Opcode::StoreLocal) {
                        ++materializedCount;
                    }
                }
            }
            const std::size_t budget = prioritySpan
                ? std::max<std::size_t>(options.maxInlineInstructions * 4, 64u)
                : options.maxInlineInstructions;
            if (allSafe && materializedCount <= budget) {
                inlineCandidates[func.symbolId] = &func;
                if (prioritySpan) {
                    prioritySpanCandidates.insert(func.symbolId);
                }
            }
        }
    };
    // Stamp pre-inlining MIR body fingerprints for all functions in this module so downstream
    // hot-reload checks can validate cross-module inlined callees deterministically.
    module.languageMetadata.functionInlineStamps.clear();
    module.languageMetadata.functionInlineStamps.reserve(module.functions.size());
    for (const auto& ownFn : module.functions) {
        if (ownFn.symbolId != 0) {
            module.languageMetadata.functionInlineStamps.push_back(
                compiler::LanguageFunctionInlineStamp{
                    ownFn.symbolId,
                    computeMirFunctionBodyFingerprint(ownFn)});
        }
    }
    std::sort(
        module.languageMetadata.functionInlineStamps.begin(),
        module.languageMetadata.functionInlineStamps.end(),
        [](const compiler::LanguageFunctionInlineStamp& a,
           const compiler::LanguageFunctionInlineStamp& b) {
            return a.symbolId < b.symbolId;
        });

    collectFromModule(module);
    if (allModules) {
        for (const auto& m : *allModules) {
            if (&m != &module) collectFromModule(m);
        }
    }
    if (inlineCandidates.empty()) return false;

    std::unordered_map<semantic::SymbolId, std::uint64_t> candidateFingerprints;
    candidateFingerprints.reserve(inlineCandidates.size());
    for (const auto& [symId, fnPtr] : inlineCandidates) {
        candidateFingerprints.emplace(symId, computeMirFunctionBodyFingerprint(*fnPtr));
    }

    // Collect existing cross-module inline dependencies per function for transitive propagation
    std::unordered_map<semantic::SymbolId, std::vector<compiler::LanguageInlinedDependency>> transitiveDeps;
    auto ingestModuleDeps = [&](const mir::Module& m) {
        for (const auto& dep : m.languageMetadata.inlinedDependencies) {
            transitiveDeps[dep.callerSymbolId].push_back(dep);
        }
    };
    ingestModuleDeps(module);
    if (allModules) {
        for (const auto& m : *allModules) {
            if (&m != &module) ingestModuleDeps(m);
        }
    }

    const auto recordInlinedEdge = [&](const mir::Function& callerFn, const mir::Function& calleeFn) {
        auto addDep = [&](const std::string& cMod,
                          const std::string& cName,
                          semantic::SymbolId cSym,
                          std::uint64_t cFp) {
            if (cMod.empty() || cMod == module.name || cSym == 0) return;
            for (const auto& existing : module.languageMetadata.inlinedDependencies) {
                if (existing.callerSymbolId == callerFn.symbolId &&
                    existing.calleeSymbolId == cSym) {
                    return;
                }
            }
            compiler::LanguageInlinedDependency rec;
            rec.callerFunction = callerFn.name;
            rec.callerSymbolId = callerFn.symbolId;
            rec.calleeModule = cMod;
            rec.calleeFunction = cName;
            rec.calleeSymbolId = cSym;
            rec.calleeBodyFingerprint = cFp;
            module.languageMetadata.inlinedDependencies.push_back(rec);
            transitiveDeps[callerFn.symbolId].push_back(std::move(rec));
        };
        if (calleeFn.moduleName != module.name) {
            addDep(
                calleeFn.moduleName,
                calleeFn.name,
                calleeFn.symbolId,
                candidateFingerprints[calleeFn.symbolId]);
        }
        const auto trIt = transitiveDeps.find(calleeFn.symbolId);
        if (trIt != transitiveDeps.end()) {
            const auto copyList = trIt->second;
            for (const auto& sub : copyList) {
                addDep(sub.calleeModule, sub.calleeFunction, sub.calleeSymbolId, sub.calleeBodyFingerprint);
            }
        }
    };

    bool anyInlined = false;
    for (auto& caller : module.functions) {
        mir::ValueId nextId = findMaxValueId(caller) + 1;
        mir::BlockId nextBlockId = 0;
        for (const auto& b : caller.blocks) {
            nextBlockId = std::max(nextBlockId, static_cast<mir::BlockId>(b.id + 1));
        }
        bool callerChanged = false;
        std::unordered_map<mir::ValueId, mir::ValueId> replacements;
        const auto resolveVal = [&](mir::ValueId v) {
            while (v >= 0) {
                const auto it = replacements.find(v);
                if (it == replacements.end() || it->second == v) break;
                v = it->second;
            }
            return v;
        };

        auto inlineMultiBlockCfg = [&](std::size_t blockIdx,
                                       std::size_t callInstIdx,
                                       const mir::Function& callee) -> bool {
            const auto& callInst = caller.blocks[blockIdx].instructions[callInstIdx];
            const bool expectsValue =
                callInst.resultType != semantic::PrimitiveType::Void && callInst.result >= 0;
            for (const auto& cb : callee.blocks) {
                if (cb.terminator.kind == mir::TerminatorKind::ReturnValue && !expectsValue) {
                    return false;
                }
                if (cb.terminator.kind == mir::TerminatorKind::ReturnVoid && expectsValue) {
                    return false;
                }
            }

            std::unordered_map<mir::BlockId, mir::BlockId> blockIdMap;
            blockIdMap.reserve(callee.blocks.size());
            std::unordered_map<mir::ValueId, mir::ValueId> valueMap;
            for (const auto& cb : callee.blocks) {
                blockIdMap[cb.id] = nextBlockId++;
                for (const auto& p : cb.parameters) {
                    valueMap[p.value] = nextId++;
                }
                for (const auto& ci : cb.instructions) {
                    if (ci.opcode == mir::Opcode::Parameter) {
                        const auto pIdx = static_cast<std::size_t>(ci.integerImmediate);
                        if (pIdx >= callInst.operands.size()) return false;
                        valueMap[ci.result] = callInst.operands[pIdx];
                    } else if (isValueInstruction(ci)) {
                        valueMap[ci.result] = nextId++;
                    }
                }
            }
            auto resolveCalleeVal = [&](mir::ValueId v) -> mir::ValueId {
                if (v < 0) return v;
                const auto it = valueMap.find(v);
                return it == valueMap.end() ? v : it->second;
            };

            const mir::BlockId contBlockId = nextBlockId++;
            mir::BasicBlock contBlock;
            contBlock.id = contBlockId;
            if (expectsValue) {
                const mir::ValueId contRetVal = nextId++;
                contBlock.parameters.push_back(mir::BlockParameter{
                    contRetVal, callInst.resultType, callInst.resultTypeId});
                replacements[callInst.result] = contRetVal;
            }
            for (std::size_t j = callInstIdx + 1; j < caller.blocks[blockIdx].instructions.size(); ++j) {
                auto tailInst = std::move(caller.blocks[blockIdx].instructions[j]);
                for (auto& op : tailInst.operands) op = resolveVal(op);
                contBlock.instructions.push_back(std::move(tailInst));
            }
            contBlock.terminator = std::move(caller.blocks[blockIdx].terminator);
            contBlock.terminator.condition = resolveVal(contBlock.terminator.condition);
            contBlock.terminator.value = resolveVal(contBlock.terminator.value);
            for (auto& arg : contBlock.terminator.arguments) arg = resolveVal(arg);
            for (auto& arg : contBlock.terminator.falseArguments) arg = resolveVal(arg);

            const std::size_t localBase = caller.localTypes.size();
            for (std::size_t li = 0; li < callee.localTypes.size(); ++li) {
                caller.localTypes.push_back(callee.localTypes[li]);
                const auto ltid = li < callee.localTypeIds.size() ? callee.localTypeIds[li] : 0;
                ensureTypeImported(ltid);
                caller.localTypeIds.push_back(ltid);
            }

            const bool crossOrigin =
                callee.moduleName != caller.moduleName ||
                callee.debugInfo.sourceFileId != caller.debugInfo.sourceFileId;
            std::vector<mir::BasicBlock> newBlocks;
            newBlocks.reserve(callee.blocks.size() + 1);
            for (const auto& cb : callee.blocks) {
                mir::BasicBlock nb;
                nb.id = blockIdMap[cb.id];
                for (const auto& p : cb.parameters) {
                    ensureTypeImported(p.typeId);
                    nb.parameters.push_back(mir::BlockParameter{
                        resolveCalleeVal(p.value), p.type, p.typeId});
                }
                for (const auto& ci : cb.instructions) {
                    if (ci.opcode == mir::Opcode::Parameter) continue;
                    mir::Instruction cloned = ci;
                    if (crossOrigin) {
                        cloned.sourceSpan = callInst.sourceSpan;
                    }
                    if (cloned.opcode == mir::Opcode::LoadLocal ||
                        cloned.opcode == mir::Opcode::StoreLocal) {
                        cloned.localIndex += localBase;
                    }
                    for (auto& op : cloned.operands) {
                        op = resolveCalleeVal(op);
                    }
                    if (isValueInstruction(cloned)) {
                        cloned.result = resolveCalleeVal(ci.result);
                    }
                    ensureTypeImported(cloned.typeId);
                    ensureTypeImported(cloned.resultTypeId);
                    ensureTypeImported(cloned.elementTypeId);
                    nb.instructions.push_back(std::move(cloned));
                }
                nb.terminator = cb.terminator;
                if (crossOrigin) {
                    nb.terminator.sourceSpan = callInst.sourceSpan;
                }
                switch (cb.terminator.kind) {
                case mir::TerminatorKind::ReturnVoid:
                    nb.terminator.kind = mir::TerminatorKind::Jump;
                    nb.terminator.target = contBlockId;
                    nb.terminator.value = -1;
                    nb.terminator.arguments.clear();
                    break;
                case mir::TerminatorKind::ReturnValue:
                    nb.terminator.kind = mir::TerminatorKind::Jump;
                    nb.terminator.target = contBlockId;
                    nb.terminator.arguments = {resolveCalleeVal(cb.terminator.value)};
                    nb.terminator.value = -1;
                    break;
                case mir::TerminatorKind::Jump:
                    nb.terminator.target = blockIdMap[cb.terminator.target];
                    for (auto& arg : nb.terminator.arguments) {
                        arg = resolveCalleeVal(arg);
                    }
                    break;
                case mir::TerminatorKind::Branch:
                    nb.terminator.condition = resolveCalleeVal(cb.terminator.condition);
                    nb.terminator.target = blockIdMap[cb.terminator.target];
                    nb.terminator.falseTarget = blockIdMap[cb.terminator.falseTarget];
                    for (auto& arg : nb.terminator.arguments) {
                        arg = resolveCalleeVal(arg);
                    }
                    for (auto& arg : nb.terminator.falseArguments) {
                        arg = resolveCalleeVal(arg);
                    }
                    break;
                case mir::TerminatorKind::Throw:
                    nb.terminator.value = resolveCalleeVal(cb.terminator.value);
                    break;
                case mir::TerminatorKind::None:
                    break;
                }
                newBlocks.push_back(std::move(nb));
            }

            const mir::BlockId origBlockId = caller.blocks[blockIdx].id;
            caller.blocks[blockIdx].instructions.resize(callInstIdx);
            caller.blocks[blockIdx].terminator = mir::Terminator{};
            caller.blocks[blockIdx].terminator.kind = mir::TerminatorKind::Jump;
            caller.blocks[blockIdx].terminator.target = blockIdMap[callee.blocks.front().id];
            caller.blocks[blockIdx].terminator.sourceSpan = callInst.sourceSpan;

            for (auto& eh : caller.exceptionHandlers) {
                if (std::find(eh.protectedBlocks.begin(), eh.protectedBlocks.end(), origBlockId) !=
                    eh.protectedBlocks.end()) {
                    for (const auto& nb : newBlocks) {
                        eh.protectedBlocks.push_back(nb.id);
                    }
                    eh.protectedBlocks.push_back(contBlockId);
                }
            }

            for (auto& nb : newBlocks) {
                caller.blocks.push_back(std::move(nb));
            }
            caller.blocks.push_back(std::move(contBlock));

            recordInlinedEdge(caller, callee);
            ++statistics.functionsInlined;
            callerChanged = true;
            anyInlined = true;
            return true;
        };

        std::size_t inlinesInCaller = 0;
        for (const bool priorityPass : {true, false}) {
            if (priorityPass && prioritySpanCandidates.empty()) continue;
            for (std::size_t bi = 0; bi < caller.blocks.size() && inlinesInCaller < 64; ++bi) {
                bool splitHappened = false;
                std::vector<mir::Instruction> expanded;
                expanded.reserve(caller.blocks[bi].instructions.size());
                for (std::size_t ii = 0; ii < caller.blocks[bi].instructions.size(); ++ii) {
                    auto& inst = caller.blocks[bi].instructions[ii];
                    for (auto& op : inst.operands) op = resolveVal(op);
                    if (inst.opcode == mir::Opcode::Call &&
                        !inst.virtualDispatch && !inst.interfaceDispatch &&
                        inst.symbolId != 0 &&
                        inlinesInCaller < 64) {
                        const auto targetSym = inst.symbolId;
                        const bool isPriorityTarget =
                            prioritySpanCandidates.find(targetSym) != prioritySpanCandidates.end();
                        const auto candIt = inlineCandidates.find(targetSym);
                        if ((!priorityPass || isPriorityTarget) &&
                            candIt != inlineCandidates.end() &&
                            targetSym != caller.symbolId &&
                            candIt->second->parameterTypes.size() == inst.operands.size()) {
                            const auto& callee = *candIt->second;
                            bool singleBlockDone = false;
                            if (callee.blocks.size() == 1) {
                                const auto& calleeBlock = callee.blocks.front();
                                std::unordered_map<mir::ValueId, mir::ValueId> valueMap;
                                std::unordered_map<std::size_t, mir::ValueId> localMap;
                                bool inlineOk = true;
                                std::vector<mir::Instruction> stagedClones;
                                for (const auto& calleeInst : calleeBlock.instructions) {
                                    if (calleeInst.opcode == mir::Opcode::Parameter) {
                                        const auto pIdx = static_cast<std::size_t>(calleeInst.integerImmediate);
                                        if (pIdx < inst.operands.size()) {
                                            valueMap[calleeInst.result] = inst.operands[pIdx];
                                        } else {
                                            inlineOk = false;
                                            break;
                                        }
                                        continue;
                                    }
                                    if (calleeInst.opcode == mir::Opcode::StoreLocal) {
                                        if (calleeInst.operands.size() == 1) {
                                            const auto m = valueMap.find(calleeInst.operands[0]);
                                            if (m != valueMap.end()) {
                                                localMap[calleeInst.localIndex] = m->second;
                                                continue;
                                            }
                                        }
                                        inlineOk = false;
                                        break;
                                    }
                                    if (calleeInst.opcode == mir::Opcode::LoadLocal) {
                                        const auto lm = localMap.find(calleeInst.localIndex);
                                        if (lm != localMap.end()) {
                                            valueMap[calleeInst.result] = lm->second;
                                            continue;
                                        }
                                        inlineOk = false;
                                        break;
                                    }
                                    mir::Instruction cloned = calleeInst;
                                    if (callee.moduleName != caller.moduleName ||
                                        callee.debugInfo.sourceFileId != caller.debugInfo.sourceFileId) {
                                        cloned.sourceSpan = inst.sourceSpan;
                                    }
                                    for (auto& op : cloned.operands) {
                                        const auto m = valueMap.find(op);
                                        if (m == valueMap.end()) {
                                            inlineOk = false;
                                            break;
                                        }
                                        op = m->second;
                                    }
                                    if (!inlineOk) break;
                                    if (isValueInstruction(cloned)) {
                                        const mir::ValueId fresh = nextId++;
                                        valueMap[calleeInst.result] = fresh;
                                        cloned.result = fresh;
                                    }
                                    stagedClones.push_back(std::move(cloned));
                                }
                                if (calleeBlock.terminator.kind == mir::TerminatorKind::ReturnVoid) {
                                    if (inlineOk && inst.resultType == semantic::PrimitiveType::Void) {
                                        for (auto& sc : stagedClones) {
                                            ensureTypeImported(sc.typeId);
                                            ensureTypeImported(sc.resultTypeId);
                                            ensureTypeImported(sc.elementTypeId);
                                            expanded.push_back(std::move(sc));
                                        }
                                        recordInlinedEdge(caller, callee);
                                        ++statistics.functionsInlined;
                                        ++inlinesInCaller;
                                        callerChanged = true;
                                        anyInlined = true;
                                        singleBlockDone = true;
                                    }
                                } else {
                                    const auto retVal = calleeBlock.terminator.value;
                                    const auto mappedRet = valueMap.find(retVal);
                                    if (inlineOk && mappedRet != valueMap.end()) {
                                        for (auto& sc : stagedClones) {
                                            ensureTypeImported(sc.typeId);
                                            ensureTypeImported(sc.resultTypeId);
                                            ensureTypeImported(sc.elementTypeId);
                                            expanded.push_back(std::move(sc));
                                        }
                                        replacements[inst.result] = mappedRet->second;
                                        recordInlinedEdge(caller, callee);
                                        ++statistics.functionsInlined;
                                        ++inlinesInCaller;
                                        callerChanged = true;
                                        anyInlined = true;
                                        singleBlockDone = true;
                                    }
                                }
                            }
                            if (singleBlockDone) {
                                continue;
                            }
                            if (callee.blocks.size() > 1 || isPriorityTarget) {
                                std::vector<mir::Instruction> rebuilt;
                                rebuilt.reserve(expanded.size() + (caller.blocks[bi].instructions.size() - ii));
                                for (auto& e : expanded) rebuilt.push_back(std::move(e));
                                const std::size_t newCallIdx = rebuilt.size();
                                for (std::size_t k = ii; k < caller.blocks[bi].instructions.size(); ++k) {
                                    auto& rem = caller.blocks[bi].instructions[k];
                                    for (auto& op : rem.operands) op = resolveVal(op);
                                    rebuilt.push_back(std::move(rem));
                                }
                                caller.blocks[bi].instructions = std::move(rebuilt);
                                if (inlineMultiBlockCfg(bi, newCallIdx, callee)) {
                                    ++inlinesInCaller;
                                    splitHappened = true;
                                    break;
                                }
                                expanded = std::move(caller.blocks[bi].instructions);
                                break;
                            }
                        }
                    }
                    expanded.push_back(std::move(inst));
                }
                if (!splitHappened) {
                    caller.blocks[bi].instructions = std::move(expanded);
                }
            }
        }
        if (!replacements.empty()) {
            for (auto& block : caller.blocks) {
                for (auto& inst : block.instructions) {
                    for (auto& op : inst.operands) op = resolveVal(op);
                }
                block.terminator.condition = resolveVal(block.terminator.condition);
                block.terminator.value = resolveVal(block.terminator.value);
                for (auto& arg : block.terminator.arguments) arg = resolveVal(arg);
                for (auto& arg : block.terminator.falseArguments) arg = resolveVal(arg);
            }
        }
        if (callerChanged) {
            compactValueIds(caller);
        }
    }
    return anyInlined;
}


bool devirtualizeTypeAssignable(
    semantic::SymbolId actual,
    semantic::SymbolId expected,
    const std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*>& types) noexcept {
    if (actual == expected) return true;
    if (actual == 0 || expected == 0) return false;
    auto current = actual;
    for (std::size_t depth = 0; current != 0 && depth < 256; ++depth) {
        const auto found = types.find(current);
        if (found == types.end()) return false;
        for (const auto& iface : found->second->interfaceDispatchMaps) {
            if (iface.interfaceTypeId == expected) return true;
        }
        if (found->second->baseTypeId == expected) return true;
        current = found->second->baseTypeId;
    }
    return false;
}

const semantic::FunctionSymbol* findClassMethodBySymbolId(
    semantic::SymbolId classTypeId,
    semantic::SymbolId methodSymbolId,
    const std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*>& types) noexcept {
    auto current = classTypeId;
    for (std::size_t depth = 0; current != 0 && depth < 256; ++depth) {
        const auto found = types.find(current);
        if (found == types.end()) return nullptr;
        for (const auto& method : found->second->methods) {
            if (method.id == methodSymbolId) return &method;
        }
        current = found->second->baseTypeId;
    }
    return nullptr;
}

bool devirtualizeCalls(
    mir::Module& module,
    const Options& options,
    Statistics& statistics,
    const std::vector<mir::Module>* allModules = nullptr) {
    if (!options.enableDevirtualization) return false;

    const auto initialModuleTypes = module.types;
    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> allTypes;
    std::unordered_map<semantic::SymbolId, const mir::Function*> allFunctions;
    std::unordered_set<semantic::SymbolId> existingModuleTypes;
    for (const auto& t : initialModuleTypes) {
        if (t.id != 0) {
            allTypes.emplace(t.id, &t);
            existingModuleTypes.insert(t.id);
        }
    }
    for (const auto& fn : module.functions) {
        if (fn.symbolId != 0) allFunctions.emplace(fn.symbolId, &fn);
    }
    if (allModules) {
        for (const auto& m : *allModules) {
            if (&m == &module) continue;
            for (const auto& t : m.types) {
                if (t.id != 0) allTypes.emplace(t.id, &t);
            }
            for (const auto& fn : m.functions) {
                if (fn.symbolId != 0) allFunctions.emplace(fn.symbolId, &fn);
            }
        }
    }

    const auto ensureTypeImported = [&](semantic::SymbolId rootTid) {
        if (rootTid == 0 || existingModuleTypes.count(rootTid)) return;
        std::vector<semantic::SymbolId> worklist{rootTid};
        while (!worklist.empty()) {
            const semantic::SymbolId tid = worklist.back();
            worklist.pop_back();
            if (tid == 0 || existingModuleTypes.count(tid)) continue;
            const auto it = allTypes.find(tid);
            if (it == allTypes.end() || !it->second) continue;
            const semantic::TypeSymbol& t = *it->second;
            module.types.push_back(t);
            existingModuleTypes.insert(tid);
            if (t.baseTypeId != 0 && !existingModuleTypes.count(t.baseTypeId)) {
                worklist.push_back(t.baseTypeId);
            }
            for (const auto& imap : t.interfaceDispatchMaps) {
                if (imap.interfaceTypeId != 0 &&
                    !existingModuleTypes.count(imap.interfaceTypeId)) {
                    worklist.push_back(imap.interfaceTypeId);
                }
            }
        }
    };

    bool anyDevirt = false;
    for (auto& caller : module.functions) {
        if (caller.blocks.empty()) continue;
        mir::ValueId nextId = findMaxValueId(caller) + 1;

        struct ValStaticType {
            semantic::PrimitiveType type = semantic::PrimitiveType::Error;
            semantic::SymbolId typeId = 0;
        };
        std::unordered_map<mir::ValueId, ValStaticType> staticTypes;
        bool entryHasPredecessors = false;
        bool entryProtectedByHandler = false;
        const mir::BlockId entryBlockId = caller.blocks.front().id;
        for (const auto& handler : caller.exceptionHandlers) {
            for (const auto pb : handler.protectedBlocks) {
                if (pb == entryBlockId) {
                    entryProtectedByHandler = true;
                    break;
                }
            }
            if (entryProtectedByHandler) break;
        }

        for (const auto& blk : caller.blocks) {
            for (const auto& p : blk.parameters) {
                if (p.value >= 0) {
                    staticTypes[p.value] = {p.type, p.typeId};
                }
            }
            for (const auto& inst : blk.instructions) {
                if (inst.result >= 0) {
                    const auto tid = inst.resultTypeId != 0
                        ? inst.resultTypeId
                        : (semantic::isExactType(inst.resultType) ? inst.typeId : 0);
                    staticTypes[inst.result] = {inst.resultType, tid};
                }
            }
            if (blk.terminator.kind == mir::TerminatorKind::Jump) {
                if (blk.terminator.target == entryBlockId) entryHasPredecessors = true;
            } else if (blk.terminator.kind == mir::TerminatorKind::Branch) {
                if (blk.terminator.target == entryBlockId ||
                    blk.terminator.falseTarget == entryBlockId) {
                    entryHasPredecessors = true;
                }
            }
        }
        const bool entryCanDominateAllBlocks =
            !entryHasPredecessors && !entryProtectedByHandler;

        struct ExactInfo {
            semantic::SymbolId exactTypeId = 0;
            mir::ValueId domExactValue = -1;
            mir::BlockId defBlockId = 0;
            bool definedInEntry = false;
            bool knownNonNull = false;
        };
        std::unordered_map<mir::ValueId, ExactInfo> exactMap;

        const auto isSealedConcreteClass = [&](semantic::SymbolId tid) noexcept -> bool {
            if (tid == 0) return false;
            const auto it = allTypes.find(tid);
            return it != allTypes.end() &&
                it->second->kind == semantic::TypeKind::Class &&
                !it->second->abstractType &&
                !it->second->interfaceType &&
                it->second->sealedType;
        };

        // Pass 1: seed SSA values from NewObject & sealed class types in entry & all blocks
        for (std::size_t bIdx = 0; bIdx < caller.blocks.size(); ++bIdx) {
            const auto& blk = caller.blocks[bIdx];
            const bool inEntry = (bIdx == 0 && entryCanDominateAllBlocks);
            for (const auto& p : blk.parameters) {
                if (p.value >= 0 && p.type == semantic::PrimitiveType::Object &&
                    isSealedConcreteClass(p.typeId)) {
                    exactMap[p.value] = {p.typeId, p.value, blk.id, inEntry, false};
                }
            }
            for (const auto& inst : blk.instructions) {
                if (inst.result < 0) continue;
                const auto stIt = staticTypes.find(inst.result);
                const auto stTid = stIt != staticTypes.end() ? stIt->second.typeId : 0;
                if (inst.opcode == mir::Opcode::NewObject && stTid != 0) {
                    const auto tIt = allTypes.find(stTid);
                    if (tIt != allTypes.end() &&
                        tIt->second->kind == semantic::TypeKind::Class &&
                        !tIt->second->abstractType &&
                        !tIt->second->interfaceType) {
                        exactMap[inst.result] = {stTid, inst.result, blk.id, inEntry, true};
                    }
                } else if (inst.opcode == mir::Opcode::CheckNotNull && !inst.operands.empty()) {
                    const auto opIt = exactMap.find(inst.operands[0]);
                    if (opIt != exactMap.end()) {
                        ExactInfo info = opIt->second;
                        info.knownNonNull = true;
                        exactMap[inst.result] = info;
                    } else if (isSealedConcreteClass(stTid)) {
                        exactMap[inst.result] = {stTid, inst.result, blk.id, inEntry, true};
                    } else {
                        exactMap[inst.result] = {0, -1, blk.id, inEntry, true};
                    }
                } else if (inst.resultType == semantic::PrimitiveType::Object &&
                           isSealedConcreteClass(stTid)) {
                    exactMap[inst.result] = {stTid, inst.result, blk.id, inEntry, false};
                }
            }
        }

        // Analyze StoreLocal across function for single-assignment entry locals & uniform exact-type locals
        const std::size_t localCount = caller.localTypes.size();
        std::vector<std::size_t> localStores(localCount, 0);
        std::vector<semantic::SymbolId> localUniformExact(localCount, 0);
        std::vector<bool> localUniformValid(localCount, true);
        std::vector<std::size_t> entryStoreInstIdx(localCount, std::numeric_limits<std::size_t>::max());
        std::vector<mir::ValueId> entryStoreRhs(localCount, -1);

        for (const auto& handler : caller.exceptionHandlers) {
            if (handler.exceptionLocal < localCount) {
                localStores[handler.exceptionLocal] += 2;
                localUniformValid[handler.exceptionLocal] = false;
            }
        }

        for (std::size_t iter = 0; iter < 2; ++iter) {
            std::fill(localStores.begin(), localStores.end(), 0);
            std::fill(localUniformExact.begin(), localUniformExact.end(), 0);
            std::fill(localUniformValid.begin(), localUniformValid.end(), true);
            std::fill(entryStoreInstIdx.begin(), entryStoreInstIdx.end(), std::numeric_limits<std::size_t>::max());
            std::fill(entryStoreRhs.begin(), entryStoreRhs.end(), -1);
            for (const auto& handler : caller.exceptionHandlers) {
                if (handler.exceptionLocal < localCount) {
                    localStores[handler.exceptionLocal] += 2;
                    localUniformValid[handler.exceptionLocal] = false;
                }
            }

            for (std::size_t bIdx = 0; bIdx < caller.blocks.size(); ++bIdx) {
                const auto& blk = caller.blocks[bIdx];
                for (std::size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
                    const auto& inst = blk.instructions[iIdx];
                    if (inst.opcode == mir::Opcode::StoreLocal &&
                        inst.localIndex < localCount &&
                        !inst.operands.empty()) {
                        const auto slot = inst.localIndex;
                        ++localStores[slot];
                        if (bIdx == 0 && !entryHasPredecessors && localStores[slot] == 1) {
                            entryStoreInstIdx[slot] = iIdx;
                            entryStoreRhs[slot] = inst.operands[0];
                        }
                        const auto exIt = exactMap.find(inst.operands[0]);
                        const semantic::SymbolId rhsExact =
                            (exIt != exactMap.end()) ? exIt->second.exactTypeId : 0;
                        if (rhsExact == 0) {
                            localUniformValid[slot] = false;
                        } else if (localUniformExact[slot] == 0) {
                            localUniformExact[slot] = rhsExact;
                        } else if (localUniformExact[slot] != rhsExact) {
                            localUniformValid[slot] = false;
                        }
                    }
                }
            }

            for (std::size_t bIdx = 0; bIdx < caller.blocks.size(); ++bIdx) {
                const auto& blk = caller.blocks[bIdx];
                const bool inEntry = (bIdx == 0 && entryCanDominateAllBlocks);
                std::unordered_map<std::size_t, mir::ValueId> blockLocalValue;
                for (std::size_t iIdx = 0; iIdx < blk.instructions.size(); ++iIdx) {
                    const auto& inst = blk.instructions[iIdx];
                    if (inst.opcode == mir::Opcode::StoreLocal &&
                        inst.localIndex < localCount &&
                        !inst.operands.empty()) {
                        blockLocalValue[inst.localIndex] = inst.operands[0];
                        continue;
                    }
                    if (inst.opcode == mir::Opcode::LoadLocal &&
                        inst.result >= 0 &&
                        inst.localIndex < localCount) {
                        const auto slot = inst.localIndex;
                        const auto blkLocalIt = blockLocalValue.find(slot);
                        if (blkLocalIt != blockLocalValue.end()) {
                            const auto rhsEx = exactMap.find(blkLocalIt->second);
                            if (rhsEx != exactMap.end()) {
                                exactMap[inst.result] = rhsEx->second;
                            } else {
                                exactMap.erase(inst.result);
                            }
                            continue;
                        }
                        if (localStores[slot] == 1 &&
                            entryStoreRhs[slot] >= 0 &&
                            ((bIdx > 0 && entryCanDominateAllBlocks) ||
                             (bIdx == 0 && !entryHasPredecessors && iIdx > entryStoreInstIdx[slot]))) {
                            const auto rhsEx = exactMap.find(entryStoreRhs[slot]);
                            if (rhsEx != exactMap.end()) {
                                ExactInfo prop = rhsEx->second;
                                if (!prop.definedInEntry) {
                                    prop.domExactValue = -1;
                                }
                                exactMap[inst.result] = prop;
                                continue;
                            }
                        }
                        if (localUniformValid[slot] && localUniformExact[slot] != 0) {
                            exactMap[inst.result] = {
                                localUniformExact[slot], -1, blk.id, inEntry, false};
                        }
                    } else if (inst.opcode == mir::Opcode::CheckNotNull &&
                               inst.result >= 0 &&
                               !inst.operands.empty()) {
                        const auto opIt = exactMap.find(inst.operands[0]);
                        if (opIt != exactMap.end()) {
                            ExactInfo info = opIt->second;
                            info.knownNonNull = true;
                            exactMap[inst.result] = info;
                        }
                    }
                }
            }
        }

        bool callerChanged = false;
        for (std::size_t bIdx = 0; bIdx < caller.blocks.size(); ++bIdx) {
            auto& blk = caller.blocks[bIdx];
            std::vector<mir::Instruction> rewritten;
            rewritten.reserve(blk.instructions.size() + 4);
            for (auto& inst : blk.instructions) {
                if (inst.opcode == mir::Opcode::Call &&
                    (inst.virtualDispatch || inst.interfaceDispatch) &&
                    !(inst.virtualDispatch && inst.interfaceDispatch) &&
                    !inst.operands.empty() &&
                    inst.operands.size() == inst.parameterTypes.size() &&
                    inst.parameterTypeIds.size() == inst.parameterTypes.size()) {
                    const mir::ValueId recvOp = inst.operands[0];
                    const auto recvStIt = staticTypes.find(recvOp);
                    const semantic::SymbolId recvStaticTid =
                        recvStIt != staticTypes.end() ? recvStIt->second.typeId : 0;
                    const auto exIt = exactMap.find(recvOp);
                    const ExactInfo exInfo = exIt != exactMap.end() ? exIt->second : ExactInfo{};

                    semantic::SymbolId resolvedSym = 0;
                    if (exInfo.exactTypeId != 0) {
                        const auto cIt = allTypes.find(exInfo.exactTypeId);
                        if (cIt != allTypes.end() &&
                            cIt->second->kind == semantic::TypeKind::Class &&
                            !cIt->second->abstractType &&
                            !cIt->second->interfaceType) {
                            if (inst.virtualDispatch) {
                                if (inst.virtualSlot < cIt->second->virtualDispatchTable.size()) {
                                    resolvedSym = cIt->second->virtualDispatchTable[inst.virtualSlot];
                                }
                            } else if (inst.interfaceDispatch) {
                                for (const auto& imap : cIt->second->interfaceDispatchMaps) {
                                    if (imap.interfaceTypeId == inst.interfaceTypeId) {
                                        if (inst.interfaceSlot < imap.slots.size()) {
                                            resolvedSym = imap.slots[inst.interfaceSlot];
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    } else if (inst.virtualDispatch && recvStaticTid != 0) {
                        const auto sIt = allTypes.find(recvStaticTid);
                        if (sIt != allTypes.end() &&
                            sIt->second->kind == semantic::TypeKind::Class &&
                            inst.virtualSlot < sIt->second->virtualDispatchTable.size()) {
                            const semantic::SymbolId candSym =
                                sIt->second->virtualDispatchTable[inst.virtualSlot];
                            if (candSym != 0) {
                                bool canDevirt = sIt->second->sealedType;
                                if (!canDevirt) {
                                    const auto* mSym = findClassMethodBySymbolId(
                                        recvStaticTid, candSym, allTypes);
                                    if (mSym && mSym->sealedMethod && !mSym->abstractMethod) {
                                        canDevirt = true;
                                    }
                                }
                                if (canDevirt) {
                                    resolvedSym = candSym;
                                }
                            }
                        }
                    }

                    if (resolvedSym != 0) {
                        const auto fnIt = allFunctions.find(resolvedSym);
                        if (fnIt != allFunctions.end()) {
                            const mir::Function& targetFn = *fnIt->second;
                            if (targetFn.parameterTypes.size() == inst.operands.size() &&
                                targetFn.parameterTypeIds.size() == targetFn.parameterTypes.size() &&
                                !targetFn.parameterTypeIds.empty() &&
                                targetFn.parameterTypes[0] == semantic::PrimitiveType::Object &&
                                targetFn.returnType == inst.resultType &&
                                targetFn.returnTypeId == inst.resultTypeId) {
                                const semantic::SymbolId expectedThisTid = targetFn.parameterTypeIds[0];
                                const auto expTypeIt = allTypes.find(expectedThisTid);
                                bool extraArgsCompatible = true;
                                for (std::size_t a = 1; a < inst.operands.size(); ++a) {
                                    const auto opSt = staticTypes.find(inst.operands[a]);
                                    if (opSt == staticTypes.end() ||
                                        opSt->second.type != targetFn.parameterTypes[a]) {
                                        extraArgsCompatible = false;
                                        break;
                                    }
                                    if (targetFn.parameterTypes[a] == semantic::PrimitiveType::Object) {
                                        if (targetFn.parameterTypeIds[a] != 0 &&
                                            !devirtualizeTypeAssignable(
                                                opSt->second.typeId,
                                                targetFn.parameterTypeIds[a],
                                                allTypes)) {
                                            extraArgsCompatible = false;
                                            break;
                                        }
                                    } else if (opSt->second.typeId != targetFn.parameterTypeIds[a]) {
                                        extraArgsCompatible = false;
                                        break;
                                    }
                                }
                                if (extraArgsCompatible &&
                                    expTypeIt != allTypes.end() &&
                                    expTypeIt->second->kind == semantic::TypeKind::Class) {
                                    mir::ValueId chosenRecv = -1;
                                    bool chosenNonNull = exInfo.knownNonNull;
                                    if (devirtualizeTypeAssignable(recvStaticTid, expectedThisTid, allTypes)) {
                                        chosenRecv = recvOp;
                                    } else if (exInfo.domExactValue >= 0 &&
                                               (exInfo.definedInEntry || exInfo.defBlockId == blk.id)) {
                                        const auto domSt = staticTypes.find(exInfo.domExactValue);
                                        if (domSt != staticTypes.end() &&
                                            devirtualizeTypeAssignable(
                                                domSt->second.typeId, expectedThisTid, allTypes)) {
                                            chosenRecv = exInfo.domExactValue;
                                            const auto domEx = exactMap.find(exInfo.domExactValue);
                                            if (domEx != exactMap.end() && domEx->second.knownNonNull) {
                                                chosenNonNull = true;
                                            }
                                        }
                                    } else if (targetFn.blocks.size() == 1 &&
                                               targetFn.exceptionHandlers.empty() &&
                                               targetFn.blocks.front().instructions.size() <=
                                                   options.maxInlineInstructions + 4) {
                                        ensureTypeImported(expectedThisTid);
                                        mir::Instruction castInst;
                                        castInst.opcode = mir::Opcode::AsType;
                                        castInst.result = nextId++;
                                        castInst.resultType = semantic::PrimitiveType::Object;
                                        castInst.resultTypeId = expectedThisTid;
                                        castInst.elementType = semantic::PrimitiveType::Object;
                                        castInst.elementTypeId = expectedThisTid;
                                        castInst.operands = {recvOp};
                                        castInst.sourceSpan = inst.sourceSpan;
                                        chosenRecv = castInst.result;
                                        staticTypes[chosenRecv] = {
                                            semantic::PrimitiveType::Object, expectedThisTid};
                                        rewritten.push_back(std::move(castInst));
                                    }

                                    if (chosenRecv >= 0) {
                                        ensureTypeImported(expectedThisTid);
                                        const auto chosenSt = staticTypes.find(chosenRecv);
                                        if (chosenSt != staticTypes.end() && chosenSt->second.typeId != 0) {
                                            ensureTypeImported(chosenSt->second.typeId);
                                        }
                                        if (!chosenNonNull) {
                                            mir::Instruction chk;
                                            chk.opcode = mir::Opcode::CheckNotNull;
                                            chk.result = nextId++;
                                            chk.resultType = semantic::PrimitiveType::Object;
                                            chk.resultTypeId = expectedThisTid;
                                            chk.typeId = expectedThisTid;
                                            chk.operands = {chosenRecv};
                                            chk.sourceSpan = inst.sourceSpan;
                                            chosenRecv = chk.result;
                                            staticTypes[chosenRecv] = {
                                                semantic::PrimitiveType::Object, expectedThisTid};
                                            rewritten.push_back(std::move(chk));
                                        }
                                        inst.operands[0] = chosenRecv;
                                        inst.symbolId = targetFn.symbolId;
                                        inst.symbolName = targetFn.moduleName + "::" + targetFn.name;
                                        inst.parameterTypes = targetFn.parameterTypes;
                                        inst.parameterTypeIds = targetFn.parameterTypeIds;
                                        inst.virtualDispatch = false;
                                        inst.virtualSlot = std::numeric_limits<std::uint32_t>::max();
                                        inst.interfaceDispatch = false;
                                        inst.interfaceTypeId = 0;
                                        inst.interfaceSlot = std::numeric_limits<std::uint32_t>::max();
                                        ++statistics.callsDevirtualized;
                                        callerChanged = true;
                                        anyDevirt = true;
                                    }
                                }
                            }
                        }
                    }
                }
                rewritten.push_back(std::move(inst));
            }
            blk.instructions = std::move(rewritten);
        }
        if (callerChanged) {
            compactValueIds(caller);
        }
    }
    return anyDevirt;
}

bool optimizeFunction(
    mir::Function& function,
    const Options& options,
    Statistics& statistics) {
    ++statistics.functionsVisited;
    if (options.level == Level::None) return false;
    bool any = false;
    const auto iterations = std::max<std::size_t>(1, options.maximumIterations);
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        bool changed = false;
        changed |= foldConstants(
            function,
            statistics,
            options.level == Level::Aggressive && options.enableStructFieldForwarding);
        changed |= removeUnreachableBlocks(function, statistics);
        if (options.level == Level::Aggressive) {
            if (options.enableLicm) {
                changed |= hoistLoopInvariants(function, statistics);
            }
            changed |= eliminateDeadInstructions(function, statistics);
        }
        ++statistics.iterations;
        any |= changed;
        if (!changed) break;
    }
    if (any) compactValueIds(function);
    return any;
}

} // namespace

bool Statistics::changed() const noexcept {
    return constantsFolded != 0 || branchesFolded != 0 ||
        instructionsRemoved != 0 || blocksRemoved != 0 ||
        localLoadsFolded != 0 || functionsInlined != 0 ||
        loopInvariantsHoisted != 0 || structFieldsForwarded != 0 ||
        callsDevirtualized != 0;
}

Result Optimizer::optimize(
    std::vector<mir::Module> modules,
    diagnostics::DiagnosticBag& diagnostics,
    Options options) const {
    Result result;
    Options innerOptions = options;
    if (options.level == Level::Aggressive &&
        (options.enableInlining || options.enableDevirtualization) &&
        modules.size() > 1) {
        for (std::size_t inlinePass = 0; inlinePass < 3; ++inlinePass) {
            const auto snapshot = modules;
            bool anyPassChanged = false;
            for (auto& module : modules) {
                diagnostics::DiagnosticBag checkDiag;
                if (mir::verifyModule(module, checkDiag)) {
                    if (options.enableDevirtualization &&
                        devirtualizeCalls(module, options, result.statistics, &snapshot)) {
                        anyPassChanged = true;
                    }
                    if (options.enableInlining &&
                        inlineSmallFunctions(module, options, result.statistics, &snapshot)) {
                        anyPassChanged = true;
                    }
                }
            }
            if (!anyPassChanged) break;
        }
        innerOptions.enableInlining = false;
        innerOptions.enableDevirtualization = false;
    }
    result.modules.reserve(modules.size());
    for (auto& module : modules) {
        result.modules.push_back(optimizeModule(
            std::move(module), diagnostics, innerOptions, &result.statistics));
    }
    return result;
}

mir::Module Optimizer::optimizeModule(
    mir::Module module,
    diagnostics::DiagnosticBag& diagnostics,
    Options options,
    Statistics* statistics) const {
    Statistics local;
    auto& target = statistics ? *statistics : local;
    if (options.level != Level::None) {
        diagnostics::DiagnosticBag inputDiagnostics;
        if (!mir::verifyModule(module, inputDiagnostics)) {
            diagnostics.report(
                "RS6000",
                "optimizer rejected invalid input MIR module '" + module.name + "'",
                {});
            diagnostics.append(inputDiagnostics);
            return module;
        }
        if (options.level == Level::Aggressive &&
            (options.enableInlining || options.enableDevirtualization)) {
            for (std::size_t inlinePass = 0; inlinePass < 3; ++inlinePass) {
                bool passChanged = false;
                if (options.enableDevirtualization) {
                    passChanged |= devirtualizeCalls(module, options, target);
                }
                if (options.enableInlining) {
                    passChanged |= inlineSmallFunctions(module, options, target);
                }
                if (!passChanged) break;
            }
        }
        for (auto& function : module.functions) {
            (void)optimizeFunction(function, options, target);
            refreshDebugInfo(function, module.sourceFiles, options.preserveDebugInfo);
        }
        diagnostics::DiagnosticBag outputDiagnostics;
        if (!mir::verifyModule(module, outputDiagnostics)) {
            diagnostics.report(
                "RS6001",
                "optimizer produced invalid MIR module '" + module.name + "'",
                {});
            diagnostics.append(outputDiagnostics);
        }
    }
    return module;
}

const char* levelName(Level level) noexcept {
    switch (level) {
    case Level::None: return "O0";
    case Level::Basic: return "O1";
    case Level::Aggressive: return "O2";
    }
    return "O0";
}

std::string formatStatistics(const Statistics& statistics) {
    std::ostringstream out;
    out << "functions=" << statistics.functionsVisited
        << " iterations=" << statistics.iterations
        << " constants-folded=" << statistics.constantsFolded
        << " local-loads-folded=" << statistics.localLoadsFolded
        << " branches-folded=" << statistics.branchesFolded
        << " instructions-removed=" << statistics.instructionsRemoved
        << " blocks-removed=" << statistics.blocksRemoved;
    return out.str();
}

} // namespace realscript::optimization
