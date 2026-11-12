#include "AotCppInternal.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace realscript::aot::detail {

namespace {

constexpr std::int64_t TypedIntMinimum =
    std::numeric_limits<std::int32_t>::min();
constexpr std::int64_t TypedIntMaximum =
    std::numeric_limits<std::int32_t>::max();

struct TypedIntRange {
    std::int64_t minimum = TypedIntMinimum;
    std::int64_t maximum = TypedIntMaximum;
};

struct TypedLocalOrigin {
    std::size_t slot = std::numeric_limits<std::size_t>::max();
    std::uint64_t version = 0;

    [[nodiscard]] bool valid() const noexcept {
        return slot != std::numeric_limits<std::size_t>::max();
    }
};

struct TypedComparison {
    mir::Opcode opcode = mir::Opcode::LessInt;
    TypedIntRange left;
    TypedIntRange right;
    TypedLocalOrigin leftOrigin;
    TypedLocalOrigin rightOrigin;
};


TypedIntRange typedFullIntRange() noexcept {
    return {TypedIntMinimum, TypedIntMaximum};
}

TypedIntRange typedValueRange(
    const std::vector<TypedIntRange>& values,
    mir::ValueId value) noexcept {
    if (value < 0 || static_cast<std::size_t>(value) >= values.size()) {
        return typedFullIntRange();
    }
    return values[static_cast<std::size_t>(value)];
}

TypedLocalOrigin typedValueOrigin(
    const std::vector<TypedLocalOrigin>& origins,
    mir::ValueId value) noexcept {
    if (value < 0 || static_cast<std::size_t>(value) >= origins.size()) {
        return {};
    }
    return origins[static_cast<std::size_t>(value)];
}


} // namespace

TypedRangeAnalysis analyzeTypedIntegerRanges(const mir::Function& function) {
    TypedRangeAnalysis analysis;
    if (function.blocks.empty()) return analysis;

    const auto valueCount = registerCount(function);
    std::unordered_map<mir::BlockId, std::size_t> blockIndices;
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        blockIndices.emplace(function.blocks[index].id, index);
    }
    const auto entry = blockIndices.find(0);
    if (entry == blockIndices.end()) return analysis;

    std::set<std::int64_t> thresholdSet{
        TypedIntMinimum, 0, TypedIntMaximum};
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != mir::Opcode::ConstantInt) continue;
            const auto value = std::clamp(
                instruction.integerImmediate,
                TypedIntMinimum,
                TypedIntMaximum);
            thresholdSet.insert(value);
            if (value > TypedIntMinimum) thresholdSet.insert(value - 1);
            if (value < TypedIntMaximum) thresholdSet.insert(value + 1);
        }
    }
    const std::vector<std::int64_t> wideningThresholds(
        thresholdSet.begin(), thresholdSet.end());

    std::vector<std::vector<TypedIntRange>> entryLocals(
        function.blocks.size(),
        std::vector<TypedIntRange>(
            function.localTypes.size(), typedFullIntRange()));
    std::vector<bool> hasEntry(function.blocks.size(), false);
    std::vector<std::vector<std::uint8_t>> lowerUpdates(
        function.blocks.size(),
        std::vector<std::uint8_t>(function.localTypes.size(), 0));
    std::vector<std::vector<std::uint8_t>> upperUpdates(
        function.blocks.size(),
        std::vector<std::uint8_t>(function.localTypes.size(), 0));
    std::vector<std::size_t> worklist;
    std::vector<bool> queued(function.blocks.size(), false);
    std::size_t workIndex = 0;

    auto& initial = entryLocals[entry->second];
    for (std::size_t slot = 0; slot < function.localTypes.size(); ++slot) {
        if (function.localTypes[slot] == semantic::PrimitiveType::Int) {
            initial[slot] = {0, 0};
        }
    }
    hasEntry[entry->second] = true;
    worklist.push_back(entry->second);
    queued[entry->second] = true;

    std::set<mir::ValueId> evaluatedArithmetic;
    std::set<mir::ValueId> unsafeArithmetic;

    const auto enqueue = [&](std::size_t index) {
        if (!queued[index]) {
            queued[index] = true;
            worklist.push_back(index);
        }
    };
    const auto mergeEntry = [&](mir::BlockId target,
                                const std::vector<TypedIntRange>& incoming) {
        const auto found = blockIndices.find(target);
        if (found == blockIndices.end()) return;
        const auto index = found->second;
        if (!hasEntry[index]) {
            entryLocals[index] = incoming;
            hasEntry[index] = true;
            enqueue(index);
            return;
        }
        bool changed = false;
        for (std::size_t slot = 0; slot < incoming.size(); ++slot) {
            if (function.localTypes[slot] != semantic::PrimitiveType::Int) continue;
            auto& current = entryLocals[index][slot];
            auto minimum = std::min(current.minimum, incoming[slot].minimum);
            auto maximum = std::max(current.maximum, incoming[slot].maximum);
            if (minimum < current.minimum) {
                auto& updates = lowerUpdates[index][slot];
                if (updates != std::numeric_limits<std::uint8_t>::max()) ++updates;
                if (updates > 8) {
                    const auto threshold = std::upper_bound(
                        wideningThresholds.begin(),
                        wideningThresholds.end(),
                        minimum);
                    minimum = threshold == wideningThresholds.begin()
                        ? TypedIntMinimum
                        : *std::prev(threshold);
                }
            }
            if (maximum > current.maximum) {
                auto& updates = upperUpdates[index][slot];
                if (updates != std::numeric_limits<std::uint8_t>::max()) ++updates;
                if (updates > 8) {
                    const auto threshold = std::lower_bound(
                        wideningThresholds.begin(),
                        wideningThresholds.end(),
                        maximum);
                    maximum = threshold == wideningThresholds.end()
                        ? TypedIntMaximum
                        : *threshold;
                }
            }
            if (minimum != current.minimum || maximum != current.maximum) {
                current = {minimum, maximum};
                changed = true;
            }
        }
        if (changed) enqueue(index);
    };

    while (workIndex < worklist.size()) {
        const auto blockIndex = worklist[workIndex++];
        queued[blockIndex] = false;
        const auto& block = function.blocks[blockIndex];
        auto locals = entryLocals[blockIndex];
        std::vector<std::uint64_t> localVersions(function.localTypes.size(), 0);
        std::vector<TypedIntRange> values(
            valueCount, typedFullIntRange());
        std::vector<TypedLocalOrigin> origins(valueCount);
        std::unordered_map<mir::ValueId, TypedComparison> comparisons;

        const auto setValue = [&](mir::ValueId value,
                                  TypedIntRange range,
                                  TypedLocalOrigin origin = {}) {
            if (value < 0 || static_cast<std::size_t>(value) >= values.size()) {
                return;
            }
            values[static_cast<std::size_t>(value)] = range;
            origins[static_cast<std::size_t>(value)] = origin;
        };
        const auto noteArithmetic = [&](const mir::Instruction& instruction,
                                        bool safe) {
            if (!instruction.checkedArithmetic || instruction.result < 0) return;
            evaluatedArithmetic.insert(instruction.result);
            if (!safe) unsafeArithmetic.insert(instruction.result);
        };

        for (const auto& instruction : block.instructions) {
            const auto operand = [&](std::size_t index) {
                return index < instruction.operands.size()
                    ? typedValueRange(values, instruction.operands[index])
                    : typedFullIntRange();
            };
            switch (instruction.opcode) {
            case mir::Opcode::Parameter:
                setValue(instruction.result, typedFullIntRange());
                break;
            case mir::Opcode::ConstantInt:
                setValue(instruction.result,
                    {instruction.integerImmediate, instruction.integerImmediate});
                break;
            case mir::Opcode::LoadLocal:
                if (instruction.localIndex < locals.size()) {
                    setValue(instruction.result, locals[instruction.localIndex],
                        {instruction.localIndex,
                         localVersions[instruction.localIndex]});
                }
                break;
            case mir::Opcode::StoreLocal:
                if (instruction.localIndex < locals.size() &&
                    !instruction.operands.empty()) {
                    locals[instruction.localIndex] = operand(0);
                    ++localVersions[instruction.localIndex];
                }
                break;
            case mir::Opcode::AddInt:
            case mir::Opcode::SubtractInt: {
                const auto left = operand(0);
                const auto right = operand(1);
                const auto minimum = instruction.opcode == mir::Opcode::AddInt
                    ? left.minimum + right.minimum
                    : left.minimum - right.maximum;
                const auto maximum = instruction.opcode == mir::Opcode::AddInt
                    ? left.maximum + right.maximum
                    : left.maximum - right.minimum;
                const auto safe = minimum >= TypedIntMinimum &&
                    maximum <= TypedIntMaximum;
                noteArithmetic(instruction, safe);
                setValue(instruction.result,
                    safe ? TypedIntRange{minimum, maximum} : typedFullIntRange());
                break;
            }
            case mir::Opcode::MultiplyInt: {
                const auto left = operand(0);
                const auto right = operand(1);
                const std::int64_t candidates[] = {
                    left.minimum * right.minimum,
                    left.minimum * right.maximum,
                    left.maximum * right.minimum,
                    left.maximum * right.maximum,
                };
                const auto bounds = std::minmax_element(
                    std::begin(candidates), std::end(candidates));
                const auto safe = *bounds.first >= TypedIntMinimum &&
                    *bounds.second <= TypedIntMaximum;
                noteArithmetic(instruction, safe);
                setValue(instruction.result,
                    safe ? TypedIntRange{*bounds.first, *bounds.second}
                         : typedFullIntRange());
                break;
            }
            case mir::Opcode::NegateInt: {
                const auto value = operand(0);
                const auto safe = value.minimum > TypedIntMinimum;
                noteArithmetic(instruction, safe);
                setValue(instruction.result,
                    safe ? TypedIntRange{-value.maximum, -value.minimum}
                         : typedFullIntRange());
                break;
            }
            case mir::Opcode::LessInt:
            case mir::Opcode::LessOrEqualInt:
            case mir::Opcode::GreaterInt:
            case mir::Opcode::GreaterOrEqualInt:
                if (instruction.result >= 0 && instruction.operands.size() >= 2) {
                    comparisons.emplace(instruction.result, TypedComparison{
                        instruction.opcode,
                        operand(0),
                        operand(1),
                        typedValueOrigin(origins, instruction.operands[0]),
                        typedValueOrigin(origins, instruction.operands[1]),
                    });
                }
                break;
            default:
                if (instruction.result >= 0 &&
                    instruction.resultType == semantic::PrimitiveType::Int) {
                    setValue(instruction.result, typedFullIntRange());
                }
                break;
            }
        }

        const auto refinedLocals = [&](const TypedComparison& comparison,
                                       bool takeTrue) {
            auto result = locals;
            bool reachable = true;
            const auto refineMinimum = [&](TypedLocalOrigin origin,
                                           std::int64_t minimum) {
                if (!origin.valid() || origin.slot >= result.size() ||
                    origin.version != localVersions[origin.slot]) return;
                result[origin.slot].minimum =
                    std::max(result[origin.slot].minimum, minimum);
                reachable &= result[origin.slot].minimum <=
                    result[origin.slot].maximum;
            };
            const auto refineMaximum = [&](TypedLocalOrigin origin,
                                           std::int64_t maximum) {
                if (!origin.valid() || origin.slot >= result.size() ||
                    origin.version != localVersions[origin.slot]) return;
                result[origin.slot].maximum =
                    std::min(result[origin.slot].maximum, maximum);
                reachable &= result[origin.slot].minimum <=
                    result[origin.slot].maximum;
            };
            switch (comparison.opcode) {
            case mir::Opcode::LessInt:
                if (takeTrue) {
                    refineMaximum(comparison.leftOrigin,
                        comparison.right.maximum - 1);
                    refineMinimum(comparison.rightOrigin,
                        comparison.left.minimum + 1);
                } else {
                    refineMinimum(comparison.leftOrigin,
                        comparison.right.minimum);
                    refineMaximum(comparison.rightOrigin,
                        comparison.left.maximum);
                }
                break;
            case mir::Opcode::LessOrEqualInt:
                if (takeTrue) {
                    refineMaximum(comparison.leftOrigin,
                        comparison.right.maximum);
                    refineMinimum(comparison.rightOrigin,
                        comparison.left.minimum);
                } else {
                    refineMinimum(comparison.leftOrigin,
                        comparison.right.minimum + 1);
                    refineMaximum(comparison.rightOrigin,
                        comparison.left.maximum - 1);
                }
                break;
            case mir::Opcode::GreaterInt:
                if (takeTrue) {
                    refineMinimum(comparison.leftOrigin,
                        comparison.right.minimum + 1);
                    refineMaximum(comparison.rightOrigin,
                        comparison.left.maximum - 1);
                } else {
                    refineMaximum(comparison.leftOrigin,
                        comparison.right.maximum);
                    refineMinimum(comparison.rightOrigin,
                        comparison.left.minimum);
                }
                break;
            case mir::Opcode::GreaterOrEqualInt:
                if (takeTrue) {
                    refineMinimum(comparison.leftOrigin,
                        comparison.right.minimum);
                    refineMaximum(comparison.rightOrigin,
                        comparison.left.maximum);
                } else {
                    refineMaximum(comparison.leftOrigin,
                        comparison.right.maximum - 1);
                    refineMinimum(comparison.rightOrigin,
                        comparison.left.minimum + 1);
                }
                break;
            default:
                break;
            }
            return std::pair{reachable, std::move(result)};
        };

        if (block.terminator.kind == mir::TerminatorKind::Jump) {
            mergeEntry(block.terminator.target, locals);
        } else if (block.terminator.kind == mir::TerminatorKind::Branch) {
            const auto comparison = comparisons.find(block.terminator.condition);
            if (comparison == comparisons.end()) {
                mergeEntry(block.terminator.target, locals);
                mergeEntry(block.terminator.falseTarget, locals);
            } else {
                auto trueState = refinedLocals(comparison->second, true);
                auto falseState = refinedLocals(comparison->second, false);
                if (trueState.first) {
                    mergeEntry(block.terminator.target, trueState.second);
                }
                if (falseState.first) {
                    mergeEntry(block.terminator.falseTarget, falseState.second);
                }
            }
        }
    }

    for (const auto value : evaluatedArithmetic) {
        if (unsafeArithmetic.find(value) == unsafeArithmetic.end()) {
            analysis.safeCheckedArithmetic.insert(value);
        }
    }
    return analysis;
}


} // namespace realscript::aot::detail
