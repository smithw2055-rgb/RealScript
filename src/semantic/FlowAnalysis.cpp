#include "FlowAnalysis.h"

#include <unordered_set>
#include <utility>

namespace realscript::semantic {
namespace {

using AssignedSet = std::unordered_set<std::size_t>;

enum class ExitKind { None, Return, Break, Continue };

AssignedSet intersectAssigned(const AssignedSet& left, const AssignedSet& right) {
    AssignedSet result;
    const auto& smaller = left.size() <= right.size() ? left : right;
    const auto& larger = left.size() <= right.size() ? right : left;
    for (const auto value : smaller) if (larger.find(value) != larger.end()) result.insert(value);
    return result;
}

bool isLiteralTrue(const BoundExpression& expression) {
    if (expression.kind() != BoundNodeKind::LiteralExpression ||
        expression.type != PrimitiveType::Bool) return false;
    const auto& literal = static_cast<const BoundLiteralExpression&>(expression);
    return std::holds_alternative<bool>(literal.value) && std::get<bool>(literal.value);
}

bool containsBreakForCurrentTarget(const BoundStatement& statement, std::size_t nestedBreakables = 0) {
    switch (statement.kind()) {
    case BoundNodeKind::BreakStatement:
        return nestedBreakables == 0;
    case BoundNodeKind::BlockStatement:
        for (const auto& child : static_cast<const BoundBlockStatement&>(statement).statements)
            if (containsBreakForCurrentTarget(*child, nestedBreakables)) return true;
        return false;
    case BoundNodeKind::IfStatement: {
        const auto& value = static_cast<const BoundIfStatement&>(statement);
        return containsBreakForCurrentTarget(*value.thenStatement, nestedBreakables) ||
            (value.elseStatement && containsBreakForCurrentTarget(*value.elseStatement, nestedBreakables));
    }
    case BoundNodeKind::TryStatement: {
        const auto& value = static_cast<const BoundTryStatement&>(statement);
        if (value.body && containsBreakForCurrentTarget(*value.body, nestedBreakables)) return true;
        for (const auto& clause : value.catches) {
            if (clause.body && containsBreakForCurrentTarget(*clause.body, nestedBreakables)) return true;
        }
        if (value.finallyBody && containsBreakForCurrentTarget(*value.finallyBody, nestedBreakables)) return true;
        return false;
    }
    case BoundNodeKind::WhileStatement:
    case BoundNodeKind::ForStatement:
    case BoundNodeKind::ForeachStatement:
    case BoundNodeKind::DoWhileStatement:
    case BoundNodeKind::SwitchStatement:
        return false;
    default:
        return false;
    }
}

class FlowAnalyzer {
public:
    explicit FlowAnalyzer(diagnostics::DiagnosticBag& diagnostics) : diagnostics_(diagnostics) {}

    bool canReachFunctionEnd(const BoundFunction& function) {
        State state;
        for (const auto& parameter : function.symbol.parameters) state.assigned.insert(parameter.index);
        return analyzeStatement(*function.body, std::move(state)).exit == ExitKind::None;
    }

private:
    struct State { AssignedSet assigned; ExitKind exit = ExitKind::None; };

    void analyzeExpression(const BoundExpression& expression, AssignedSet& assigned) {
        switch (expression.kind()) {
        case BoundNodeKind::VariableExpression: {
            const auto& varExpr = static_cast<const BoundVariableExpression&>(expression);
            if (!varExpr.syntheticOverwriteRead &&
                assigned.find(varExpr.variable.index) == assigned.end())
                diagnostics_.report("RS2300", "variable '" + varExpr.variable.name + "' is used before it is definitely assigned", expression.span);
            return;
        }
        case BoundNodeKind::AssignmentExpression: {
            const auto& value = static_cast<const BoundAssignmentExpression&>(expression);
            analyzeExpression(*value.expression, assigned);
            assigned.insert(value.variable.index);
            return;
        }
        case BoundNodeKind::ConversionExpression:
            analyzeExpression(*static_cast<const BoundConversionExpression&>(expression).expression, assigned); return;
        case BoundNodeKind::DelegateCreationExpression: {
            const auto& creation = static_cast<const
                BoundDelegateCreationExpression&>(expression);
            if (creation.receiver) {
                analyzeExpression(*creation.receiver, assigned);
            }
            for (const auto& capture : creation.captures) {
                analyzeExpression(*capture, assigned);
            }
            return;
        }
        case BoundNodeKind::DelegateInvocationExpression: {
            const auto& invocation = static_cast<const
                BoundDelegateInvocationExpression&>(expression);
            analyzeExpression(*invocation.delegate, assigned);
            for (const auto& argument : invocation.arguments) {
                if (argument.value) {
                    analyzeExpression(*argument.value, assigned);
                }
                if (argument.modifier == ParameterModifier::Out &&
                    argument.variable.id != 0) {
                    assigned.insert(argument.variable.index);
                }
            }
            return;
        }
        case BoundNodeKind::DelegateCombinationExpression: {
            const auto& combination = static_cast<const
                BoundDelegateCombinationExpression&>(expression);
            analyzeExpression(*combination.left, assigned);
            analyzeExpression(*combination.right, assigned);
            return;
        }
        case BoundNodeKind::CallExpression:
        {
            const auto& call = static_cast<const
                BoundCallExpression&>(expression);
            for (std::size_t index = 0; index < call.arguments.size(); ++index) {
                if (call.nullConditional && index != 0) {
                    auto conditional = assigned;
                    analyzeExpression(*call.arguments[index], conditional);
                } else {
                    analyzeExpression(*call.arguments[index], assigned);
                }
            }
            return;
        }
        case BoundNodeKind::EventInvocationExpression: {
            const auto& event = static_cast<const
                BoundEventInvocationExpression&>(expression);
            analyzeExpression(*event.receiver, assigned);
            for (const auto& argument : event.arguments) {
                analyzeExpression(*argument, assigned);
            }
            return;
        }
        case BoundNodeKind::ReferenceCallExpression: {
            const auto& call = static_cast<
                const BoundReferenceCallExpression&>(expression);
            for (const auto& argument : call.arguments) {
                if (argument.targetReceiver) {
                    analyzeExpression(*argument.targetReceiver, assigned);
                }
                if (argument.targetIndex) {
                    analyzeExpression(*argument.targetIndex, assigned);
                }
                if (argument.structRootStaticInitializer) {
                    analyzeExpression(*argument.structRootStaticInitializer, assigned);
                }
                if (argument.value &&
                    argument.modifier != ParameterModifier::Out) {
                    analyzeExpression(*argument.value, assigned);
                }
                if (!argument.forwarded &&
                    argument.targetKind == ReferenceTargetKind::Variable &&
                    (argument.modifier == ParameterModifier::Ref ||
                     argument.modifier == ParameterModifier::Out)) {
                    assigned.insert(argument.variable.index);
                }
            }
            return;
        }
        case BoundNodeKind::UnaryExpression:
            analyzeExpression(*static_cast<const BoundUnaryExpression&>(expression).operand, assigned); return;
        case BoundNodeKind::AwaitExpression:
            analyzeExpression(*static_cast<const BoundAwaitExpression&>(expression).operand, assigned); return;
        case BoundNodeKind::BinaryExpression: {
            const auto& value = static_cast<const BoundBinaryExpression&>(expression);
            if (value.operatorKind == BoundBinaryOperatorKind::LogicalAnd ||
                value.operatorKind == BoundBinaryOperatorKind::LogicalOr) {
                AssignedSet trueSet;
                AssignedSet falseSet;
                analyzeCondition(expression, assigned, trueSet, falseSet);
                assigned = intersectAssigned(trueSet, falseSet);
            } else {
                analyzeExpression(*value.left, assigned);
                analyzeExpression(*value.right, assigned);
            }
            return;
        }
        case BoundNodeKind::ConditionalExpression: {
            const auto& value = static_cast<const
                BoundConditionalExpression&>(expression);
            AssignedSet whenTrue;
            AssignedSet whenFalse;
            analyzeCondition(*value.condition, assigned, whenTrue, whenFalse);
            analyzeExpression(*value.whenTrue, whenTrue);
            analyzeExpression(*value.whenFalse, whenFalse);
            assigned = intersectAssigned(whenTrue, whenFalse);
            return;
        }
        case BoundNodeKind::NullCoalescingExpression: {
            const auto& value = static_cast<const
                BoundNullCoalescingExpression&>(expression);
            analyzeExpression(*value.left, assigned);
            auto fallback = assigned;
            analyzeExpression(*value.right, fallback);
            return;
        }
        case BoundNodeKind::TypeBinaryExpression:
        {
            const auto& type = static_cast<const
                BoundTypeBinaryExpression&>(expression);
            analyzeExpression(*type.expression, assigned);
            if (type.pattern) {
                analyzePatternSubexpressions(*type.pattern, assigned);
            }
            return;
        }
        case BoundNodeKind::TypeOfExpression:
            return;
        case BoundNodeKind::SwitchExpression: {
            const auto& value = static_cast<const
                BoundSwitchExpression&>(expression);
            analyzeExpression(*value.expression, assigned);
            AssignedSet merged;
            bool first = true;
            for (const auto& arm : value.arms) {
                auto armAssigned = assigned;
                if (arm.pattern) {
                    analyzePatternSubexpressions(*arm.pattern, armAssigned);
                    collectPatternDefiniteVariables(*arm.pattern, armAssigned);
                } else {
                    if (arm.label) analyzeExpression(*arm.label, armAssigned);
                    if (arm.patternVariable) {
                        armAssigned.insert(arm.patternVariable->index);
                    }
                }
                if (arm.guard) analyzeExpression(*arm.guard, armAssigned);
                analyzeExpression(*arm.value, armAssigned);
                merged = first ? std::move(armAssigned)
                               : intersectAssigned(merged, armAssigned);
                first = false;
            }
            if (!first) assigned = std::move(merged);
            return;
        }
        case BoundNodeKind::NewObjectExpression: {
            const auto& value = static_cast<const
                BoundNewObjectExpression&>(expression);
            for (const auto& initializer : value.defaultFieldInitializers) {
                for (const auto& argument : initializer.arguments) {
                    analyzeExpression(*argument, assigned);
                }
            }
            for (const auto& argument : value.arguments) {
                analyzeExpression(*argument, assigned);
            }
            for (const auto& initializer : value.initializers) {
                for (const auto& argument : initializer.arguments) {
                    analyzeExpression(*argument, assigned);
                }
            }
            return;
        }
        case BoundNodeKind::NewStructExpression: {
            const auto& value = static_cast<const
                BoundNewStructExpression&>(expression);
            for (const auto& argument : value.arguments) {
                analyzeExpression(*argument, assigned);
            }
            for (const auto& initializer : value.initializers) {
                for (const auto& argument : initializer.arguments) {
                    analyzeExpression(*argument, assigned);
                }
            }
            return;
        }
        case BoundNodeKind::NewArrayExpression: {
            const auto& value = static_cast<const
                BoundNewArrayExpression&>(expression);
            analyzeExpression(*value.length, assigned);
            for (const auto& item : value.initialValues) {
                analyzeExpression(*item, assigned);
            }
            return;
        }
        case BoundNodeKind::MemberAccessExpression: {
            const auto& value = static_cast<const BoundMemberAccessExpression&>(expression);
            if (value.receiver) analyzeExpression(*value.receiver, assigned);
            if (value.staticInitializer) analyzeExpression(*value.staticInitializer, assigned);
            return;
        }
        case BoundNodeKind::MemberAssignmentExpression: {
            const auto& value = static_cast<const BoundMemberAssignmentExpression&>(expression);
            if (value.ownerType.synthetic &&
                value.ownerType.fields.size() == 1 &&
                value.field.name == "Value" &&
                value.receiver &&
                value.receiver->kind() == BoundNodeKind::VariableExpression) {
                analyzeExpression(*value.expression, assigned);
                assigned.insert(
                    static_cast<const BoundVariableExpression&>(*value.receiver)
                        .variable.index);
                return;
            }
            if (value.receiver) analyzeExpression(*value.receiver, assigned);
            analyzeExpression(*value.expression, assigned);
            return;
        }
        case BoundNodeKind::StructFieldAccessExpression: {
            const auto& value = static_cast<const BoundStructFieldAccessExpression&>(expression);
            if (value.receiver) analyzeExpression(*value.receiver, assigned);
            return;
        }
        case BoundNodeKind::StructFieldAssignmentExpression: {
            const auto& value = static_cast<const BoundStructFieldAssignmentExpression&>(expression);
            if (value.objectReceiver) {
                analyzeExpression(*value.objectReceiver, assigned);
            }
            if (value.elementReceiver) {
                analyzeExpression(*value.elementReceiver, assigned);
            }
            if (value.elementIndex) {
                analyzeExpression(*value.elementIndex, assigned);
            }
            if (value.staticReceiverInitializer) {
                analyzeExpression(*value.staticReceiverInitializer, assigned);
            }
            if (value.expression) analyzeExpression(*value.expression, assigned);
            return;
        }
        case BoundNodeKind::ArrayLengthExpression:
            analyzeExpression(*static_cast<const BoundArrayLengthExpression&>(expression).receiver, assigned); return;
        case BoundNodeKind::ElementAccessExpression: {
            const auto& value = static_cast<const BoundElementAccessExpression&>(expression);
            analyzeExpression(*value.receiver, assigned); analyzeExpression(*value.index, assigned); return;
        }
        case BoundNodeKind::ElementAssignmentExpression: {
            const auto& value = static_cast<const BoundElementAssignmentExpression&>(expression);
            analyzeExpression(*value.receiver, assigned); analyzeExpression(*value.index, assigned);
            analyzeExpression(*value.expression, assigned); return;
        }
        default: return;
        }
    }

    void analyzePatternSubexpressions(
        const BoundPattern& pattern,
        AssignedSet& assigned) {
        if (pattern.constantValue) {
            analyzeExpression(*pattern.constantValue, assigned);
        }
        if (pattern.left) analyzePatternSubexpressions(*pattern.left, assigned);
        if (pattern.right) analyzePatternSubexpressions(*pattern.right, assigned);
        for (const auto& prop : pattern.properties) {
            if (prop.pattern) analyzePatternSubexpressions(*prop.pattern, assigned);
        }
    }

    void collectPatternDefiniteVariables(
        const BoundPattern& pattern,
        AssignedSet& assignedWhenMatched) {
        switch (pattern.kind) {
        case BoundPatternKind::Var:
        case BoundPatternKind::Type:
            if (pattern.variable) {
                assignedWhenMatched.insert(pattern.variable->index);
            }
            return;
        case BoundPatternKind::Property:
            if (pattern.variable) {
                assignedWhenMatched.insert(pattern.variable->index);
            }
            for (const auto& prop : pattern.properties) {
                if (prop.pattern) {
                    collectPatternDefiniteVariables(*prop.pattern, assignedWhenMatched);
                }
            }
            return;
        case BoundPatternKind::LogicalAnd:
            if (pattern.left) collectPatternDefiniteVariables(*pattern.left, assignedWhenMatched);
            if (pattern.right) collectPatternDefiniteVariables(*pattern.right, assignedWhenMatched);
            return;
        default:
            return;
        }
    }

    void analyzeCondition(
        const BoundExpression& condition,
        const AssignedSet& incoming,
        AssignedSet& whenTrue,
        AssignedSet& whenFalse) {
        if (condition.kind() == BoundNodeKind::ConversionExpression) {
            const auto& conv = static_cast<const BoundConversionExpression&>(condition);
            if (conv.expression) {
                analyzeCondition(*conv.expression, incoming, whenTrue, whenFalse);
                return;
            }
        }
        if (condition.kind() == BoundNodeKind::UnaryExpression) {
            const auto& un = static_cast<const BoundUnaryExpression&>(condition);
            if (un.operatorKind == BoundUnaryOperatorKind::LogicalNegation && un.operand) {
                analyzeCondition(*un.operand, incoming, whenFalse, whenTrue);
                return;
            }
        }
        if (condition.kind() == BoundNodeKind::BinaryExpression) {
            const auto& bin = static_cast<const BoundBinaryExpression&>(condition);
            if (bin.operatorKind == BoundBinaryOperatorKind::LogicalAnd) {
                AssignedSet leftTrue;
                AssignedSet leftFalse;
                analyzeCondition(*bin.left, incoming, leftTrue, leftFalse);
                AssignedSet rightTrue;
                AssignedSet rightFalse;
                analyzeCondition(*bin.right, leftTrue, rightTrue, rightFalse);
                whenTrue = std::move(rightTrue);
                whenFalse = intersectAssigned(leftFalse, rightFalse);
                return;
            }
            if (bin.operatorKind == BoundBinaryOperatorKind::LogicalOr) {
                AssignedSet leftTrue;
                AssignedSet leftFalse;
                analyzeCondition(*bin.left, incoming, leftTrue, leftFalse);
                AssignedSet rightTrue;
                AssignedSet rightFalse;
                analyzeCondition(*bin.right, leftFalse, rightTrue, rightFalse);
                whenTrue = intersectAssigned(leftTrue, rightTrue);
                whenFalse = std::move(rightFalse);
                return;
            }
        }
        if (condition.kind() == BoundNodeKind::TypeBinaryExpression) {
            const auto& tb = static_cast<const BoundTypeBinaryExpression&>(condition);
            auto base = incoming;
            analyzeExpression(*tb.expression, base);
            if (tb.pattern) {
                analyzePatternSubexpressions(*tb.pattern, base);
                whenFalse = base;
                whenTrue = std::move(base);
                collectPatternDefiniteVariables(*tb.pattern, whenTrue);
                return;
            }
            whenFalse = base;
            whenTrue = std::move(base);
            if (tb.patternVariable) {
                whenTrue.insert(tb.patternVariable->index);
            }
            return;
        }
        auto base = incoming;
        analyzeExpression(condition, base);
        whenTrue = base;
        whenFalse = std::move(base);
    }

    State analyzeBlock(const std::vector<std::unique_ptr<BoundStatement>>& statements, State state) {
        for (const auto& child : statements) {
            if (state.exit != ExitKind::None) break;
            state = analyzeStatement(*child, std::move(state));
        }
        return state;
    }

    State analyzeStatement(const BoundStatement& statement, State state) {
        if (state.exit != ExitKind::None) return state;
        switch (statement.kind()) {
        case BoundNodeKind::BlockStatement:
            return analyzeBlock(static_cast<const BoundBlockStatement&>(statement).statements, std::move(state));
        case BoundNodeKind::ReturnStatement: {
            const auto& value = static_cast<const BoundReturnStatement&>(statement);
            if (value.expression) analyzeExpression(*value.expression, state.assigned);
            state.exit = ExitKind::Return; return state;
        }
        case BoundNodeKind::BreakStatement: state.exit = ExitKind::Break; return state;
        case BoundNodeKind::ContinueStatement: state.exit = ExitKind::Continue; return state;
        case BoundNodeKind::ThrowStatement: {
            const auto& value = static_cast<const BoundThrowStatement&>(statement);
            analyzeExpression(*value.expression, state.assigned);
            state.exit = ExitKind::Return;
            return state;
        }
        case BoundNodeKind::TryStatement: {
            const auto& value = static_cast<const BoundTryStatement&>(statement);
            std::vector<State> paths;
            paths.push_back(analyzeStatement(
                *value.body, {state.assigned, ExitKind::None}));
            for (const auto& clause : value.catches) {
                auto catchAssigned = state.assigned;
                catchAssigned.insert(clause.exceptionVariable.index);
                paths.push_back(analyzeStatement(
                    *clause.body,
                    {std::move(catchAssigned), ExitKind::None}));
            }
            State merged = paths.front();
            for (std::size_t index = 1; index < paths.size(); ++index) {
                if (merged.exit == ExitKind::None &&
                    paths[index].exit == ExitKind::None) {
                    merged.assigned = intersectAssigned(
                        merged.assigned, paths[index].assigned);
                } else if (merged.exit != paths[index].exit) {
                    merged.exit = ExitKind::None;
                }
            }
            if (value.finallyBody) {
                auto finalState = analyzeStatement(
                    *value.finallyBody,
                    {merged.assigned, ExitKind::None});
                if (finalState.exit != ExitKind::None) return finalState;
                merged.assigned = std::move(finalState.assigned);
            }
            return merged;
        }
        case BoundNodeKind::EventSubscriptionStatement: {
            const auto& subscription = static_cast<const
                BoundEventSubscriptionStatement&>(statement);
            if (subscription.receiver) {
                analyzeExpression(*subscription.receiver, state.assigned);
            }
            if (subscription.handler) {
                analyzeExpression(*subscription.handler, state.assigned);
            }
            return state;
        }
        case BoundNodeKind::VariableDeclarationStatement: {
            const auto& value = static_cast<const BoundVariableDeclarationStatement&>(statement);
            if (value.initializer) { analyzeExpression(*value.initializer, state.assigned); state.assigned.insert(value.variable.index); }
            else state.assigned.erase(value.variable.index);
            return state;
        }
        case BoundNodeKind::ExpressionStatement:
            analyzeExpression(*static_cast<const BoundExpressionStatement&>(statement).expression, state.assigned); return state;
        case BoundNodeKind::IfStatement: {
            const auto& value = static_cast<const BoundIfStatement&>(statement);
            AssignedSet trueAssigned;
            AssignedSet falseAssigned;
            analyzeCondition(*value.condition, state.assigned, trueAssigned, falseAssigned);
            auto left = analyzeStatement(*value.thenStatement, {std::move(trueAssigned), ExitKind::None});
            auto right = value.elseStatement ? analyzeStatement(*value.elseStatement, {std::move(falseAssigned), ExitKind::None})
                                             : State{std::move(falseAssigned), ExitKind::None};
            if (left.exit == right.exit && left.exit != ExitKind::None) return {{}, left.exit};
            if (left.exit == ExitKind::None && right.exit == ExitKind::None)
                return {intersectAssigned(left.assigned, right.assigned), ExitKind::None};
            return left.exit == ExitKind::None ? left : right;
        }
        case BoundNodeKind::WhileStatement: {
            const auto& value = static_cast<const BoundWhileStatement&>(statement);
            AssignedSet trueAssigned;
            AssignedSet falseAssigned;
            analyzeCondition(*value.condition, state.assigned, trueAssigned, falseAssigned);
            (void)analyzeStatement(*value.body, {std::move(trueAssigned), ExitKind::None});
            state.assigned = std::move(falseAssigned);
            if (isLiteralTrue(*value.condition) && !containsBreakForCurrentTarget(*value.body)) state.exit = ExitKind::Return;
            return state;
        }
        case BoundNodeKind::ForStatement: {
            const auto& value = static_cast<const BoundForStatement&>(statement);
            if (value.initializer) state = analyzeStatement(*value.initializer, std::move(state));
            if (state.exit != ExitKind::None) return state;
            AssignedSet trueAssigned;
            AssignedSet falseAssigned;
            analyzeCondition(*value.condition, state.assigned, trueAssigned, falseAssigned);
            auto body = analyzeStatement(*value.body, {std::move(trueAssigned), ExitKind::None});
            if (value.increment && body.exit != ExitKind::Return) analyzeExpression(*value.increment, body.assigned);
            state.assigned = std::move(falseAssigned);
            if (isLiteralTrue(*value.condition) && !containsBreakForCurrentTarget(*value.body)) state.exit = ExitKind::Return;
            return state;
        }
        case BoundNodeKind::ForeachStatement: {
            const auto& value = static_cast<const BoundForeachStatement&>(statement);
            if (value.isEcsQuery) {
                state.assigned.insert(value.indexVariable.index);
                auto bodyAssigned = state.assigned;
                if (value.hasEcsEntityBinding) {
                    state.assigned.insert(value.ecsEntityCollectionVariable.index);
                    bodyAssigned.insert(value.ecsEntityCollectionVariable.index);
                    bodyAssigned.insert(value.ecsEntityVariable.index);
                }
                for (const auto& col : value.ecsColumns) {
                    state.assigned.insert(col.collectionVariable.index);
                    bodyAssigned.insert(col.collectionVariable.index);
                    bodyAssigned.insert(col.iterationVariable.index);
                }
                (void)analyzeStatement(*value.body, {std::move(bodyAssigned), ExitKind::None});
                return state;
            }
            analyzeExpression(*value.collection, state.assigned);
            state.assigned.insert(value.collectionVariable.index);
            state.assigned.insert(value.indexVariable.index);
            auto bodyAssigned = state.assigned;
            analyzeExpression(*value.count, bodyAssigned);
            analyzeExpression(*value.element, bodyAssigned);
            bodyAssigned.insert(value.iterationVariable.index);
            (void)analyzeStatement(*value.body, {std::move(bodyAssigned), ExitKind::None});
            return state;
        }
        case BoundNodeKind::DoWhileStatement: {
            const auto& value = static_cast<const BoundDoWhileStatement&>(statement);
            auto body = analyzeStatement(*value.body, {state.assigned, ExitKind::None});
            if (body.exit == ExitKind::Return) return body;
            analyzeExpression(*value.condition, body.assigned);
            if (isLiteralTrue(*value.condition) && !containsBreakForCurrentTarget(*value.body)) body.exit = ExitKind::Return;
            else body.exit = ExitKind::None;
            return body;
        }
        case BoundNodeKind::SwitchStatement: {
            const auto& value = static_cast<const BoundSwitchStatement&>(statement);
            analyzeExpression(*value.expression, state.assigned);
            bool hasDefault = false;
            bool hasReachable = false;
            AssignedSet merged;
            bool first = true;
            for (const auto& section : value.sections) {
                auto sectionAssigned = state.assigned;
                if (section.pattern) {
                    analyzePatternSubexpressions(*section.pattern, sectionAssigned);
                    collectPatternDefiniteVariables(*section.pattern, sectionAssigned);
                    if ((section.pattern->kind == BoundPatternKind::Discard ||
                         section.pattern->kind == BoundPatternKind::Var) &&
                        !section.guard) {
                        hasDefault = true;
                    }
                } else {
                    if (section.label) {
                        analyzeExpression(*section.label, sectionAssigned);
                    } else if (section.patternType == PrimitiveType::Error && !section.guard) {
                        hasDefault = true;
                    }
                    if (section.patternVariable) {
                        sectionAssigned.insert(section.patternVariable->index);
                    }
                }
                if (section.guard) {
                    analyzeExpression(*section.guard, sectionAssigned);
                }
                auto sectionState = analyzeBlock(
                    section.statements,
                    {std::move(sectionAssigned), ExitKind::None});
                if (sectionState.exit == ExitKind::Break) sectionState.exit = ExitKind::None;
                if (sectionState.exit == ExitKind::None) {
                    hasReachable = true;
                    merged = first ? sectionState.assigned : intersectAssigned(merged, sectionState.assigned);
                    first = false;
                }
            }
            if (!hasDefault) { hasReachable = true; merged = first ? state.assigned : intersectAssigned(merged, state.assigned); }
            return hasReachable ? State{std::move(merged), ExitKind::None} : State{{}, ExitKind::Return};
        }
        default: return state;
        }
    }

    diagnostics::DiagnosticBag& diagnostics_;
};

} // namespace

namespace detail {
bool canReachFunctionEnd(const BoundFunction& function, diagnostics::DiagnosticBag& diagnostics) {
    FlowAnalyzer analyzer(diagnostics);
    return analyzer.canReachFunctionEnd(function);
}
} // namespace detail
} // namespace realscript::semantic
