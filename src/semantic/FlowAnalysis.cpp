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
            const auto& variable = static_cast<const BoundVariableExpression&>(expression).variable;
            if (assigned.find(variable.index) == assigned.end())
                diagnostics_.report("RS2300", "variable '" + variable.name + "' is used before it is definitely assigned", expression.span);
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
                if (argument.value &&
                    argument.modifier != ParameterModifier::Out) {
                    analyzeExpression(*argument.value, assigned);
                }
                if (!argument.forwarded &&
                    (argument.modifier == ParameterModifier::Ref ||
                     argument.modifier == ParameterModifier::Out)) {
                    assigned.insert(argument.variable.index);
                }
            }
            return;
        }
        case BoundNodeKind::UnaryExpression:
            analyzeExpression(*static_cast<const BoundUnaryExpression&>(expression).operand, assigned); return;
        case BoundNodeKind::BinaryExpression: {
            const auto& value = static_cast<const BoundBinaryExpression&>(expression);
            analyzeExpression(*value.left, assigned);
            if (value.operatorKind == BoundBinaryOperatorKind::LogicalAnd ||
                value.operatorKind == BoundBinaryOperatorKind::LogicalOr) {
                auto copy = assigned; analyzeExpression(*value.right, copy);
            } else analyzeExpression(*value.right, assigned);
            return;
        }
        case BoundNodeKind::ConditionalExpression: {
            const auto& value = static_cast<const
                BoundConditionalExpression&>(expression);
            analyzeExpression(*value.condition, assigned);
            auto whenTrue = assigned;
            auto whenFalse = assigned;
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
            if (type.patternVariable) {
                assigned.insert(type.patternVariable->index);
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
                if (arm.label) analyzeExpression(*arm.label, armAssigned);
                if (arm.patternVariable) {
                    armAssigned.insert(arm.patternVariable->index);
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
        case BoundNodeKind::MemberAccessExpression:
            analyzeExpression(*static_cast<const BoundMemberAccessExpression&>(expression).receiver, assigned); return;
        case BoundNodeKind::MemberAssignmentExpression: {
            const auto& value = static_cast<const BoundMemberAssignmentExpression&>(expression);
            analyzeExpression(*value.receiver, assigned); analyzeExpression(*value.expression, assigned); return;
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
            analyzeExpression(*value.condition, state.assigned);
            auto left = analyzeStatement(*value.thenStatement, {state.assigned, ExitKind::None});
            auto right = value.elseStatement ? analyzeStatement(*value.elseStatement, {state.assigned, ExitKind::None})
                                             : State{state.assigned, ExitKind::None};
            if (left.exit == right.exit && left.exit != ExitKind::None) return {{}, left.exit};
            if (left.exit == ExitKind::None && right.exit == ExitKind::None)
                return {intersectAssigned(left.assigned, right.assigned), ExitKind::None};
            return left.exit == ExitKind::None ? left : right;
        }
        case BoundNodeKind::WhileStatement: {
            const auto& value = static_cast<const BoundWhileStatement&>(statement);
            analyzeExpression(*value.condition, state.assigned);
            (void)analyzeStatement(*value.body, {state.assigned, ExitKind::None});
            if (isLiteralTrue(*value.condition) && !containsBreakForCurrentTarget(*value.body)) state.exit = ExitKind::Return;
            return state;
        }
        case BoundNodeKind::ForStatement: {
            const auto& value = static_cast<const BoundForStatement&>(statement);
            if (value.initializer) state = analyzeStatement(*value.initializer, std::move(state));
            if (state.exit != ExitKind::None) return state;
            analyzeExpression(*value.condition, state.assigned);
            auto body = analyzeStatement(*value.body, {state.assigned, ExitKind::None});
            if (value.increment && body.exit != ExitKind::Return) analyzeExpression(*value.increment, body.assigned);
            if (isLiteralTrue(*value.condition) && !containsBreakForCurrentTarget(*value.body)) state.exit = ExitKind::Return;
            return state;
        }
        case BoundNodeKind::ForeachStatement: {
            const auto& value = static_cast<const BoundForeachStatement&>(statement);
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
                if (section.label) {
                    analyzeExpression(*section.label, sectionAssigned);
                } else if (section.patternType == PrimitiveType::Error) {
                    hasDefault = true;
                }
                if (section.patternVariable) {
                    sectionAssigned.insert(section.patternVariable->index);
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
