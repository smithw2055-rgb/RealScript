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

bool isPureVectorizableBodyInstruction(const mir::Instruction& instruction) noexcept {
    switch (instruction.opcode) {
    case mir::Opcode::ConstantInt:
    case mir::Opcode::ConstantDouble:
    case mir::Opcode::ConstantBool:
    case mir::Opcode::LoadLocal:
    case mir::Opcode::StoreLocal:
    case mir::Opcode::ConvertIntToLong:
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt:
    case mir::Opcode::NegateInt:
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong:
    case mir::Opcode::NegateLong:
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble:
    case mir::Opcode::NegateDouble:
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
    case mir::Opcode::NewStruct:
    case mir::Opcode::LoadStructField:
    case mir::Opcode::StoreStructField:
        return true;
    default:
        return false;
    }
}


} // namespace

std::unordered_map<mir::BlockId, VectorizedEcsLoopPlan> analyzeVectorizedEcsLoops(
    const mir::Function& function,
    const std::unordered_map<std::size_t, semantic::SymbolId>& arrayLocalStructElemType) {
    std::unordered_map<mir::BlockId, VectorizedEcsLoopPlan> plans;
    std::unordered_map<mir::ValueId, std::int64_t> constantInts;
    for (const auto& blk : function.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.opcode == mir::Opcode::ConstantInt &&
                inst.result >= 0 &&
                inst.resultType != semantic::PrimitiveType::Enum) {
                constantInts[inst.result] = inst.integerImmediate;
            }
        }
    }

    for (const auto& headerBlock : function.blocks) {
        if (headerBlock.instructions.size() != 4 ||
            headerBlock.terminator.kind != mir::TerminatorKind::Branch ||
            !headerBlock.terminator.arguments.empty() ||
            !headerBlock.terminator.falseArguments.empty()) {
            continue;
        }
        const auto& idxLoad = headerBlock.instructions[0];
        if (idxLoad.opcode != mir::Opcode::LoadLocal ||
            idxLoad.result < 0 ||
            idxLoad.localIndex >= function.localTypes.size() ||
            function.localTypes[idxLoad.localIndex] != semantic::PrimitiveType::Int) {
            continue;
        }

        VectorizedEcsLoopPlan plan;
        plan.headerBlockId = headerBlock.id;
        plan.indexLocalIndex = idxLoad.localIndex;
        plan.exitBlockId = headerBlock.terminator.falseTarget;
        const mir::ValueId indexVal = idxLoad.result;

        const mir::BasicBlock* curBlock = &headerBlock;
        std::size_t instOffset = 1;
        bool chainValid = true;
        while (curBlock) {
            if (curBlock->instructions.size() != instOffset + 3 ||
                curBlock->terminator.kind != mir::TerminatorKind::Branch ||
                curBlock->terminator.falseTarget != plan.exitBlockId ||
                !curBlock->terminator.arguments.empty() ||
                !curBlock->terminator.falseArguments.empty()) {
                chainValid = false;
                break;
            }
            const auto& colLoad = curBlock->instructions[instOffset + 0];
            const auto& lenInst = curBlock->instructions[instOffset + 1];
            const auto& cmpInst = curBlock->instructions[instOffset + 2];
            if (colLoad.opcode != mir::Opcode::LoadLocal ||
                colLoad.result < 0 ||
                colLoad.localIndex >= function.localTypes.size() ||
                function.localTypes[colLoad.localIndex] != semantic::PrimitiveType::Array ||
                lenInst.opcode != mir::Opcode::ArrayLength ||
                lenInst.operands.size() != 1 ||
                lenInst.operands[0] != colLoad.result ||
                lenInst.result < 0 ||
                cmpInst.opcode != mir::Opcode::LessInt ||
                cmpInst.operands.size() != 2 ||
                cmpInst.operands[0] != indexVal ||
                cmpInst.operands[1] != lenInst.result ||
                cmpInst.result != curBlock->terminator.condition) {
                chainValid = false;
                break;
            }
            plan.columns.push_back(VectorizedEcsColumnBinding{
                colLoad.localIndex, 0, 0, false});
            if (plan.columns.size() > 16) {
                chainValid = false;
                break;
            }
            const auto* nextBlock = findBlock(function, curBlock->terminator.target);
            if (!nextBlock) {
                chainValid = false;
                break;
            }
            if (nextBlock->instructions.size() == 3 &&
                nextBlock->instructions[0].opcode == mir::Opcode::LoadLocal &&
                nextBlock->instructions[1].opcode == mir::Opcode::ArrayLength &&
                nextBlock->instructions[2].opcode == mir::Opcode::LessInt &&
                nextBlock->terminator.kind == mir::TerminatorKind::Branch &&
                nextBlock->terminator.falseTarget == plan.exitBlockId) {
                curBlock = nextBlock;
                instOffset = 0;
                continue;
            }
            plan.bodyBlockId = nextBlock->id;
            break;
        }
        if (!chainValid || plan.columns.empty()) continue;

        const auto* bodyBlock = findBlock(function, plan.bodyBlockId);
        if (!bodyBlock ||
            bodyBlock->terminator.kind != mir::TerminatorKind::Jump ||
            !bodyBlock->terminator.arguments.empty()) {
            continue;
        }
        plan.incrementBlockId = bodyBlock->terminator.target;
        plan.bodyPreambleCount = 4 * plan.columns.size();
        if (bodyBlock->instructions.size() < plan.bodyPreambleCount) continue;

        bool preambleValid = true;
        std::set<std::size_t> usedLocals{plan.indexLocalIndex};
        std::set<semantic::SymbolId> usedElemTypes;
        for (std::size_t c = 0; c < plan.columns.size(); ++c) {
            if (!usedLocals.insert(plan.columns[c].colLocalIndex).second) {
                preambleValid = false;
                break;
            }
            const std::size_t base = 4 * c;
            const auto& b0 = bodyBlock->instructions[base + 0];
            const auto& b1 = bodyBlock->instructions[base + 1];
            const auto& b2 = bodyBlock->instructions[base + 2];
            const auto& b3 = bodyBlock->instructions[base + 3];
            if (b0.opcode != mir::Opcode::LoadLocal ||
                b0.localIndex != plan.columns[c].colLocalIndex ||
                b0.result < 0 ||
                b1.opcode != mir::Opcode::LoadLocal ||
                b1.localIndex != plan.indexLocalIndex ||
                b1.result < 0 ||
                b2.opcode != mir::Opcode::LoadElement ||
                b2.operands.size() != 2 ||
                b2.operands[0] != b0.result ||
                b2.operands[1] != b1.result ||
                b2.result < 0 ||
                b2.elementType != semantic::PrimitiveType::Struct ||
                b2.elementTypeId == 0 ||
                classifyInlineMath(b2.elementType, b2.elementTypeId) != InlineMathKind::None ||
                b3.opcode != mir::Opcode::StoreLocal ||
                b3.operands.size() != 1 ||
                b3.operands[0] != b2.result ||
                b3.localIndex >= function.localTypes.size() ||
                function.localTypes[b3.localIndex] != semantic::PrimitiveType::Struct) {
                preambleValid = false;
                break;
            }
            const auto elemTypeIt = arrayLocalStructElemType.find(plan.columns[c].colLocalIndex);
            if (elemTypeIt == arrayLocalStructElemType.end() ||
                elemTypeIt->second != b2.elementTypeId ||
                !usedElemTypes.insert(b2.elementTypeId).second ||
                !usedLocals.insert(b3.localIndex).second) {
                preambleValid = false;
                break;
            }
            plan.columns[c].iterLocalIndex = b3.localIndex;
            plan.columns[c].elemTypeId = b2.elementTypeId;
        }
        if (!preambleValid) continue;

        bool bodyValid = true;
        for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
            const auto& inst = bodyBlock->instructions[i];
            if (!isPureVectorizableBodyInstruction(inst)) {
                bodyValid = false;
                break;
            }
            if (inst.opcode == mir::Opcode::StoreLocal) {
                if (inst.localIndex == plan.indexLocalIndex) {
                    bodyValid = false;
                    break;
                }
                for (const auto& col : plan.columns) {
                    if (inst.localIndex == col.colLocalIndex) {
                        bodyValid = false;
                        break;
                    }
                }
                if (!bodyValid) break;
            } else if (inst.opcode == mir::Opcode::LoadLocal &&
                       inst.localIndex == plan.indexLocalIndex) {
                plan.usesIndexInsideBody = true;
            }
        }
        if (!bodyValid) continue;

        const auto* incBlock = findBlock(function, plan.incrementBlockId);
        if (!incBlock ||
            incBlock->terminator.kind != mir::TerminatorKind::Jump ||
            incBlock->terminator.target != plan.headerBlockId ||
            !incBlock->terminator.arguments.empty()) {
            continue;
        }

        std::size_t incPos = 0;
        bool incValid = true;
        while (incPos + 4 <= incBlock->instructions.size() &&
               incBlock->instructions[incPos].opcode == mir::Opcode::LoadLocal &&
               incBlock->instructions[incPos].localIndex < function.localTypes.size() &&
               function.localTypes[incBlock->instructions[incPos].localIndex] ==
                   semantic::PrimitiveType::Array) {
            const auto& i0 = incBlock->instructions[incPos + 0];
            const auto& i1 = incBlock->instructions[incPos + 1];
            const auto& i2 = incBlock->instructions[incPos + 2];
            const auto& i3 = incBlock->instructions[incPos + 3];
            if (i0.result < 0 ||
                i1.opcode != mir::Opcode::LoadLocal ||
                i1.localIndex != plan.indexLocalIndex ||
                i1.result < 0 ||
                i2.opcode != mir::Opcode::LoadLocal ||
                i2.result < 0 ||
                i3.opcode != mir::Opcode::StoreElement ||
                i3.operands.size() != 3 ||
                i3.operands[0] != i0.result ||
                i3.operands[1] != i1.result ||
                i3.operands[2] != i2.result) {
                incValid = false;
                break;
            }
            bool matchedCol = false;
            for (auto& col : plan.columns) {
                if (col.colLocalIndex == i0.localIndex &&
                    col.iterLocalIndex == i2.localIndex) {
                    col.isWrite = true;
                    matchedCol = true;
                    break;
                }
            }
            if (!matchedCol) {
                incValid = false;
                break;
            }
            incPos += 4;
            if (incPos < incBlock->instructions.size() &&
                incBlock->instructions[incPos].opcode == mir::Opcode::Call) {
                const std::string_view sym(incBlock->instructions[incPos].symbolName);
                if (sym.rfind("$intrinsic::ecs_writeback::", 0) == 0) {
                    plan.hasSparseWritebackCall = true;
                    incPos += 1;
                } else {
                    incValid = false;
                    break;
                }
            }
        }
        if (!incValid) continue;

        if (incPos >= incBlock->instructions.size()) continue;
        const auto& idxLoadInc = incBlock->instructions[incPos++];
        if (idxLoadInc.opcode != mir::Opcode::LoadLocal ||
            idxLoadInc.localIndex != plan.indexLocalIndex ||
            idxLoadInc.result < 0) {
            continue;
        }
        if (incPos < incBlock->instructions.size() &&
            incBlock->instructions[incPos].opcode == mir::Opcode::ConstantInt) {
            ++incPos;
        }
        if (incPos + 2 != incBlock->instructions.size()) continue;
        const auto& addInc = incBlock->instructions[incPos];
        const auto& storeInc = incBlock->instructions[incPos + 1];
        if (addInc.opcode != mir::Opcode::AddInt ||
            addInc.operands.size() != 2 ||
            addInc.operands[0] != idxLoadInc.result ||
            addInc.result < 0 ||
            storeInc.opcode != mir::Opcode::StoreLocal ||
            storeInc.localIndex != plan.indexLocalIndex ||
            storeInc.operands.size() != 1 ||
            storeInc.operands[0] != addInc.result) {
            continue;
        }
        const auto oneIt = constantInts.find(addInc.operands[1]);
        if (oneIt == constantInts.end() || oneIt->second != 1) continue;

        plans.emplace(plan.headerBlockId, std::move(plan));
    }
    return plans;
}


namespace {

void emitVectorizedBodyInstruction(
    Emitter& source,
    const mir::Instruction& instruction,
    const std::vector<TypedSlotType>& types,
    const std::string& indent) {
    (void)types;
    const auto result = instruction.result >= 0
        ? typedValueExpression(instruction.result)
        : std::string{};
    const auto operand = [&](std::size_t index) {
        return typedValueExpression(instruction.operands[index]);
    };
    switch (instruction.opcode) {
    case mir::Opcode::ConstantInt:
        if (instruction.resultType == semantic::PrimitiveType::Enum) {
            source.line(indent + result + " = runtime::EnumValue{0x" +
                hexId(instruction.resultTypeId) + "ULL, " +
                int64Literal(instruction.integerImmediate) + "};");
        } else {
            source.line(indent + result + " = std::int64_t{" +
                int64Literal(instruction.integerImmediate) + "};");
        }
        break;
    case mir::Opcode::ConstantDouble: {
        std::ostringstream literal;
        literal << std::setprecision(17) << instruction.doubleImmediate;
        if (instruction.resultType == semantic::PrimitiveType::Float) {
            source.line(indent + result + " = static_cast<float>(" +
                literal.str() + ");");
        } else {
            source.line(indent + result + " = " + literal.str() + ";");
        }
        break;
    }
    case mir::Opcode::ConstantBool:
        source.line(indent + result + " = " +
            (instruction.boolImmediate ? "true" : "false") + ";");
        break;
    case mir::Opcode::LoadLocal:
        source.line(indent + result + " = " +
            typedLocalExpression(instruction.localIndex) + ";");
        break;
    case mir::Opcode::StoreLocal:
        source.line(indent +
            typedLocalExpression(instruction.localIndex) + " = " +
            operand(0) + ";");
        break;
    case mir::Opcode::ConvertIntToLong:
        source.line(indent + result + " = static_cast<std::int64_t>(" + operand(0) + ");");
        break;
    case mir::Opcode::ConvertIntToDouble:
    case mir::Opcode::ConvertLongToDouble:
        source.line(indent + result + " = static_cast<double>(" + operand(0) + ");");
        break;
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt: {
        const auto operation = instruction.opcode == mir::Opcode::AddInt
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractInt ? " - " : " * ");
        if (!instruction.checkedArithmetic) {
            source.line(indent + result + " = aot::typedWrapInt32(" +
                operand(0) + operation + operand(1) + ");");
        } else {
            source.line(indent + result + " = " + operand(0) + operation + operand(1) + ";");
        }
        break;
    }
    case mir::Opcode::NegateInt:
        if (!instruction.checkedArithmetic) {
            source.line(indent + result + " = aot::typedWrapInt32(-" + operand(0) + ");");
        } else {
            source.line(indent + result + " = -" + operand(0) + ";");
        }
        break;
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong: {
        const auto operation = instruction.opcode == mir::Opcode::AddLong
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractLong ? " - " : " * ");
        source.line(indent + result +
            " = static_cast<std::int64_t>(static_cast<std::uint64_t>(" +
            operand(0) + ")" + operation + "static_cast<std::uint64_t>(" +
            operand(1) + "));");
        break;
    }
    case mir::Opcode::NegateLong:
        source.line(indent + result +
            " = static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(" +
            operand(0) + "));");
        break;
    case mir::Opcode::NegateDouble:
        source.line(indent + result + " = -" + operand(0) + ";");
        break;
    case mir::Opcode::AddDouble:
    case mir::Opcode::SubtractDouble:
    case mir::Opcode::MultiplyDouble:
    case mir::Opcode::DivideDouble: {
        const char* operation = instruction.opcode == mir::Opcode::AddDouble
            ? " + "
            : (instruction.opcode == mir::Opcode::SubtractDouble
                ? " - "
                : (instruction.opcode == mir::Opcode::MultiplyDouble
                    ? " * "
                    : " / "));
        source.line(indent + result + " = " + operand(0) + operation + operand(1) + ";");
        break;
    }
    case mir::Opcode::LogicalNot:
        source.line(indent + result + " = !" + operand(0) + ";");
        break;
    case mir::Opcode::Equal:
        source.line(indent + result + " = (" + operand(0) + " == " + operand(1) + ");");
        break;
    case mir::Opcode::NotEqual:
        source.line(indent + result + " = !(" + operand(0) + " == " + operand(1) + ");");
        break;
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
    case mir::Opcode::GreaterOrEqualDouble: {
        const char* operation = "<";
        if (instruction.opcode == mir::Opcode::LessOrEqualInt ||
            instruction.opcode == mir::Opcode::LessOrEqualLong ||
            instruction.opcode == mir::Opcode::LessOrEqualDouble) {
            operation = "<=";
        } else if (instruction.opcode == mir::Opcode::GreaterInt ||
                   instruction.opcode == mir::Opcode::GreaterLong ||
                   instruction.opcode == mir::Opcode::GreaterDouble) {
            operation = ">";
        } else if (instruction.opcode == mir::Opcode::GreaterOrEqualInt ||
                   instruction.opcode == mir::Opcode::GreaterOrEqualLong ||
                   instruction.opcode == mir::Opcode::GreaterOrEqualDouble) {
            operation = ">=";
        }
        source.line(indent + result + " = " + operand(0) + " " + operation + " " + operand(1) + ";");
        break;
    }
    case mir::Opcode::NewStruct: {
        const auto typeId = instruction.resultTypeId != 0
            ? instruction.resultTypeId
            : instruction.typeId;
        const auto kind = classifyInlineMath(semantic::PrimitiveType::Struct, typeId);
        if (kind != InlineMathKind::None) {
            source.line(indent + result + " = " +
                typedDefaultValue({semantic::PrimitiveType::Struct, typeId}) + ";");
        } else {
            source.line(indent + result + " = " +
                typedDefaultValue({semantic::PrimitiveType::Struct, typeId}) + ";");
            source.line(indent + "(void)context.initUnboxedStruct(0x" +
                hexId(typeId) + "ULL, " + result + ");");
        }
        break;
    }
    case mir::Opcode::LoadStructField: {
        const auto kind = classifyInlineMath(
            semantic::PrimitiveType::Struct, instruction.typeId);
        if (kind != InlineMathKind::None) {
            const char* member = inlineMathFieldMember(kind, instruction.fieldIndex);
            if (kind == InlineMathKind::Fixed || kind == InlineMathKind::Fixed16) {
                source.line(indent + result + " = static_cast<std::int64_t>(" +
                    operand(0) + "." + member + ");");
            } else {
                source.line(indent + result + " = runtime::FixedValue{" +
                    operand(0) + "." + member + "};");
            }
        } else {
            source.line(indent + result + " = " + operand(0) + ".f_" +
                std::to_string(instruction.fieldIndex) + ";");
        }
        break;
    }
    case mir::Opcode::StoreStructField: {
        const auto kind = classifyInlineMath(
            semantic::PrimitiveType::Struct, instruction.typeId);
        if (kind != InlineMathKind::None) {
            const char* member = inlineMathFieldMember(kind, instruction.fieldIndex);
            if (result != operand(0)) {
                source.line(indent + result + " = " + operand(0) + ";");
            }
            if (kind == InlineMathKind::Fixed || kind == InlineMathKind::Fixed16) {
                source.line(indent + result + "." + member +
                    " = static_cast<std::int32_t>(" + operand(1) + ");");
            } else {
                source.line(indent + result + "." + member + " = " + operand(1) + ".raw;");
            }
        } else {
            if (result != operand(0)) {
                source.line(indent + result + " = " + operand(0) + ";");
            }
            source.line(indent + result + ".f_" +
                std::to_string(instruction.fieldIndex) + " = " + operand(1) + ";");
        }
        break;
    }
    default:
        break;
    }
}


} // namespace

void emitVectorizedEcsLoop(
    Emitter& source,
    const mir::Module& module,
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const VectorizedEcsLoopPlan& plan,
    bool emitLineDirectives) {
    const auto* bodyBlock = findBlock(function, plan.bodyBlockId);
    if (!bodyBlock) return;

    const auto idxLocalExpr = typedLocalExpression(plan.indexLocalIndex);
    std::string exactCond;
    std::string rawCond;
    for (std::size_t c = 0; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        if (!exactCond.empty()) exactCond += " && ";
        if (!rawCond.empty()) rawCond += " && ";
        exactCond += rawCol + ".exactNativeLayout";
        rawCond += "(" + rawCol + ".basePtr != nullptr)";
        if (plan.columns[c].isWrite) {
            exactCond += " && !" + rawCol + ".readOnly";
            rawCond += " && !" + rawCol + ".readOnly";
        }
    }
    exactCond += " && (" + idxLocalExpr + " >= 0)";
    rawCond += " && (" + idxLocalExpr + " >= 0)";
    if (plan.hasSparseWritebackCall) {
        exactCond += " && !context.hasSparseEcsState()";
        rawCond += " && !context.hasSparseEcsState()";
    }

    const auto firstRawCol = "typedLocalRawCol_" + std::to_string(plan.columns[0].colLocalIndex);

    source.line("            // Reconstructed Vectorized ECS Query Fast-Loop (__restrict + #pragma GCC ivdep)");
    source.line("            if constexpr (TrustedRelease) {");
    source.line("                if (RS_AOT_LIKELY(" + exactCond + ")) {");
    source.line("                    std::int64_t ecsFastCount = " + firstRawCol + ".count;");
    for (std::size_t c = 1; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        source.line("                    if (" + rawCol + ".count < ecsFastCount) ecsFastCount = " + rawCol + ".count;");
    }
    for (const auto& col : plan.columns) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        if (col.isWrite) {
            source.line("                    auto* __restrict " + ptrName +
                " = reinterpret_cast<" + structName + "*>(" + rawCol + ".basePtr);");
        } else {
            source.line("                    const auto* __restrict " + ptrName +
                " = reinterpret_cast<const " + structName + "*>(" + rawCol + ".basePtr);");
        }
    }
    source.line("#if defined(__GNUC__) && !defined(__clang__)");
    source.line("#pragma GCC ivdep");
    source.line("#elif defined(__clang__)");
    source.line("#pragma clang loop vectorize(enable) interleave(enable)");
    source.line("#elif defined(_MSC_VER) && !defined(__clang__)");
    source.line("#pragma loop(ivdep)");
    source.line("#endif");
    source.line("                    for (std::int64_t ecsIdx = " + idxLocalExpr + "; ecsIdx < ecsFastCount; ++ecsIdx) {");
    if (plan.usesIndexInsideBody) {
        source.line("                        " + idxLocalExpr + " = ecsIdx;");
    }
    for (const auto& col : plan.columns) {
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        source.line("                        " + typedLocalExpression(col.iterLocalIndex) +
            " = " + ptrName + "[ecsIdx];");
    }
    for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
        emitLineDirective(
            source,
            module,
            resolveSequencePoint(
                module,
                function,
                bodyBlock->id,
                static_cast<std::uint32_t>(i),
                false,
                bodyBlock->instructions[i].sourceSpan),
            emitLineDirectives,
            nullptr,
            function.symbolId,
            false);
        emitVectorizedBodyInstruction(source, bodyBlock->instructions[i], types, "                        ");
    }
    emitGeneratedLineReset(source, emitLineDirectives);
    for (const auto& col : plan.columns) {
        if (!col.isWrite) continue;
        const std::string ptrName = "ecsPtr_" + std::to_string(col.colLocalIndex);
        source.line("                        " + ptrName + "[ecsIdx] = " +
            typedLocalExpression(col.iterLocalIndex) + ";");
    }
    source.line("                    }");
    source.line("                    if (ecsFastCount > " + idxLocalExpr + ") " + idxLocalExpr + " = ecsFastCount;");
    source.line("                    goto typedBlock_" + std::to_string(plan.exitBlockId) + ";");
    source.line("                } else if (RS_AOT_LIKELY(" + rawCond + ")) {");
    source.line("                    std::int64_t ecsFastCount = " + firstRawCol + ".count;");
    for (std::size_t c = 1; c < plan.columns.size(); ++c) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(plan.columns[c].colLocalIndex);
        source.line("                    if (" + rawCol + ".count < ecsFastCount) ecsFastCount = " + rawCol + ".count;");
    }
    source.line("                    for (std::int64_t ecsIdx = " + idxLocalExpr + "; ecsIdx < ecsFastCount; ++ecsIdx) {");
    if (plan.usesIndexInsideBody) {
        source.line("                        " + idxLocalExpr + " = ecsIdx;");
    }
    for (const auto& col : plan.columns) {
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        source.line("                        (void)" + structName + "::loadFromRawColumn(" +
            rawCol + ", ecsIdx, " + typedLocalExpression(col.iterLocalIndex) + ");");
    }
    for (std::size_t i = plan.bodyPreambleCount; i < bodyBlock->instructions.size(); ++i) {
        emitLineDirective(
            source,
            module,
            resolveSequencePoint(
                module,
                function,
                bodyBlock->id,
                static_cast<std::uint32_t>(i),
                false,
                bodyBlock->instructions[i].sourceSpan),
            emitLineDirectives,
            nullptr,
            function.symbolId,
            false);
        emitVectorizedBodyInstruction(source, bodyBlock->instructions[i], types, "                        ");
    }
    emitGeneratedLineReset(source, emitLineDirectives);
    for (const auto& col : plan.columns) {
        if (!col.isWrite) continue;
        const auto rawCol = "typedLocalRawCol_" + std::to_string(col.colLocalIndex);
        const std::string structName = "AotStruct_" + hexId(col.elemTypeId);
        source.line("                        (void)" + structName + "::storeToRawColumn(" +
            rawCol + ", ecsIdx, " + typedLocalExpression(col.iterLocalIndex) + ");");
    }
    source.line("                    }");
    source.line("                    if (ecsFastCount > " + idxLocalExpr + ") " + idxLocalExpr + " = ecsFastCount;");
    source.line("                    goto typedBlock_" + std::to_string(plan.exitBlockId) + ";");
    source.line("                }");
    source.line("            }");
}


std::string packedArrayCacheName(
    std::size_t localIndex,
    semantic::PrimitiveType elemPrim) {
    return (elemPrim == semantic::PrimitiveType::Int
        ? "typedLocalPackedI32_"
        : "typedLocalPackedF64_") + std::to_string(localIndex);
}

std::unordered_map<mir::BlockId, VectorizedPackedArrayLoopPlan> analyzeVectorizedPackedArrayLoops(
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const TypedRangeAnalysis& rangeAnalysis,
    const std::unordered_map<std::size_t, semantic::PrimitiveType>& singleStorePackedArrayLocals) {
    std::unordered_map<mir::BlockId, VectorizedPackedArrayLoopPlan> plans;
    if (singleStorePackedArrayLocals.empty() || !function.exceptionHandlers.empty()) {
        return plans;
    }

    // Count block predecessors & constant integer values
    std::unordered_map<mir::BlockId, std::size_t> predCounts;
    std::unordered_map<mir::ValueId, std::int64_t> constantInts;
    std::unordered_map<mir::ValueId, mir::BlockId> valueDefBlock;
    for (const auto& blk : function.blocks) {
        for (const auto& bp : blk.parameters) {
            if (bp.value >= 0) valueDefBlock[bp.value] = blk.id;
        }
        for (const auto& inst : blk.instructions) {
            if (inst.result >= 0) {
                valueDefBlock[inst.result] = blk.id;
                if (inst.opcode == mir::Opcode::ConstantInt) {
                    constantInts[inst.result] = inst.integerImmediate;
                }
            }
        }
        if (blk.terminator.kind == mir::TerminatorKind::Jump) {
            ++predCounts[blk.terminator.target];
        } else if (blk.terminator.kind == mir::TerminatorKind::Branch) {
            ++predCounts[blk.terminator.target];
            ++predCounts[blk.terminator.falseTarget];
        }
    }

    for (const auto& headerBlock : function.blocks) {
        if (headerBlock.terminator.kind != mir::TerminatorKind::Branch ||
            !headerBlock.terminator.arguments.empty() ||
            !headerBlock.terminator.falseArguments.empty() ||
            headerBlock.instructions.size() < 2 ||
            headerBlock.instructions.size() > 4) {
            continue;
        }
        const auto& idxLoad = headerBlock.instructions[0];
        if (idxLoad.opcode != mir::Opcode::LoadLocal ||
            idxLoad.result < 0 ||
            idxLoad.localIndex >= function.localTypes.size() ||
            function.localTypes[idxLoad.localIndex] != semantic::PrimitiveType::Int) {
            continue;
        }

        const mir::BlockId bodyId = headerBlock.terminator.target;
        const mir::BlockId exitId = headerBlock.terminator.falseTarget;
        if (bodyId == headerBlock.id || exitId == headerBlock.id || bodyId == exitId) {
            continue;
        }
        const auto* bodyBlock = findBlock(function, bodyId);
        const auto* exitBlock = findBlock(function, exitId);
        if (!bodyBlock || !exitBlock ||
            !bodyBlock->parameters.empty() ||
            !exitBlock->parameters.empty() ||
            predCounts[bodyId] != 1u ||
            bodyBlock->terminator.kind != mir::TerminatorKind::Jump ||
            !bodyBlock->terminator.arguments.empty()) {
            continue;
        }

        const mir::BlockId incId = bodyBlock->terminator.target;
        const auto* incBlock = findBlock(function, incId);
        if (!incBlock ||
            incId == headerBlock.id ||
            incId == bodyId ||
            incId == exitId ||
            !incBlock->parameters.empty() ||
            predCounts[incId] != 1u ||
            incBlock->terminator.kind != mir::TerminatorKind::Jump ||
            incBlock->terminator.target != headerBlock.id ||
            !incBlock->terminator.arguments.empty()) {
            continue;
        }

        const auto isInsideLoop = [&](mir::BlockId bid) noexcept {
            return bid == headerBlock.id || bid == bodyId || bid == incId;
        };

        // Check increment block: [load.local idx, (const.int 1)?, add.i32 idx 1, store.local idx]
        std::size_t incPos = 0;
        if (incBlock->instructions.size() != 3 && incBlock->instructions.size() != 4) {
            continue;
        }
        const auto& incLoad = incBlock->instructions[incPos++];
        if (incLoad.opcode != mir::Opcode::LoadLocal ||
            incLoad.localIndex != idxLoad.localIndex ||
            incLoad.result < 0) {
            continue;
        }
        if (incBlock->instructions.size() == 4) {
            const auto& incConst = incBlock->instructions[incPos++];
            if (incConst.opcode != mir::Opcode::ConstantInt || incConst.integerImmediate != 1) {
                continue;
            }
        }
        const auto& incAdd = incBlock->instructions[incPos++];
        const auto& incStore = incBlock->instructions[incPos++];
        if (incAdd.opcode != mir::Opcode::AddInt ||
            incAdd.operands.size() != 2 ||
            incAdd.operands[0] != incLoad.result ||
            incAdd.result < 0 ||
            incStore.opcode != mir::Opcode::StoreLocal ||
            incStore.localIndex != idxLoad.localIndex ||
            incStore.operands.size() != 1 ||
            incStore.operands[0] != incAdd.result) {
            continue;
        }
        const auto oneIt = constantInts.find(incAdd.operands[1]);
        if (oneIt == constantInts.end() || oneIt->second != 1) {
            continue;
        }

        // Collect locals written inside bodyBlock or incBlock
        std::unordered_set<std::size_t> loopWrittenLocals{idxLoad.localIndex};
        for (const auto& inst : bodyBlock->instructions) {
            if (inst.opcode == mir::Opcode::StoreLocal) {
                loopWrittenLocals.insert(inst.localIndex);
            }
        }

        // Match header bound shape
        VectorizedPackedArrayLoopPlan plan;
        plan.headerBlockId = headerBlock.id;
        plan.bodyBlockId = bodyId;
        plan.incrementBlockId = incId;
        plan.exitBlockId = exitId;
        plan.indexLocalIndex = idxLoad.localIndex;

        std::map<std::size_t, VectorizedPackedArrayBinding> boundArrays;
        auto touchArrayLocal = [&](std::size_t arrLoc, semantic::PrimitiveType prim, bool rd, bool wr) -> bool {
            const auto pIt = singleStorePackedArrayLocals.find(arrLoc);
            if (pIt == singleStorePackedArrayLocals.end() || pIt->second != prim) {
                return false;
            }
            auto& entry = boundArrays[arrLoc];
            entry.arrayLocalIndex = arrLoc;
            entry.elemPrim = prim;
            if (rd) entry.isRead = true;
            if (wr) entry.isWrite = true;
            return true;
        };

        if (headerBlock.instructions.size() == 2) {
            const auto& cmp = headerBlock.instructions[1];
            if (cmp.opcode != mir::Opcode::LessInt ||
                cmp.operands.size() != 2 ||
                cmp.operands[0] != idxLoad.result ||
                cmp.result != headerBlock.terminator.condition) {
                continue;
            }
            const mir::ValueId bndVal = cmp.operands[1];
            const auto defIt = valueDefBlock.find(bndVal);
            if (bndVal < 0 ||
                static_cast<std::size_t>(bndVal) >= types.size() ||
                types[static_cast<std::size_t>(bndVal)].primitive != semantic::PrimitiveType::Int ||
                (defIt != valueDefBlock.end() && isInsideLoop(defIt->second))) {
                continue;
            }
            plan.boundExpr = typedValueExpression(bndVal);
        } else if (headerBlock.instructions.size() == 3) {
            const auto& bndInst = headerBlock.instructions[1];
            const auto& cmp = headerBlock.instructions[2];
            if (cmp.opcode != mir::Opcode::LessInt ||
                cmp.operands.size() != 2 ||
                cmp.operands[0] != idxLoad.result ||
                cmp.operands[1] != bndInst.result ||
                cmp.result != headerBlock.terminator.condition) {
                continue;
            }
            if (bndInst.opcode == mir::Opcode::LoadLocal &&
                bndInst.localIndex < function.localTypes.size() &&
                function.localTypes[bndInst.localIndex] == semantic::PrimitiveType::Int &&
                loopWrittenLocals.find(bndInst.localIndex) == loopWrittenLocals.end()) {
                plan.boundExpr = typedLocalExpression(bndInst.localIndex);
            } else if (bndInst.opcode == mir::Opcode::ConstantInt) {
                plan.boundExpr = "std::int64_t{" + int64Literal(bndInst.integerImmediate) + "}";
            } else {
                continue;
            }
        } else if (headerBlock.instructions.size() == 4) {
            const auto& arrLoad = headerBlock.instructions[1];
            const auto& lenInst = headerBlock.instructions[2];
            const auto& cmp = headerBlock.instructions[3];
            if (arrLoad.opcode != mir::Opcode::LoadLocal ||
                arrLoad.result < 0 ||
                lenInst.opcode != mir::Opcode::ArrayLength ||
                lenInst.operands.size() != 1 ||
                lenInst.operands[0] != arrLoad.result ||
                lenInst.result < 0 ||
                cmp.opcode != mir::Opcode::LessInt ||
                cmp.operands.size() != 2 ||
                cmp.operands[0] != idxLoad.result ||
                cmp.operands[1] != lenInst.result ||
                cmp.result != headerBlock.terminator.condition) {
                continue;
            }
            const auto pIt = singleStorePackedArrayLocals.find(arrLoad.localIndex);
            if (pIt == singleStorePackedArrayLocals.end()) {
                continue;
            }
            (void)touchArrayLocal(arrLoad.localIndex, pIt->second, true, false);
            plan.boundExpr = packedArrayCacheName(arrLoad.localIndex, pIt->second) + ".count";
        } else {
            continue;
        }

        // Verify no ValueId defined inside the loop is read outside the loop
        bool ssaEscapes = false;
        for (const auto& blk : function.blocks) {
            if (isInsideLoop(blk.id)) continue;
            auto checkExternalUse = [&](mir::ValueId vid) {
                if (vid < 0) return;
                const auto dIt = valueDefBlock.find(vid);
                if (dIt != valueDefBlock.end() && isInsideLoop(dIt->second)) {
                    ssaEscapes = true;
                }
            };
            for (const auto& inst : blk.instructions) {
                for (const auto op : inst.operands) checkExternalUse(op);
            }
            checkExternalUse(blk.terminator.value);
            checkExternalUse(blk.terminator.condition);
            for (const auto a : blk.terminator.arguments) checkExternalUse(a);
            for (const auto a : blk.terminator.falseArguments) checkExternalUse(a);
            if (ssaEscapes) break;
        }
        if (ssaEscapes) continue;

        // Inspect bodyBlock instructions
        bool bodyOk = true;
        bool hasElementAccess = false;
        for (const auto& inst : bodyBlock->instructions) {
            // Operands cannot come from headerBlock or incBlock
            for (const auto op : inst.operands) {
                const auto dIt = valueDefBlock.find(op);
                if (dIt != valueDefBlock.end() &&
                    (dIt->second == headerBlock.id || dIt->second == incId)) {
                    bodyOk = false;
                    break;
                }
            }
            if (!bodyOk) break;

            switch (inst.opcode) {
            case mir::Opcode::LoadLocal: {
                if (inst.localIndex >= function.localTypes.size() || inst.result < 0) {
                    bodyOk = false;
                    break;
                }
                const auto lPrim = function.localTypes[inst.localIndex];
                if (lPrim == semantic::PrimitiveType::Array) {
                    if (singleStorePackedArrayLocals.find(inst.localIndex) ==
                        singleStorePackedArrayLocals.end()) {
                        bodyOk = false;
                        break;
                    }
                    plan.bodyArrayLoadRegs[inst.result] = inst.localIndex;
                } else if (inst.localIndex == plan.indexLocalIndex) {
                    plan.bodyIndexLoadRegs.insert(inst.result);
                } else if (lPrim == semantic::PrimitiveType::Object ||
                           lPrim == semantic::PrimitiveType::String ||
                           lPrim == semantic::PrimitiveType::Handle) {
                    bodyOk = false;
                }
                break;
            }
            case mir::Opcode::StoreLocal: {
                if (inst.localIndex == plan.indexLocalIndex ||
                    inst.localIndex >= function.localTypes.size()) {
                    bodyOk = false;
                    break;
                }
                const auto lPrim = function.localTypes[inst.localIndex];
                if (lPrim == semantic::PrimitiveType::Array ||
                    lPrim == semantic::PrimitiveType::Object ||
                    lPrim == semantic::PrimitiveType::String ||
                    lPrim == semantic::PrimitiveType::Handle) {
                    bodyOk = false;
                }
                break;
            }
            case mir::Opcode::LoadElement: {
                if (inst.operands.size() != 2 ||
                    inst.result < 0 ||
                    inst.elementTypeId != 0 ||
                    (inst.elementType != semantic::PrimitiveType::Int &&
                     inst.elementType != semantic::PrimitiveType::Double) ||
                    inst.resultType != inst.elementType) {
                    bodyOk = false;
                    break;
                }
                const auto arrIt = plan.bodyArrayLoadRegs.find(inst.operands[0]);
                if (arrIt == plan.bodyArrayLoadRegs.end() ||
                    plan.bodyIndexLoadRegs.find(inst.operands[1]) == plan.bodyIndexLoadRegs.end() ||
                    !touchArrayLocal(arrIt->second, inst.elementType, true, false)) {
                    bodyOk = false;
                    break;
                }
                hasElementAccess = true;
                break;
            }
            case mir::Opcode::StoreElement: {
                if (inst.operands.size() != 3 ||
                    inst.elementTypeId != 0 ||
                    (inst.elementType != semantic::PrimitiveType::Int &&
                     inst.elementType != semantic::PrimitiveType::Double)) {
                    bodyOk = false;
                    break;
                }
                const auto valVid = inst.operands[2];
                if (valVid < 0 ||
                    static_cast<std::size_t>(valVid) >= types.size() ||
                    types[static_cast<std::size_t>(valVid)].primitive != inst.elementType) {
                    bodyOk = false;
                    break;
                }
                const auto arrIt = plan.bodyArrayLoadRegs.find(inst.operands[0]);
                if (arrIt == plan.bodyArrayLoadRegs.end() ||
                    plan.bodyIndexLoadRegs.find(inst.operands[1]) == plan.bodyIndexLoadRegs.end() ||
                    !touchArrayLocal(arrIt->second, inst.elementType, false, true)) {
                    bodyOk = false;
                    break;
                }
                hasElementAccess = true;
                break;
            }
            case mir::Opcode::ConstantInt:
                if (inst.resultType == semantic::PrimitiveType::Enum) bodyOk = false;
                break;
            case mir::Opcode::ConstantDouble:
            case mir::Opcode::ConstantBool:
            case mir::Opcode::ConvertIntToLong:
            case mir::Opcode::ConvertIntToDouble:
            case mir::Opcode::ConvertLongToDouble:
            case mir::Opcode::AddDouble:
            case mir::Opcode::SubtractDouble:
            case mir::Opcode::MultiplyDouble:
            case mir::Opcode::DivideDouble:
            case mir::Opcode::NegateDouble:
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
                break;
            case mir::Opcode::AddLong:
            case mir::Opcode::SubtractLong:
            case mir::Opcode::MultiplyLong:
            case mir::Opcode::NegateLong:
                if (inst.checkedArithmetic) {
                    bodyOk = false;
                }
                break;
            case mir::Opcode::AddInt:
            case mir::Opcode::SubtractInt:
            case mir::Opcode::MultiplyInt:
            case mir::Opcode::NegateInt:
                if (inst.checkedArithmetic &&
                    !typedCheckedArithmeticProvenSafe(rangeAnalysis, inst)) {
                    bodyOk = false;
                }
                break;
            default:
                bodyOk = false;
                break;
            }
            if (!bodyOk) break;
        }
        if (!bodyOk || !hasElementAccess || boundArrays.empty()) {
            continue;
        }

        // Verify any LoadLocal(array) register inside bodyBlock is ONLY consumed by LoadElement/StoreElement[0]
        for (const auto& inst : bodyBlock->instructions) {
            for (std::size_t oi = 0; oi < inst.operands.size(); ++oi) {
                if (plan.bodyArrayLoadRegs.find(inst.operands[oi]) != plan.bodyArrayLoadRegs.end()) {
                    const bool validArrayUse =
                        oi == 0u &&
                        (inst.opcode == mir::Opcode::LoadElement ||
                         inst.opcode == mir::Opcode::StoreElement);
                    if (!validArrayUse) {
                        bodyOk = false;
                        break;
                    }
                }
            }
            if (!bodyOk) break;
        }
        if (!bodyOk) continue;

        for (const auto& [arrLoc, binding] : boundArrays) {
            (void)arrLoc;
            plan.arrays.push_back(binding);
        }
        plan.headerInstsWithTerminator =
            static_cast<std::uint64_t>(headerBlock.instructions.size()) + 1ULL;
        plan.iterInstsWithTerminators =
            plan.headerInstsWithTerminator +
            (static_cast<std::uint64_t>(bodyBlock->instructions.size()) + 1ULL) +
            (static_cast<std::uint64_t>(incBlock->instructions.size()) + 1ULL);
        plan.iterBranches = 3ULL;

        plans.emplace(plan.headerBlockId, std::move(plan));
    }

    return plans;
}

void emitVectorizedPackedArrayLoop(
    Emitter& source,
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const VectorizedPackedArrayLoopPlan& plan) {
    const auto* bodyBlock = findBlock(function, plan.bodyBlockId);
    if (!bodyBlock) return;

    const std::string idxLocalExpr = typedLocalExpression(plan.indexLocalIndex);
    std::string validCachesCond;
    std::string inBoundsCond;
    std::string noAliasCond;

    for (const auto& arr : plan.arrays) {
        const std::string cName = packedArrayCacheName(arr.arrayLocalIndex, arr.elemPrim);
        if (!validCachesCond.empty()) validCachesCond += " && ";
        validCachesCond += "(" + cName + ".count >= 0)";
        if (!inBoundsCond.empty()) inBoundsCond += " && ";
        inBoundsCond += "(vecEnd <= " + cName + ".count)";
    }

    for (std::size_t i = 0; i < plan.arrays.size(); ++i) {
        for (std::size_t j = i + 1u; j < plan.arrays.size(); ++j) {
            if (!plan.arrays[i].isWrite && !plan.arrays[j].isWrite) continue;
            const std::string cA =
                packedArrayCacheName(plan.arrays[i].arrayLocalIndex, plan.arrays[i].elemPrim);
            const std::string cB =
                packedArrayCacheName(plan.arrays[j].arrayLocalIndex, plan.arrays[j].elemPrim);
            if (!noAliasCond.empty()) noAliasCond += " && ";
            noAliasCond += "(static_cast<const void*>(" + cA +
                ".data) != static_cast<const void*>(" + cB + ".data))";
        }
    }

    std::string activeTripGuard = inBoundsCond;
    if (!noAliasCond.empty()) {
        activeTripGuard += " && " + noAliasCond;
    }

    // Track body-defined ValueIds so Int temporaries use narrow 32-bit std::int32_t for AVX2/NEON SIMD
    std::unordered_set<mir::ValueId> bodyDefinedValues;
    for (const auto& inst : bodyBlock->instructions) {
        if (inst.result >= 0 &&
            plan.bodyArrayLoadRegs.find(inst.result) == plan.bodyArrayLoadRegs.end()) {
            bodyDefinedValues.insert(inst.result);
        }
    }

    const auto bodyOperandExpr = [&](mir::ValueId vid) -> std::string {
        if (bodyDefinedValues.find(vid) != bodyDefinedValues.end()) {
            return "vecReg_" + std::to_string(vid);
        }
        return typedValueExpression(vid);
    };

    const auto vecRegCppType = [&](mir::ValueId vid) -> std::string {
        const auto& slot = types[static_cast<std::size_t>(vid)];
        if (slot.primitive == semantic::PrimitiveType::Int) {
            return "std::int32_t";
        }
        return typedCppType(slot);
    };

    source.line("            // P2-C: Reconstructed PackedArray __restrict SIMD Fast-Loop");
    source.line("            if constexpr (TrustedRelease || FastAccounting) {");
    source.line("                const std::int64_t vecStart = " + idxLocalExpr + ";");
    source.line("                const std::int64_t vecEnd = " + plan.boundExpr + ";");
    source.line("                if (RS_AOT_LIKELY(" + validCachesCond +
        " && (vecStart >= 0) && (vecEnd <= static_cast<std::int64_t>(INT32_MAX)) && "
        "((vecEnd <= vecStart) || (" + activeTripGuard + ")))) {");
    source.line("                    const std::uint64_t vecTrips = (vecEnd > vecStart) "
        "? static_cast<std::uint64_t>(vecEnd - vecStart) : 0ULL;");
    source.line("                    const std::uint64_t vecTotalInsts = vecTrips * " +
        std::to_string(plan.iterInstsWithTerminators) + "ULL + " +
        std::to_string(plan.headerInstsWithTerminator) + "ULL;");
    source.line("                    const std::uint64_t vecTotalBranches = vecTrips * " +
        std::to_string(plan.iterBranches) + "ULL + 1ULL;");
    source.line("                    if (TrustedRelease || rawAccounting.canConsume(vecTotalInsts)) {");
    source.line("                        if constexpr (FastAccounting && !TrustedRelease) {");
    source.line("                            (void)rawAccounting.consume(vecTotalInsts);");
    source.line("                            rawAccounting.addBranches(vecTotalBranches);");
    source.line("                        }");
    source.line("                        if (vecTrips > 0ULL) {");
    for (const auto& arr : plan.arrays) {
        const std::string cName = packedArrayCacheName(arr.arrayLocalIndex, arr.elemPrim);
        const std::string ptrName = "packedPtr_" + std::to_string(arr.arrayLocalIndex);
        const std::string elemTypeStr =
            (arr.elemPrim == semantic::PrimitiveType::Int) ? "std::int32_t" : "double";
        if (arr.isWrite) {
            source.line("                            " + elemTypeStr + "* RS_AOT_RESTRICT " +
                ptrName + " = " + cName + ".data;");
        } else {
            source.line("                            const " + elemTypeStr + "* RS_AOT_RESTRICT " +
                ptrName + " = " + cName + ".data;");
        }
        source.line("                            (void)" + ptrName + ";");
    }
    source.line("                            const auto vecStartIdx = static_cast<std::ptrdiff_t>(vecStart);");
    source.line("                            const auto vecEndIdx = static_cast<std::ptrdiff_t>(vecEnd);");
    source.line("                            RS_AOT_PRAGMA_SIMD_LOOP");
    source.line("                            for (std::ptrdiff_t vecIdx = vecStartIdx; vecIdx < vecEndIdx; ++vecIdx) {");

    const std::string ind = "                                ";
    for (const auto& inst : bodyBlock->instructions) {
        if (inst.opcode == mir::Opcode::LoadLocal &&
            plan.bodyArrayLoadRegs.find(inst.result) != plan.bodyArrayLoadRegs.end()) {
            continue;
        }
        const std::string resDecl = (inst.result >= 0)
            ? ("[[maybe_unused]] const " + vecRegCppType(inst.result) +
               " vecReg_" + std::to_string(inst.result))
            : std::string{};
        const auto op = [&](std::size_t idx) {
            return bodyOperandExpr(inst.operands[idx]);
        };

        switch (inst.opcode) {
        case mir::Opcode::LoadLocal:
            if (inst.localIndex == plan.indexLocalIndex) {
                source.line(ind + resDecl + " = static_cast<std::int32_t>(vecIdx);");
            } else if (function.localTypes[inst.localIndex] == semantic::PrimitiveType::Int) {
                source.line(ind + resDecl + " = static_cast<std::int32_t>(" +
                    typedLocalExpression(inst.localIndex) + ");");
            } else {
                source.line(ind + resDecl + " = " +
                    typedLocalExpression(inst.localIndex) + ";");
            }
            break;
        case mir::Opcode::StoreLocal:
            if (function.localTypes[inst.localIndex] == semantic::PrimitiveType::Int) {
                source.line(ind + typedLocalExpression(inst.localIndex) +
                    " = static_cast<std::int64_t>(" + op(0) + ");");
            } else {
                source.line(ind + typedLocalExpression(inst.localIndex) + " = " + op(0) + ";");
            }
            break;
        case mir::Opcode::LoadElement: {
            const std::size_t arrLoc = plan.bodyArrayLoadRegs.at(inst.operands[0]);
            source.line(ind + resDecl + " = packedPtr_" + std::to_string(arrLoc) + "[vecIdx];");
            break;
        }
        case mir::Opcode::StoreElement: {
            const std::size_t arrLoc = plan.bodyArrayLoadRegs.at(inst.operands[0]);
            if (inst.elementType == semantic::PrimitiveType::Int) {
                source.line(ind + "packedPtr_" + std::to_string(arrLoc) +
                    "[vecIdx] = static_cast<std::int32_t>(" + op(2) + ");");
            } else {
                source.line(ind + "packedPtr_" + std::to_string(arrLoc) +
                    "[vecIdx] = " + op(2) + ";");
            }
            break;
        }
        case mir::Opcode::ConstantInt:
            if (inst.resultType == semantic::PrimitiveType::Long) {
                source.line(ind + resDecl + " = std::int64_t{" +
                    int64Literal(inst.integerImmediate) + "};");
            } else {
                source.line(ind + resDecl + " = static_cast<std::int32_t>(" +
                    int64Literal(inst.integerImmediate) + ");");
            }
            break;
        case mir::Opcode::ConstantDouble: {
            std::ostringstream lit;
            lit << std::setprecision(17) << inst.doubleImmediate;
            if (inst.resultType == semantic::PrimitiveType::Float) {
                source.line(ind + resDecl + " = static_cast<float>(" + lit.str() + ");");
            } else {
                source.line(ind + resDecl + " = " + lit.str() + ";");
            }
            break;
        }
        case mir::Opcode::ConstantBool:
            source.line(ind + resDecl + " = " +
                (inst.boolImmediate ? "true" : "false") + ";");
            break;
        case mir::Opcode::ConvertIntToLong:
            source.line(ind + resDecl + " = static_cast<std::int64_t>(" + op(0) + ");");
            break;
        case mir::Opcode::ConvertIntToDouble:
        case mir::Opcode::ConvertLongToDouble:
            source.line(ind + resDecl + " = static_cast<double>(" + op(0) + ");");
            break;
        case mir::Opcode::AddInt:
        case mir::Opcode::SubtractInt:
        case mir::Opcode::MultiplyInt: {
            const char* sym = inst.opcode == mir::Opcode::AddInt
                ? " + "
                : (inst.opcode == mir::Opcode::SubtractInt ? " - " : " * ");
            source.line(ind + resDecl +
                " = static_cast<std::int32_t>(static_cast<std::uint32_t>(" + op(0) + ")" +
                sym + "static_cast<std::uint32_t>(" + op(1) + "));");
            break;
        }
        case mir::Opcode::NegateInt:
            source.line(ind + resDecl +
                " = static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(" + op(0) + "));");
            break;
        case mir::Opcode::AddLong:
        case mir::Opcode::SubtractLong:
        case mir::Opcode::MultiplyLong: {
            const char* sym = inst.opcode == mir::Opcode::AddLong
                ? " + "
                : (inst.opcode == mir::Opcode::SubtractLong ? " - " : " * ");
            source.line(ind + resDecl +
                " = static_cast<std::int64_t>(static_cast<std::uint64_t>(" + op(0) + ")" +
                sym + "static_cast<std::uint64_t>(" + op(1) + "));");
            break;
        }
        case mir::Opcode::NegateLong:
            source.line(ind + resDecl +
                " = static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(" + op(0) + "));");
            break;
        case mir::Opcode::AddDouble:
        case mir::Opcode::SubtractDouble:
        case mir::Opcode::MultiplyDouble:
        case mir::Opcode::DivideDouble: {
            const char* sym = inst.opcode == mir::Opcode::AddDouble
                ? " + "
                : (inst.opcode == mir::Opcode::SubtractDouble
                    ? " - "
                    : (inst.opcode == mir::Opcode::MultiplyDouble ? " * " : " / "));
            source.line(ind + resDecl + " = " + op(0) + sym + op(1) + ";");
            break;
        }
        case mir::Opcode::NegateDouble:
            source.line(ind + resDecl + " = -" + op(0) + ";");
            break;
        case mir::Opcode::LogicalNot:
            source.line(ind + resDecl + " = !" + op(0) + ";");
            break;
        case mir::Opcode::Equal:
        case mir::Opcode::NotEqual: {
            const auto lhsPrim = types[static_cast<std::size_t>(inst.operands[0])].primitive;
            const char* cmpOp = (inst.opcode == mir::Opcode::Equal) ? " == " : " != ";
            if (lhsPrim == semantic::PrimitiveType::Int) {
                source.line(ind + resDecl + " = (static_cast<std::int32_t>(" + op(0) + ")" +
                    cmpOp + "static_cast<std::int32_t>(" + op(1) + "));");
            } else {
                source.line(ind + resDecl + " = (" + op(0) + cmpOp + op(1) + ");");
            }
            break;
        }
        case mir::Opcode::LessInt:
        case mir::Opcode::LessOrEqualInt:
        case mir::Opcode::GreaterInt:
        case mir::Opcode::GreaterOrEqualInt: {
            const char* cmpOp = inst.opcode == mir::Opcode::LessInt
                ? " < "
                : (inst.opcode == mir::Opcode::LessOrEqualInt
                    ? " <= "
                    : (inst.opcode == mir::Opcode::GreaterInt ? " > " : " >= "));
            source.line(ind + resDecl + " = (static_cast<std::int32_t>(" + op(0) + ")" +
                cmpOp + "static_cast<std::int32_t>(" + op(1) + "));");
            break;
        }
        case mir::Opcode::LessLong:
        case mir::Opcode::LessOrEqualLong:
        case mir::Opcode::GreaterLong:
        case mir::Opcode::GreaterOrEqualLong:
        case mir::Opcode::LessDouble:
        case mir::Opcode::LessOrEqualDouble:
        case mir::Opcode::GreaterDouble:
        case mir::Opcode::GreaterOrEqualDouble: {
            const char* cmpOp =
                (inst.opcode == mir::Opcode::LessLong || inst.opcode == mir::Opcode::LessDouble)
                ? " < "
                : ((inst.opcode == mir::Opcode::LessOrEqualLong ||
                    inst.opcode == mir::Opcode::LessOrEqualDouble)
                    ? " <= "
                    : ((inst.opcode == mir::Opcode::GreaterLong ||
                        inst.opcode == mir::Opcode::GreaterDouble)
                        ? " > "
                        : " >= "));
            source.line(ind + resDecl + " = (" + op(0) + cmpOp + op(1) + ");");
            break;
        }
        default:
            break;
        }
    }

    source.line("                            }");
    source.line("                            " + idxLocalExpr + " = vecEnd;");
    source.line("                        }");
    source.line("                        goto typedBlock_" + std::to_string(plan.exitBlockId) + ";");
    source.line("                    }");
    source.line("                }");
    source.line("            }");
}


} // namespace realscript::aot::detail
