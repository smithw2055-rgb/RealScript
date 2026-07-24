#include "FlowAnalysis.h"

#include <algorithm>
#include <unordered_set>

namespace realscript::semantic {
namespace {

using AssignedSet = std::unordered_set<std::size_t>;

AssignedSet intersectAssigned(const AssignedSet& left, const AssignedSet& right) {
    AssignedSet result;
    const auto& smaller = left.size() <= right.size() ? left : right;
    const auto& larger = left.size() <= right.size() ? right : left;
    for (const auto value : smaller) {
        if (larger.find(value) != larger.end()) {
            result.insert(value);
        }
    }
    return result;
}

bool isLiteralTrue(const BoundExpression& expression) {
    if (expression.kind() != BoundNodeKind::LiteralExpression ||
        expression.type != PrimitiveType::Bool) {
        return false;
    }
    const auto& literal = static_cast<const BoundLiteralExpression&>(expression);
    return std::holds_alternative<bool>(literal.value) && std::get<bool>(literal.value);
}

class FlowAnalyzer {
public:
    explicit FlowAnalyzer(diagnostics::DiagnosticBag& diagnostics)
        : diagnostics_(diagnostics) {}

    bool canReachFunctionEnd(const BoundFunction& function) {
        State state;
        for (const auto& parameter : function.symbol.parameters) {
            state.assigned.insert(parameter.index);
        }
        return analyzeStatement(*function.body, std::move(state)).reachable;
    }

private:
    struct State {
        AssignedSet assigned;
        bool reachable = true;
    };

    State analyzeStatement(const BoundStatement& statement, State state) {
        if (!state.reachable) {
            return state;
        }

        switch (statement.kind()) {
        case BoundNodeKind::BlockStatement: {
            const auto& block = static_cast<const BoundBlockStatement&>(statement);
            for (const auto& child : block.statements) {
                state = analyzeStatement(*child, std::move(state));
                if (!state.reachable) {
                    break;
                }
            }
            return state;
        }
        case BoundNodeKind::ReturnStatement: {
            const auto& returnStatement = static_cast<const BoundReturnStatement&>(statement);
            if (returnStatement.expression) {
                analyzeExpression(*returnStatement.expression, state.assigned);
            }
            state.reachable = false;
            return state;
        }
        case BoundNodeKind::VariableDeclarationStatement: {
            const auto& declaration =
                static_cast<const BoundVariableDeclarationStatement&>(statement);
            if (declaration.initializer) {
                analyzeExpression(*declaration.initializer, state.assigned);
                state.assigned.insert(declaration.variable.index);
            } else {
                state.assigned.erase(declaration.variable.index);
            }
            return state;
        }
        case BoundNodeKind::ExpressionStatement: {
            const auto& expressionStatement =
                static_cast<const BoundExpressionStatement&>(statement);
            analyzeExpression(*expressionStatement.expression, state.assigned);
            return state;
        }
        case BoundNodeKind::IfStatement: {
            const auto& ifStatement = static_cast<const BoundIfStatement&>(statement);
            analyzeExpression(*ifStatement.condition, state.assigned);

            State thenState{state.assigned, true};
            thenState = analyzeStatement(*ifStatement.thenStatement, std::move(thenState));

            State elseState{state.assigned, true};
            if (ifStatement.elseStatement) {
                elseState = analyzeStatement(*ifStatement.elseStatement, std::move(elseState));
            }

            if (!thenState.reachable && !elseState.reachable) {
                return {{}, false};
            }
            if (thenState.reachable && elseState.reachable) {
                return {intersectAssigned(thenState.assigned, elseState.assigned), true};
            }
            return thenState.reachable ? std::move(thenState) : std::move(elseState);
        }
        case BoundNodeKind::WhileStatement: {
            const auto& whileStatement = static_cast<const BoundWhileStatement&>(statement);
            analyzeExpression(*whileStatement.condition, state.assigned);
            State bodyState{state.assigned, true};
            (void)analyzeStatement(*whileStatement.body, std::move(bodyState));
            if (isLiteralTrue(*whileStatement.condition)) {
                state.reachable = false;
            }
            return state;
        }
        default:
            return state;
        }
    }

    void analyzeExpression(const BoundExpression& expression, AssignedSet& assigned) {
        switch (expression.kind()) {
        case BoundNodeKind::VariableExpression: {
            const auto& variable = static_cast<const BoundVariableExpression&>(expression).variable;
            if (assigned.find(variable.index) == assigned.end()) {
                diagnostics_.report(
                    "RS2300",
                    "variable '" + variable.name + "' is used before it is definitely assigned",
                    expression.span);
            }
            return;
        }
        case BoundNodeKind::AssignmentExpression: {
            const auto& assignment = static_cast<const BoundAssignmentExpression&>(expression);
            analyzeExpression(*assignment.expression, assigned);
            assigned.insert(assignment.variable.index);
            return;
        }
        case BoundNodeKind::UnaryExpression:
            analyzeExpression(*static_cast<const BoundUnaryExpression&>(expression).operand, assigned);
            return;
        case BoundNodeKind::BinaryExpression: {
            const auto& binary = static_cast<const BoundBinaryExpression&>(expression);
            analyzeExpression(*binary.left, assigned);
            if (binary.operatorKind == BoundBinaryOperatorKind::LogicalAnd ||
                binary.operatorKind == BoundBinaryOperatorKind::LogicalOr) {
                auto rightAssigned = assigned;
                analyzeExpression(*binary.right, rightAssigned);
            } else {
                analyzeExpression(*binary.right, assigned);
            }
            return;
        }
        default:
            return;
        }
    }

    diagnostics::DiagnosticBag& diagnostics_;
};


} // namespace

namespace detail {

bool canReachFunctionEnd(
    const BoundFunction& function,
    diagnostics::DiagnosticBag& diagnostics) {
    FlowAnalyzer analyzer(diagnostics);
    return analyzer.canReachFunctionEnd(function);
}

} // namespace detail

} // namespace realscript::semantic
