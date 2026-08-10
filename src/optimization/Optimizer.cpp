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
        instruction.opcode == mir::Opcode::StoreStructField) {
        return false;
    }
    return (instruction.opcode != mir::Opcode::Call &&
            instruction.opcode != mir::Opcode::InvokeDelegate) ||
        instruction.resultType != semantic::PrimitiveType::Void;
}

bool isRemovableWhenUnused(mir::Opcode opcode) noexcept {
    switch (opcode) {
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
        return true;
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

bool foldConstants(mir::Function& function, Statistics& statistics) {
    bool changed = false;
    std::unordered_map<mir::ValueId, Constant> constants;
    for (auto& block : function.blocks) {
        std::unordered_map<std::size_t, Constant> localConstants;
        for (auto& instruction : block.instructions) {
            if (instruction.opcode == mir::Opcode::StoreLocal &&
                instruction.operands.size() == 1) {
                const auto found = constants.find(instruction.operands.front());
                if (found != constants.end()) {
                    localConstants[instruction.localIndex] = found->second;
                } else {
                    localConstants.erase(instruction.localIndex);
                }
                continue;
            }
            if (instruction.opcode == mir::Opcode::Call ||
                instruction.opcode == mir::Opcode::StoreField ||
                instruction.opcode == mir::Opcode::StoreElement ||
                instruction.opcode == mir::Opcode::StoreStructField) {
                localConstants.clear();
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
            if (!isRemovableWhenUnused(instruction.opcode)) {
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
                        isRemovableWhenUnused(instruction.opcode) &&
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
        changed |= foldConstants(function, statistics);
        changed |= removeUnreachableBlocks(function, statistics);
        if (options.level == Level::Aggressive) {
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
        localLoadsFolded != 0;
}

Result Optimizer::optimize(
    std::vector<mir::Module> modules,
    diagnostics::DiagnosticBag& diagnostics,
    Options options) const {
    Result result;
    result.modules.reserve(modules.size());
    for (auto& module : modules) {
        result.modules.push_back(optimizeModule(
            std::move(module), diagnostics, options, &result.statistics));
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
