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
        case BoundNodeKind::CallExpression:
            for (const auto& argument :
                 static_cast<const BoundCallExpression&>(expression).arguments) {
                analyzeExpression(*argument, assigned);
            }
            return;
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
        case BoundNodeKind::EventSubscriptionStatement: {
            const auto& subscription = static_cast<const
                BoundEventSubscriptionStatement&>(statement);
            if (subscription.receiver) {
                analyzeExpression(*subscription.receiver, state.assigned);
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
                if (section.label) analyzeExpression(*section.label, state.assigned); else hasDefault = true;
                auto sectionState = analyzeBlock(section.statements, {state.assigned, ExitKind::None});
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
