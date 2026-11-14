#include "CompilationInternal.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::compiler::detail {

namespace {

semantic::VariableSymbol makeSequenceThisParameter(
    const semantic::TypeSymbol& owner,
    text::TextSpan span) {
    semantic::VariableSymbol parameter;
    parameter.name = "this";
    parameter.type = owner.kind == semantic::TypeKind::Struct
        ? semantic::PrimitiveType::Struct
        : semantic::PrimitiveType::Object;
    parameter.typeName = semantic::canonicalTypeName(owner);
    parameter.index = 0;
    parameter.parameter = true;
    parameter.declarationSpan = span;
    return parameter;
}

semantic::FunctionSymbol makeSequenceFunction(
    const std::string& moduleName,
    const semantic::TypeSymbol& owner,
    const syntax::SequenceDeclarationSyntax& sequence,
    std::string name,
    bool entry,
    const std::string& sourceName,
    const std::vector<InterfaceTypeRef>& parameterTypes = {}) {
    semantic::FunctionSymbol result;
    result.moduleName = moduleName;
    result.name = std::move(name);
    result.ownerTypeName = owner.name;
    result.ownerTypeId = owner.id;
    result.returnType = semantic::PrimitiveType::Void;
    result.method = true;
    result.deterministic = owner.deterministic;
    result.sourceName = sourceName;
    result.declarationSpan = sequence.identifierToken.span;
    result.bodySpan = sequence.body.span();
    result.parameters.push_back(makeSequenceThisParameter(
        owner,
        sequence.identifierToken.span));
    if (entry) {
        for (std::size_t index = 0;
             index < sequence.parameters.size() && index < parameterTypes.size();
             ++index) {
            semantic::VariableSymbol parameter;
            parameter.name = sequence.parameters[index].identifierToken.text;
            parameter.type = parameterTypes[index].type;
            parameter.typeName = parameterTypes[index].typeName;
            parameter.index = index + 1;
            parameter.parameter = true;
            parameter.declarationSpan =
                sequence.parameters[index].identifierToken.span;
            result.parameters.push_back(std::move(parameter));
        }
    }
    result.id = semantic::stableFunctionId(result);
    for (auto& parameter : result.parameters) {
        parameter.id = semantic::stableTypeId(
            std::to_string(result.id) + "::local:" +
            std::to_string(parameter.index) + ":" + parameter.name);
    }
    return result;
}

semantic::FunctionSymbol makeSequenceCancellationFunction(
    const std::string& moduleName,
    const semantic::TypeSymbol& owner,
    const syntax::SequenceDeclarationSyntax& sequence,
    const std::string& sourceName) {
    auto result = makeSequenceFunction(
        moduleName,
        owner,
        sequence,
        "Cancel" + sequence.identifierToken.text,
        false,
        sourceName);
    result.returnType = semantic::PrimitiveType::Bool;
    result.synthetic = true;
    result.id = semantic::stableFunctionId(result);
    for (auto& parameter : result.parameters) {
        parameter.id = semantic::stableTypeId(
            std::to_string(result.id) + "::local:" +
            std::to_string(parameter.index) + ":" + parameter.name);
    }
    return result;
}

std::vector<const syntax::YieldWaitStatementSyntax*> sequenceYields(
    const syntax::SequenceDeclarationSyntax& sequence,
    diagnostics::DiagnosticBag& diagnostics,
    const std::string& sourceName) {
    std::vector<const syntax::YieldWaitStatementSyntax*> result;
    std::function<void(const syntax::StatementSyntax&)> collect;
    collect = [&](const syntax::StatementSyntax& statement) {
        switch (statement.kind()) {
        case syntax::SyntaxKind::YieldWaitStatement:
            result.push_back(static_cast<const
                syntax::YieldWaitStatementSyntax*>(&statement));
            return;
        case syntax::SyntaxKind::YieldBreakStatement:
            return;
        case syntax::SyntaxKind::BlockStatement:
            for (const auto& child : static_cast<const
                 syntax::BlockStatementSyntax&>(statement).statements) {
                if (child->kind() ==
                        syntax::SyntaxKind::YieldBreakStatement ||
                    child->kind() ==
                        syntax::SyntaxKind::ReturnStatement) break;
                collect(*child);
            }
            return;
        case syntax::SyntaxKind::IfStatement: {
            const auto& value = static_cast<const
                syntax::IfStatementSyntax&>(statement);
            collect(*value.thenStatement);
            if (value.elseStatement) collect(*value.elseStatement);
            return;
        }
        case syntax::SyntaxKind::WhileStatement:
            collect(*static_cast<const syntax::WhileStatementSyntax&>(
                statement).body);
            return;
        case syntax::SyntaxKind::ForStatement: {
            const auto& value = static_cast<const
                syntax::ForStatementSyntax&>(statement);
            if (value.initializer) collect(*value.initializer);
            collect(*value.body);
            return;
        }
        case syntax::SyntaxKind::ForeachStatement:
            collect(*static_cast<const syntax::ForeachStatementSyntax&>(
                statement).body);
            return;
        case syntax::SyntaxKind::DoWhileStatement:
            collect(*static_cast<const syntax::DoWhileStatementSyntax&>(
                statement).body);
            return;
        case syntax::SyntaxKind::SwitchStatement:
            for (const auto& section : static_cast<const
                 syntax::SwitchStatementSyntax&>(statement).sections) {
                for (const auto& child : section.statements) collect(*child);
            }
            return;
        case syntax::SyntaxKind::TryStatement: {
            const auto& value = static_cast<const
                syntax::TryStatementSyntax&>(statement);
            collect(value.body);
            for (const auto& c : value.catches) collect(c.body);
            if (value.finallyBody) collect(*value.finallyBody);
            return;
        }
        default:
            return;
        }
    };
    collect(sequence.body);
    if (sequence.parameters.empty() ||
        sequence.parameters.front().type.name.text != "long" ||
        sequence.parameters.front().type.isArray()) {
        diagnostics.report(
            "RS2490",
            "sequence must declare a long target as its first parameter",
            sequence.identifierToken.span,
            diagnostics::DiagnosticSeverity::Error,
            sourceName);
    }
    return result;
}

bool isSingleYieldLoopSequence(
    const syntax::SequenceDeclarationSyntax& sequence) {
    std::size_t loopCount = 0;
    std::size_t yieldCount = 0;
    for (const auto& statement : sequence.body.statements) {
        if (statement->kind() == syntax::SyntaxKind::YieldWaitStatement) {
            ++yieldCount;
            continue;
        }
        if (statement->kind() != syntax::SyntaxKind::WhileStatement) continue;
        ++loopCount;
        const auto& loop = static_cast<const
            syntax::WhileStatementSyntax&>(*statement);
        if (loop.body->kind() != syntax::SyntaxKind::BlockStatement) return false;
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(*loop.body).statements) {
            if (child->kind() == syntax::SyntaxKind::YieldWaitStatement) {
                ++yieldCount;
            }
        }
    }
    return loopCount == 1 && yieldCount == 1;
}

bool isSingleYieldBranchSequence(
    const syntax::SequenceDeclarationSyntax& sequence) {
    std::size_t branchCount = 0;
    std::size_t yieldCount = 0;
    const auto countDirect = [&](const syntax::StatementSyntax* arm) {
        if (!arm || arm->kind() != syntax::SyntaxKind::BlockStatement) return;
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(*arm).statements) {
            if (child->kind() == syntax::SyntaxKind::YieldWaitStatement) {
                ++yieldCount;
            }
        }
    };
    for (const auto& statement : sequence.body.statements) {
        if (statement->kind() == syntax::SyntaxKind::YieldWaitStatement ||
            statement->kind() == syntax::SyntaxKind::WhileStatement) {
            return false;
        }
        if (statement->kind() != syntax::SyntaxKind::IfStatement) continue;
        ++branchCount;
        const auto& branch = static_cast<const
            syntax::IfStatementSyntax&>(*statement);
        countDirect(branch.thenStatement.get());
        countDirect(branch.elseStatement.get());
    }
    return branchCount == 1 && yieldCount == 1;
}

bool isSequenceCompositionCall(
    const syntax::StatementSyntax& statement,
    const std::unordered_set<std::string>& sequenceNames,
    std::string* targetName = nullptr) {
    if (statement.kind() != syntax::SyntaxKind::ExpressionStatement) {
        return false;
    }
    const auto& expression = static_cast<const
        syntax::ExpressionStatementSyntax&>(statement);
    if (!expression.expression ||
        expression.expression->kind() !=
            syntax::SyntaxKind::CallExpression) {
        return false;
    }
    const auto& call = static_cast<const syntax::CallExpressionSyntax&>(
        *expression.expression);
    if (sequenceNames.find(call.identifierToken.text) ==
        sequenceNames.end()) {
        return false;
    }
    if (targetName) *targetName = call.identifierToken.text;
    return true;
}

void collectSequenceCompositionCalls(
    const syntax::StatementSyntax& statement,
    const std::unordered_set<std::string>& sequenceNames,
    std::vector<const syntax::StatementSyntax*>& output,
    std::unordered_set<std::string>* targets = nullptr) {
    std::string target;
    if (isSequenceCompositionCall(statement, sequenceNames, &target)) {
        output.push_back(&statement);
        if (targets) targets->insert(std::move(target));
        return;
    }
    switch (statement.kind()) {
    case syntax::SyntaxKind::BlockStatement:
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(statement).statements) {
            collectSequenceCompositionCalls(
                *child, sequenceNames, output, targets);
        }
        return;
    case syntax::SyntaxKind::IfStatement: {
        const auto& branch = static_cast<const
            syntax::IfStatementSyntax&>(statement);
        collectSequenceCompositionCalls(
            *branch.thenStatement, sequenceNames, output, targets);
        if (branch.elseStatement) {
            collectSequenceCompositionCalls(
                *branch.elseStatement, sequenceNames, output, targets);
        }
        return;
    }
    case syntax::SyntaxKind::WhileStatement:
        collectSequenceCompositionCalls(
            *static_cast<const syntax::WhileStatementSyntax&>(
                statement).body,
            sequenceNames, output, targets);
        return;
    case syntax::SyntaxKind::ForStatement:
        collectSequenceCompositionCalls(
            *static_cast<const syntax::ForStatementSyntax&>(
                statement).body,
            sequenceNames, output, targets);
        return;
    case syntax::SyntaxKind::ForeachStatement:
        collectSequenceCompositionCalls(
            *static_cast<const syntax::ForeachStatementSyntax&>(
                statement).body,
            sequenceNames, output, targets);
        return;
    case syntax::SyntaxKind::DoWhileStatement:
        collectSequenceCompositionCalls(
            *static_cast<const syntax::DoWhileStatementSyntax&>(
                statement).body,
            sequenceNames, output, targets);
        return;
    case syntax::SyntaxKind::SwitchStatement:
        for (const auto& section : static_cast<const
             syntax::SwitchStatementSyntax&>(statement).sections) {
            for (const auto& child : section.statements) {
                collectSequenceCompositionCalls(
                    *child, sequenceNames, output, targets);
            }
        }
        return;
    case syntax::SyntaxKind::TryStatement: {
        const auto& value = static_cast<const
            syntax::TryStatementSyntax&>(statement);
        collectSequenceCompositionCalls(
            value.body, sequenceNames, output, targets);
        for (const auto& c : value.catches) {
            collectSequenceCompositionCalls(
                c.body, sequenceNames, output, targets);
        }
        if (value.finallyBody) {
            collectSequenceCompositionCalls(
                *value.finallyBody, sequenceNames, output, targets);
        }
        return;
    }
    default:
        return;
    }
}

bool sequenceStatementAlwaysReturnsResult(
    const syntax::StatementSyntax& statement) {
    switch (statement.kind()) {
    case syntax::SyntaxKind::ReturnStatement:
        return static_cast<const syntax::ReturnStatementSyntax&>(
            statement).expression != nullptr;
    case syntax::SyntaxKind::BlockStatement:
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(statement).statements) {
            if (sequenceStatementAlwaysReturnsResult(*child)) return true;
        }
        return false;
    case syntax::SyntaxKind::IfStatement: {
        const auto& branch = static_cast<const
            syntax::IfStatementSyntax&>(statement);
        return branch.elseStatement &&
            sequenceStatementAlwaysReturnsResult(
                *branch.thenStatement) &&
            sequenceStatementAlwaysReturnsResult(
                *branch.elseStatement);
    }
    case syntax::SyntaxKind::SwitchStatement: {
        const auto& value = static_cast<const
            syntax::SwitchStatementSyntax&>(statement);
        bool hasDefault = false;
        if (value.sections.empty()) return false;
        for (const auto& section : value.sections) {
            hasDefault = hasDefault || !section.label;
            bool returns = false;
            for (const auto& child : section.statements) {
                if (sequenceStatementAlwaysReturnsResult(*child)) {
                    returns = true;
                    break;
                }
            }
            if (!returns) return false;
        }
        return hasDefault;
    }
    case syntax::SyntaxKind::TryStatement: {
        const auto& value = static_cast<const
            syntax::TryStatementSyntax&>(statement);
        if (value.finallyBody &&
            sequenceStatementAlwaysReturnsResult(*value.finallyBody)) {
            return true;
        }
        if (!sequenceStatementAlwaysReturnsResult(value.body)) return false;
        for (const auto& c : value.catches) {
            if (!sequenceStatementAlwaysReturnsResult(c.body)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

void collectSequenceLocals(
    const syntax::StatementSyntax& statement,
    std::vector<const syntax::VariableDeclarationStatementSyntax*>& output) {
    switch (statement.kind()) {
    case syntax::SyntaxKind::VariableDeclarationStatement:
        output.push_back(static_cast<const
            syntax::VariableDeclarationStatementSyntax*>(&statement));
        return;
    case syntax::SyntaxKind::BlockStatement:
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(statement).statements) {
            collectSequenceLocals(*child, output);
        }
        return;
    case syntax::SyntaxKind::IfStatement: {
        const auto& value = static_cast<const syntax::IfStatementSyntax&>(statement);
        collectSequenceLocals(*value.thenStatement, output);
        if (value.elseStatement) collectSequenceLocals(*value.elseStatement, output);
        return;
    }
    case syntax::SyntaxKind::WhileStatement:
        collectSequenceLocals(*static_cast<const
            syntax::WhileStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::ForStatement: {
        const auto& value = static_cast<const syntax::ForStatementSyntax&>(statement);
        if (value.initializer) collectSequenceLocals(*value.initializer, output);
        collectSequenceLocals(*value.body, output);
        return;
    }
    case syntax::SyntaxKind::ForeachStatement:
        collectSequenceLocals(*static_cast<const
            syntax::ForeachStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::DoWhileStatement:
        collectSequenceLocals(*static_cast<const
            syntax::DoWhileStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::SwitchStatement:
        for (const auto& section : static_cast<const
             syntax::SwitchStatementSyntax&>(statement).sections) {
            for (const auto& child : section.statements) {
                collectSequenceLocals(*child, output);
            }
        }
        return;
    case syntax::SyntaxKind::TryStatement: {
        const auto& value = static_cast<const
            syntax::TryStatementSyntax&>(statement);
        collectSequenceLocals(value.body, output);
        for (const auto& c : value.catches) {
            collectSequenceLocals(c.body, output);
        }
        if (value.finallyBody) {
            collectSequenceLocals(*value.finallyBody, output);
        }
        return;
    }
    default:
        return;
    }
}

void collectSuspendingSequenceForeach(
    const syntax::StatementSyntax& statement,
    std::vector<const syntax::ForeachStatementSyntax*>& output) {
    const auto containsYield = [](const syntax::StatementSyntax& root) {
        bool found = false;
        std::function<void(const syntax::StatementSyntax&)> visit;
        visit = [&](const syntax::StatementSyntax& current) {
            if (found) return;
            switch (current.kind()) {
            case syntax::SyntaxKind::YieldWaitStatement:
                found = true;
                return;
            case syntax::SyntaxKind::YieldBreakStatement:
                return;
            case syntax::SyntaxKind::BlockStatement:
                for (const auto& child : static_cast<const
                     syntax::BlockStatementSyntax&>(current).statements) {
                    visit(*child);
                }
                return;
            case syntax::SyntaxKind::IfStatement: {
                const auto& branch = static_cast<const
                    syntax::IfStatementSyntax&>(current);
                visit(*branch.thenStatement);
                if (branch.elseStatement) visit(*branch.elseStatement);
                return;
            }
            case syntax::SyntaxKind::WhileStatement:
                visit(*static_cast<const syntax::WhileStatementSyntax&>(
                    current).body);
                return;
            case syntax::SyntaxKind::ForStatement:
                visit(*static_cast<const syntax::ForStatementSyntax&>(
                    current).body);
                return;
            case syntax::SyntaxKind::ForeachStatement:
                visit(*static_cast<const syntax::ForeachStatementSyntax&>(
                    current).body);
                return;
            case syntax::SyntaxKind::DoWhileStatement:
                visit(*static_cast<const syntax::DoWhileStatementSyntax&>(
                    current).body);
                return;
            case syntax::SyntaxKind::SwitchStatement:
                for (const auto& section : static_cast<const
                     syntax::SwitchStatementSyntax&>(current).sections) {
                    for (const auto& child : section.statements) visit(*child);
                }
                return;
            case syntax::SyntaxKind::TryStatement: {
                const auto& tryStmt = static_cast<const
                    syntax::TryStatementSyntax&>(current);
                visit(tryStmt.body);
                for (const auto& c : tryStmt.catches) visit(c.body);
                if (tryStmt.finallyBody) visit(*tryStmt.finallyBody);
                return;
            }
            default:
                return;
            }
        };
        visit(root);
        return found;
    };

    switch (statement.kind()) {
    case syntax::SyntaxKind::ForeachStatement: {
        const auto& loop = static_cast<const
            syntax::ForeachStatementSyntax&>(statement);
        if (containsYield(*loop.body)) output.push_back(&loop);
        collectSuspendingSequenceForeach(*loop.body, output);
        return;
    }
    case syntax::SyntaxKind::BlockStatement:
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(statement).statements) {
            collectSuspendingSequenceForeach(*child, output);
        }
        return;
    case syntax::SyntaxKind::IfStatement: {
        const auto& branch = static_cast<const
            syntax::IfStatementSyntax&>(statement);
        collectSuspendingSequenceForeach(*branch.thenStatement, output);
        if (branch.elseStatement) {
            collectSuspendingSequenceForeach(*branch.elseStatement, output);
        }
        return;
    }
    case syntax::SyntaxKind::WhileStatement:
        collectSuspendingSequenceForeach(*static_cast<const
            syntax::WhileStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::ForStatement:
        collectSuspendingSequenceForeach(*static_cast<const
            syntax::ForStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::DoWhileStatement:
        collectSuspendingSequenceForeach(*static_cast<const
            syntax::DoWhileStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::SwitchStatement:
        for (const auto& section : static_cast<const
             syntax::SwitchStatementSyntax&>(statement).sections) {
            for (const auto& child : section.statements) {
                collectSuspendingSequenceForeach(*child, output);
            }
        }
        return;
    case syntax::SyntaxKind::TryStatement: {
        const auto& tryStmt = static_cast<const
            syntax::TryStatementSyntax&>(statement);
        collectSuspendingSequenceForeach(tryStmt.body, output);
        for (const auto& c : tryStmt.catches) {
            collectSuspendingSequenceForeach(c.body, output);
        }
        if (tryStmt.finallyBody) {
            collectSuspendingSequenceForeach(*tryStmt.finallyBody, output);
        }
        return;
    }
    default:
        return;
    }
}

std::optional<semantic::FieldSymbol> makeSequenceLocalField(
    const semantic::TypeSymbol& owner,
    const syntax::SequenceDeclarationSyntax& sequence,
    const syntax::VariableDeclarationStatementSyntax& declaration,
    const ModuleWork& module,
    const std::map<std::string, ModuleWork>& allModules,
    const std::vector<std::pair<std::string, semantic::FieldSymbol>>& sequenceParameterFields,
    const std::vector<std::pair<std::string, semantic::FieldSymbol>>& existingSequenceLocals,
    const std::string& sourceName,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto& visibleTypes = module.visibleTypes;
    semantic::PrimitiveType type = semantic::resolvePrimitiveType(
        declaration.type.name.text);
    std::string typeName;
    if (declaration.type.name.text == "var" && declaration.initializer) {
        std::function<bool(const syntax::ExpressionSyntax&, semantic::PrimitiveType&, std::string&)> inferExpr;
        inferExpr = [&](const syntax::ExpressionSyntax& expr,
                        semantic::PrimitiveType& outType,
                        std::string& outName) -> bool {
            switch (expr.kind()) {
            case syntax::SyntaxKind::LiteralExpression: {
                const auto& lit = static_cast<const syntax::LiteralExpressionSyntax&>(expr);
                switch (lit.literalToken.kind) {
                case syntax::SyntaxKind::TrueKeyword:
                case syntax::SyntaxKind::FalseKeyword:
                    outType = semantic::PrimitiveType::Bool;
                    return true;
                case syntax::SyntaxKind::StringLiteralToken:
                    outType = semantic::PrimitiveType::String;
                    return true;
                case syntax::SyntaxKind::IntegerLiteralToken: {
                    const auto& txt = lit.literalToken.text;
                    outType = (!txt.empty() && (txt.back() == 'L' || txt.back() == 'l'))
                        ? semantic::PrimitiveType::Long
                        : semantic::PrimitiveType::Int;
                    return true;
                }
                case syntax::SyntaxKind::FloatLiteralToken: {
                    const auto& txt = lit.literalToken.text;
                    outType = (!txt.empty() && (txt.back() == 'f' || txt.back() == 'F'))
                        ? semantic::PrimitiveType::Float
                        : semantic::PrimitiveType::Double;
                    return true;
                }
                default:
                    return false;
                }
            }
            case syntax::SyntaxKind::ThisExpression:
                outType = owner.kind == semantic::TypeKind::Struct
                    ? semantic::PrimitiveType::Struct
                    : semantic::PrimitiveType::Object;
                outName = semantic::canonicalTypeName(owner);
                return true;
            case syntax::SyntaxKind::NameExpression: {
                const auto& ident = static_cast<const syntax::NameExpressionSyntax&>(expr).identifierToken.text;
                for (const auto& [lname, lfield] : existingSequenceLocals) {
                    if (lname == ident) {
                        outType = lfield.type;
                        outName = lfield.typeName;
                        return true;
                    }
                }
                for (const auto& [pname, pfield] : sequenceParameterFields) {
                    if (pname == ident) {
                        outType = pfield.type;
                        outName = pfield.typeName;
                        return true;
                    }
                }
                for (const auto& f : owner.fields) {
                    if (!f.synthetic && f.name == ident) {
                        outType = f.type;
                        outName = f.typeName;
                        return true;
                    }
                }
                return false;
            }
            case syntax::SyntaxKind::ParenthesizedExpression:
                return inferExpr(
                    *static_cast<const syntax::ParenthesizedExpressionSyntax&>(expr).expression,
                    outType, outName);
            case syntax::SyntaxKind::UnaryExpression:
                return inferExpr(
                    *static_cast<const syntax::UnaryExpressionSyntax&>(expr).operand,
                    outType, outName);
            case syntax::SyntaxKind::BinaryExpression: {
                const auto& bin = static_cast<const syntax::BinaryExpressionSyntax&>(expr);
                return inferExpr(*bin.left, outType, outName);
            }
            case syntax::SyntaxKind::NewObjectExpression: {
                const auto& nw = static_cast<const syntax::NewObjectExpressionSyntax&>(expr);
                const auto found = visibleTypes.find(nw.type.name.text);
                if (found != visibleTypes.end()) {
                    outType = found->second.kind == semantic::TypeKind::Struct
                        ? semantic::PrimitiveType::Struct
                        : (found->second.kind == semantic::TypeKind::Enum
                            ? semantic::PrimitiveType::Enum
                            : semantic::PrimitiveType::Object);
                    outName = semantic::canonicalTypeName(found->second);
                    return true;
                }
                return false;
            }
            case syntax::SyntaxKind::NewArrayExpression: {
                const auto& na = static_cast<const syntax::NewArrayExpressionSyntax&>(expr);
                auto elemPrim = semantic::resolvePrimitiveType(na.elementType.name.text);
                std::string elemName;
                if (elemPrim == semantic::PrimitiveType::Error) {
                    const auto found = visibleTypes.find(na.elementType.name.text);
                    if (found != visibleTypes.end()) {
                        elemPrim = found->second.kind == semantic::TypeKind::Struct
                            ? semantic::PrimitiveType::Struct
                            : (found->second.kind == semantic::TypeKind::Enum
                                ? semantic::PrimitiveType::Enum
                                : semantic::PrimitiveType::Object);
                        elemName = semantic::canonicalTypeName(found->second);
                    }
                }
                if (elemPrim != semantic::PrimitiveType::Error) {
                    outType = semantic::PrimitiveType::Array;
                    outName = semantic::arrayTypeName(elemPrim, elemName);
                    return true;
                }
                return false;
            }
            case syntax::SyntaxKind::CallExpression: {
                const auto& call = static_cast<const syntax::CallExpressionSyntax&>(expr);
                const auto& fnName = call.identifierToken.text;
                if (fnName == "FixedFromInt" || fnName == "FixedFromRatio") {
                    const auto found = visibleTypes.find("Fixed");
                    if (found != visibleTypes.end()) {
                        outType = semantic::PrimitiveType::Struct;
                        outName = semantic::canonicalTypeName(found->second);
                        return true;
                    }
                }
                for (const auto& m : owner.methods) {
                    if (m.name == fnName) {
                        outType = m.returnType;
                        outName = m.returnTypeName;
                        return outType != semantic::PrimitiveType::Error;
                    }
                }
                for (const auto& fn : module.declarations) {
                    if (!fn.method && fn.name == fnName) {
                        outType = fn.returnType;
                        outName = fn.returnTypeName;
                        return outType != semantic::PrimitiveType::Error;
                    }
                }
                for (const auto& impName : module.imports) {
                    const auto impIt = allModules.find(impName);
                    if (impIt == allModules.end()) continue;
                    for (const auto& fn : impIt->second.declarations) {
                        if (!fn.method && fn.name == fnName) {
                            outType = fn.returnType;
                            outName = fn.returnTypeName;
                            return outType != semantic::PrimitiveType::Error;
                        }
                    }
                }
                return false;
            }
            case syntax::SyntaxKind::MemberCallExpression: {
                const auto& mcall = static_cast<const syntax::MemberCallExpressionSyntax&>(expr);
                const auto& mname = mcall.nameToken.text;
                auto resolveMethodOnType = [&](const semantic::TypeSymbol& targetType, bool wantStatic) -> bool {
                    for (const auto& m : targetType.methods) {
                        if (m.staticMethod == wantStatic && m.name == mname) {
                            outType = m.returnType;
                            outName = m.returnTypeName;
                            return outType != semantic::PrimitiveType::Error;
                        }
                    }
                    const auto modIt = allModules.find(targetType.moduleName);
                    if (modIt == allModules.end()) return false;
                    for (const auto& liveT : modIt->second.types) {
                        if (liveT.id != targetType.id && liveT.name != targetType.name) continue;
                        for (const auto& m : liveT.methods) {
                            if (m.staticMethod == wantStatic && m.name == mname) {
                                outType = m.returnType;
                                outName = m.returnTypeName;
                                return outType != semantic::PrimitiveType::Error;
                            }
                        }
                    }
                    auto checkSyntaxMethods = [&](const auto& decls) -> bool {
                        for (const auto& decl : decls) {
                            if (decl.identifierToken.text != targetType.name) continue;
                            for (const auto& ms : decl.methods) {
                                if (!ms.typeParameters.empty() || ms.identifierToken.text != mname) continue;
                                const bool isStat = semantic::hasModifier(ms.modifiers, syntax::SyntaxKind::StaticKeyword);
                                if (isStat != wantStatic) continue;
                                diagnostics::DiagnosticBag ignored;
                                const auto retRef = resolveInterfaceType(
                                    ms.returnType, modIt->second.visibleTypes, ignored, sourceName, true);
                                if (retRef.type != semantic::PrimitiveType::Error) {
                                    outType = retRef.type;
                                    outName = retRef.typeName;
                                    return true;
                                }
                            }
                        }
                        return false;
                    };
                    for (const auto* u : modIt->second.units) {
                        if (!u || !u->syntaxTree) continue;
                        if (checkSyntaxMethods(u->syntaxTree->classes) ||
                            checkSyntaxMethods(u->syntaxTree->structs)) {
                            return true;
                        }
                    }
                    return false;
                };
                if (mcall.receiver &&
                    mcall.receiver->kind() == syntax::SyntaxKind::NameExpression) {
                    const auto& recvIdent = static_cast<const syntax::NameExpressionSyntax&>(
                        *mcall.receiver).identifierToken.text;
                    const auto typeIt = visibleTypes.find(recvIdent);
                    if (typeIt != visibleTypes.end() && resolveMethodOnType(typeIt->second, true)) {
                        return true;
                    }
                }
                if (mcall.receiver) {
                    semantic::PrimitiveType recvPrim = semantic::PrimitiveType::Error;
                    std::string recvName;
                    if (inferExpr(*mcall.receiver, recvPrim, recvName) && !recvName.empty()) {
                        if ((recvName == semantic::canonicalTypeName(owner) || recvName == owner.name) &&
                            resolveMethodOnType(owner, false)) {
                            return true;
                        }
                        const auto typeIt = visibleTypes.find(recvName);
                        if (typeIt != visibleTypes.end() && resolveMethodOnType(typeIt->second, false)) {
                            return true;
                        }
                    }
                }
                return false;
            }
            case syntax::SyntaxKind::MemberAccessExpression: {
                const auto& macc = static_cast<const syntax::MemberAccessExpressionSyntax&>(expr);
                const auto& mname = macc.nameToken.text;
                auto lookupFieldOrProp = [&](const semantic::TypeSymbol& ts, bool staticOnly) -> bool {
                    for (const auto& f : ts.fields) {
                        if (f.staticField == staticOnly && f.name == mname) {
                            outType = f.type;
                            outName = f.typeName;
                            return true;
                        }
                    }
                    for (const auto& pr : ts.properties) {
                        if (pr.staticProperty == staticOnly && pr.name == mname) {
                            outType = pr.type;
                            outName = pr.typeName;
                            return true;
                        }
                    }
                    for (const auto& m : ts.methods) {
                        if (m.staticMethod == staticOnly && m.name == mname) {
                            outType = m.returnType;
                            outName = m.returnTypeName;
                            return outType != semantic::PrimitiveType::Error;
                        }
                    }
                    return false;
                };
                if (macc.receiver &&
                    macc.receiver->kind() == syntax::SyntaxKind::NameExpression) {
                    const auto& recvIdent = static_cast<const syntax::NameExpressionSyntax&>(
                        *macc.receiver).identifierToken.text;
                    const auto typeIt = visibleTypes.find(recvIdent);
                    if (typeIt != visibleTypes.end() && lookupFieldOrProp(typeIt->second, true)) {
                        return true;
                    }
                }
                if (macc.receiver) {
                    semantic::PrimitiveType recvPrim = semantic::PrimitiveType::Error;
                    std::string recvName;
                    if (inferExpr(*macc.receiver, recvPrim, recvName) && !recvName.empty()) {
                        if ((recvName == semantic::canonicalTypeName(owner) || recvName == owner.name) &&
                            lookupFieldOrProp(owner, false)) {
                            return true;
                        }
                        const auto typeIt = visibleTypes.find(recvName);
                        if (typeIt != visibleTypes.end() && lookupFieldOrProp(typeIt->second, false)) {
                            return true;
                        }
                    }
                }
                return false;
            }
            default:
                return false;
            }
        };
        (void)inferExpr(*declaration.initializer, type, typeName);
    }
    if (type == semantic::PrimitiveType::Error) {
        const auto found = visibleTypes.find(declaration.type.name.text);
        if (found == visibleTypes.end()) {
            diagnostics.report(
                "RS8800",
                "unknown sequence local type '" + declaration.type.name.text + "'",
                declaration.type.span(),
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            return std::nullopt;
        }
        type = found->second.kind == semantic::TypeKind::Struct
            ? semantic::PrimitiveType::Struct
            : found->second.kind == semantic::TypeKind::Enum
                ? semantic::PrimitiveType::Enum
                : semantic::PrimitiveType::Object;
        typeName = semantic::canonicalTypeName(found->second);
    }
    if (declaration.type.isArray()) {
        if (semantic::isStackOnlyType(type, typeName, visibleTypes)) {
            diagnostics.report(
                "RS2695",
                "stack-only struct '" +
                    semantic::stackOnlyTypeDisplayName(type, typeName, visibleTypes) +
                    "' cannot be an array element type",
                declaration.type.span(),
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            return std::nullopt;
        }
        typeName = semantic::arrayTypeName(type, typeName);
        type = semantic::PrimitiveType::Array;
    }
    if (semantic::isStackOnlyType(type, typeName, visibleTypes)) {
        diagnostics.report(
            "RS2696",
            "sequence '" + sequence.identifierToken.text +
                "' cannot declare stack-only local variable '" +
                declaration.identifierToken.text + "'",
            declaration.identifierToken.span,
            diagnostics::DiagnosticSeverity::Error,
            sourceName);
        return std::nullopt;
    }
    if (semantic::isStackOnlyArrayType(type, typeName, visibleTypes)) {
        diagnostics.report(
            "RS2695",
            "stack-only struct '" +
                semantic::stackOnlyTypeDisplayName(type, typeName, visibleTypes) +
                "' cannot be an array element type",
            declaration.type.span(),
            diagnostics::DiagnosticSeverity::Error,
            sourceName);
        return std::nullopt;
    }
    semantic::FieldSymbol field;
    field.name = "$sequence_local_" + sequence.identifierToken.text + "_" +
        declaration.identifierToken.text;
    field.declaringTypeId = owner.id;
    field.declaringTypeName = semantic::canonicalTypeName(owner);
    field.type = type;
    field.typeName = std::move(typeName);
    field.synthetic = true;
    field.isUsingResource = declaration.isUsingDeclaration;
    field.sourceName = sourceName;
    field.declarationSpan = declaration.identifierToken.span;
    field.id = semantic::stableTypeId(
        semantic::canonicalTypeName(owner) + "::field:" + field.name);
    return field;
}

void collectEventSubscriptions(
    const syntax::StatementSyntax& statement,
    std::vector<const syntax::EventSubscriptionStatementSyntax*>& output) {
    switch (statement.kind()) {
    case syntax::SyntaxKind::EventSubscriptionStatement:
        output.push_back(static_cast<const
            syntax::EventSubscriptionStatementSyntax*>(&statement));
        return;
    case syntax::SyntaxKind::BlockStatement:
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(statement).statements) {
            collectEventSubscriptions(*child, output);
        }
        return;
    case syntax::SyntaxKind::IfStatement: {
        const auto& value = static_cast<const
            syntax::IfStatementSyntax&>(statement);
        collectEventSubscriptions(*value.thenStatement, output);
        if (value.elseStatement) {
            collectEventSubscriptions(*value.elseStatement, output);
        }
        return;
    }
    case syntax::SyntaxKind::WhileStatement:
        collectEventSubscriptions(*static_cast<const
            syntax::WhileStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::ForStatement:
        collectEventSubscriptions(*static_cast<const
            syntax::ForStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::ForeachStatement:
        collectEventSubscriptions(*static_cast<const
            syntax::ForeachStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::DoWhileStatement:
        collectEventSubscriptions(*static_cast<const
            syntax::DoWhileStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::SwitchStatement:
        for (const auto& section : static_cast<const
             syntax::SwitchStatementSyntax&>(statement).sections) {
            for (const auto& child : section.statements) {
                collectEventSubscriptions(*child, output);
            }
        }
        return;
    case syntax::SyntaxKind::TryStatement: {
        const auto& value = static_cast<const
            syntax::TryStatementSyntax&>(statement);
        collectEventSubscriptions(value.body, output);
        for (const auto& c : value.catches) {
            collectEventSubscriptions(c.body, output);
        }
        if (value.finallyBody) {
            collectEventSubscriptions(*value.finallyBody, output);
        }
        return;
    }
    default:
        return;
    }
}

bool eventMethodMatches(
    const semantic::FunctionSymbol& function,
    const DelegateContract& contract) {
    if (function.staticMethod ||
        function.returnType != contract.returnType.type ||
        (semantic::isExactType(function.returnType) &&
         function.returnTypeName != contract.returnType.typeName)) {
        return false;
    }
    const auto offset = function.method && !function.staticMethod
        ? std::size_t{1}
        : std::size_t{0};
    if (function.parameters.size() != contract.parameters.size() + offset) {
        return false;
    }
    for (std::size_t index = 0; index < contract.parameters.size(); ++index) {
        const auto& parameter = function.parameters[index + offset];
        if (parameter.modifier != contract.parameterModifiers[index] ||
            parameter.type != contract.parameters[index].type ||
            (semantic::isExactType(parameter.type) &&
             parameter.typeName != contract.parameters[index].typeName)) {
            return false;
        }
    }
    return true;
}

semantic::VariableSymbol makeEventThisParameter(
    const semantic::TypeSymbol& owner,
    text::TextSpan span) {
    semantic::VariableSymbol parameter;
    parameter.name = "this";
    parameter.type = semantic::PrimitiveType::Object;
    parameter.typeName = semantic::canonicalTypeName(owner);
    parameter.storageType = parameter.type;
    parameter.storageTypeName = parameter.typeName;
    parameter.index = 0;
    parameter.parameter = true;
    parameter.declarationSpan = span;
    return parameter;
}

semantic::FunctionSymbol makeEventLambdaFunction(
    const std::string& moduleName,
    const semantic::TypeSymbol& owner,
    const DelegateContract& contract,
    const syntax::LambdaExpressionSyntax& lambda,
    const std::string& eventName,
    std::size_t ordinal,
    const std::string& sourceName,
    diagnostics::DiagnosticBag& diagnostics) {
    semantic::FunctionSymbol result;
    result.moduleName = moduleName;
    result.name = "$event_" + eventName + "_lambda_" +
        std::to_string(ordinal);
    result.ownerTypeName = owner.name;
    result.ownerTypeId = owner.id;
    result.returnType = contract.returnType.type;
    result.returnTypeName = contract.returnType.typeName;
    result.method = true;
    result.synthetic = true;
    result.sourceName = sourceName;
    result.declarationSpan = lambda.arrowToken.span;
    result.bodySpan = lambda.span();
    result.parameters.push_back(makeEventThisParameter(
        owner, lambda.span()));
    if (lambda.parameterTokens.size() != contract.parameters.size()) {
        diagnostics.report(
            "RS8312",
            "event lambda parameter count does not match delegate",
            lambda.span(),
            diagnostics::DiagnosticSeverity::Error,
            sourceName);
    }
    for (std::size_t index = 0; index < contract.parameters.size(); ++index) {
        semantic::VariableSymbol parameter;
        parameter.name = index < lambda.parameterTokens.size()
            ? lambda.parameterTokens[index].text
            : "$arg" + std::to_string(index);
        parameter.type = contract.parameters[index].type;
        parameter.typeName = contract.parameters[index].typeName;
        parameter.storageType = parameter.type;
        parameter.storageTypeName = parameter.typeName;
        parameter.index = result.parameters.size();
        parameter.parameter = true;
        parameter.declarationSpan = index < lambda.parameterTokens.size()
            ? lambda.parameterTokens[index].span
            : lambda.arrowToken.span;
        result.parameters.push_back(std::move(parameter));
    }
    result.id = semantic::stableFunctionId(result);
    for (auto& parameter : result.parameters) {
        parameter.id = semantic::stableTypeId(
            std::to_string(result.id) + "::local:" +
            std::to_string(parameter.index) + ":" + parameter.name);
    }
    return result;
}

} // namespace

void synthesizeDeclarationsSequencesAndMembers(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    // Declare free functions before member signatures. Member declarations
    // are then processed in one global inheritance order so file and module
    // names cannot affect inherited metadata or dispatch-table construction.
    std::map<std::string, std::unordered_set<std::string>> functionKeys;
    const auto addBinding = [&](
        ModuleWork& module,
        semantic::FunctionBindingInput binding,
        text::TextSpan span) {
        if (!binding.symbol.ownerTypeName.empty()) {
            if (const auto* ownT = findOwnType(module, binding.symbol.ownerTypeName)) {
                binding.hygienicAccessibleModules.insert(
                    ownT->hygienicAccessibleModules.begin(),
                    ownT->hygienicAccessibleModules.end());
            }
        }
        if (module.deterministic && !binding.symbol.nonDeterministic) {
            binding.symbol.deterministic = true;
        }
        const auto key = semantic::canonicalFunctionKey(binding.symbol);
        auto& keys = functionKeys[module.name];
        if (!keys.insert(key).second) {
            result.diagnostics.report(
                "RS4002", "duplicate function overload '" + key + "'", span);
            module.invalid = true;
        }
        module.declarations.push_back(binding.symbol);
        if (!binding.symbol.abstractMethod) {
            module.functionBindings.push_back(std::move(binding));
        }
    };

    for (auto& [moduleName, module] : modules) {
        if (module.frontendCached) continue;
        for (const auto* unit : module.units) {
            for (const auto& functionSyntax : unit->syntaxTree->functions) {
                if (!functionSyntax.typeParameters.empty()) continue;
                semantic::FunctionBindingInput binding;
                diagnostics::DiagnosticBag fnDeclBag;
                binding.symbol = semantic::declareFunctionSymbol(
                    moduleName, functionSyntax, module.visibleTypes,
                    fnDeclBag);
                appendStampedDiagnostics(result.diagnostics, fnDeclBag, unit->source->name());
                if (fnDeclBag.hasErrors()) module.invalid = true;
                binding.sourceName = unit->source->name();
                binding.symbol.sourceName = binding.sourceName;
                if (functionSyntax.semicolonToken.has_value()) {
                    if (module.deterministic && !binding.symbol.nonDeterministic) {
                        binding.symbol.deterministic = true;
                    }
                    const auto key = semantic::canonicalFunctionKey(binding.symbol);
                    auto& keys = functionKeys[module.name];
                    if (!keys.insert(key).second) {
                        result.diagnostics.report(
                            "RS4002",
                            "duplicate function overload '" + key + "'",
                            functionSyntax.identifierToken.span,
                            diagnostics::DiagnosticSeverity::Error,
                            unit->source->name());
                        module.invalid = true;
                    }
                    module.declarations.push_back(binding.symbol);
                    continue;
                }
                binding.body = &functionSyntax.body;
                binding.hygienicAccessibleModules.insert(
                    unit->unitHygienicModules.begin(),
                    unit->unitHygienicModules.end());
                binding.hygienicAccessibleModules.insert(
                    functionSyntax.hygienicModules.begin(),
                    functionSyntax.hygienicModules.end());
                appendParameters(binding, functionSyntax.parameters);
                addBinding(
                    module, std::move(binding),
                    functionSyntax.identifierToken.span);
            }
        }
    }

    const auto processMembers = [&](
        const auto& typeSyntax,
        const ParsedUnit* unit,
        const std::string& moduleName,
        ModuleWork& module) {
                    if (!typeSyntax.typeParameters.empty()) return;
                    auto* ownerPointer = findOwnType(module, typeSyntax.identifierToken.text);
                    if (!ownerPointer) {
                        module.invalid = true;
                        return;
                    }
                    auto owner = *ownerPointer;
                    const semantic::TypeSymbol* base = nullptr;
                    if (!owner.baseTypeName.empty()) {
                        base = findGlobalType(
                            modules, owner.baseTypeName);
                        if (base) {
                            module.visibleTypes[base->name] = *base;
                            module.visibleTypes[semantic::canonicalTypeName(*base)] = *base;
                            std::vector<semantic::FieldSymbol> ownFields;
                            for (const auto& f : owner.fields) {
                                if (f.declaringTypeId == owner.id) {
                                    ownFields.push_back(f);
                                }
                            }
                            owner.fields = base->fields;
                            for (auto& f : ownFields) {
                                f.index = owner.fields.size();
                                owner.fields.push_back(f);
                            }
                            owner.methods = base->methods;
                            owner.virtualDispatchTable =
                                base->virtualDispatchTable;
                            owner.interfaceDispatchMaps =
                                base->interfaceDispatchMaps;
                            owner.properties = base->properties;
                            owner.events = base->events;
                        }
                    }
                    std::unordered_set<std::string> fieldNames;
                    std::unordered_set<std::string> methodNames;
                    std::unordered_set<std::string> propertyNames;
                    for (const auto& field : owner.fields) {
                        fieldNames.insert(field.name);
                    }
                    std::vector<std::pair<std::string, semantic::SymbolId>>
                        newVirtualMethods;
                    for (const auto& methodSyntax : typeSyntax.methods) {
                        if (!methodSyntax.typeParameters.empty()) continue;
                        diagnostics::DiagnosticBag ignoredDiagnostics;
                        auto method = semantic::declareFunctionSymbol(
                            moduleName, methodSyntax, module.visibleTypes,
                            ignoredDiagnostics, &owner);
                        if (!method.staticMethod && !method.overrideMethod &&
                            (method.virtualMethod || method.abstractMethod)) {
                            newVirtualMethods.emplace_back(
                                semantic::canonicalFunctionSignature(method),
                                method.id);
                        }
                    }
                    std::sort(
                        newVirtualMethods.begin(), newVirtualMethods.end(),
                        [](const auto& left, const auto& right) {
                            return left.first < right.first ||
                                (left.first == right.first &&
                                 left.second < right.second);
                        });
                    std::unordered_map<
                        semantic::SymbolId, std::uint32_t> plannedVirtualSlots;
                    auto nextVirtualSlot = static_cast<std::uint32_t>(
                        owner.virtualDispatchTable.size());
                    for (const auto& [signature, methodId] :
                         newVirtualMethods) {
                        (void)signature;
                        plannedVirtualSlots.emplace(
                            methodId, nextVirtualSlot++);
                    }
                    for (const auto& methodSyntax : typeSyntax.methods) {
                        if (!methodSyntax.typeParameters.empty()) continue;
                        const auto& methodSrc = methodSyntax.originSourceName.empty()
                            ? unit->source->name()
                            : methodSyntax.originSourceName;
                        if (fieldNames.find(methodSyntax.identifierToken.text) !=
                            fieldNames.end()) {
                            result.diagnostics.report(
                                "RS2464",
                                "method '" + methodSyntax.identifierToken.text +
                                    "' conflicts with a field of the same name",
                                methodSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                methodSrc);
                            module.invalid = true;
                        }
                        methodNames.insert(methodSyntax.identifierToken.text);
                        semantic::FunctionBindingInput binding;
                        diagnostics::DiagnosticBag methodDeclBag;
                        binding.symbol = semantic::declareFunctionSymbol(
                            moduleName, methodSyntax, module.visibleTypes,
                            methodDeclBag, &owner);
                        appendStampedDiagnostics(result.diagnostics, methodDeclBag, methodSrc);
                        if (methodDeclBag.hasErrors()) module.invalid = true;
                        if (owner.staticType && !binding.symbol.staticMethod) {
                            result.diagnostics.report(
                                "RS2489",
                                "cannot declare instance method '" +
                                    methodSyntax.identifierToken.text +
                                    "' in static class '" + owner.name + "'",
                                methodSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                methodSrc);
                            module.invalid = true;
                        }
                        const auto plannedVirtualSlot =
                            plannedVirtualSlots.find(binding.symbol.id);
                        if (plannedVirtualSlot !=
                            plannedVirtualSlots.end()) {
                            binding.symbol.virtualSlot =
                                plannedVirtualSlot->second;
                        }
                        binding.sourceName = methodSrc;
                        binding.symbol.sourceName = methodSrc;
                        binding.body = methodSyntax.semicolonToken
                            ? nullptr
                            : &methodSyntax.body;
                        if (!binding.symbol.staticMethod) {
                            binding.parameterNames.push_back("this");
                            binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                        }
                        binding.hygienicAccessibleModules.insert(
                            unit->unitHygienicModules.begin(),
                            unit->unitHygienicModules.end());
                        binding.hygienicAccessibleModules.insert(
                            methodSyntax.hygienicModules.begin(),
                            methodSyntax.hygienicModules.end());
                        appendParameters(binding, methodSyntax.parameters);
                        const auto inheritedIndex =
                            prepareVirtualMethod(
                                owner, binding.symbol, methodSyntax,
                                module, result, methodSrc);
                        if (inheritedIndex) {
                            owner.methods[*inheritedIndex] = binding.symbol;
                        } else {
                            owner.methods.push_back(binding.symbol);
                        }
                        addBinding(module, std::move(binding), methodSyntax.identifierToken.span);
                    }
                    std::unordered_set<std::string> ownerSequenceNames;
                    for (const auto& sequence : typeSyntax.sequences) {
                        ownerSequenceNames.insert(
                            sequence.identifierToken.text);
                    }
                    for (const auto& sequenceSyntax : typeSyntax.sequences) {
                        const auto& seqSrc = sequenceSyntax.originSourceName.empty()
                            ? unit->source->name()
                            : sequenceSyntax.originSourceName;
                        if (owner.staticType) {
                            result.diagnostics.report(
                                "RS2489",
                                "cannot declare sequence method '" +
                                    sequenceSyntax.identifierToken.text +
                                    "' in static class '" + owner.name + "'",
                                sequenceSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                seqSrc);
                            module.invalid = true;
                            return;
                        }
                        if (owner.kind == semantic::TypeKind::Struct) {
                            result.diagnostics.report(
                                "RS2491",
                                "sequence methods require a class owner",
                                sequenceSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                seqSrc);
                            module.invalid = true;
                            return;
                        }
                        if (fieldNames.find(sequenceSyntax.identifierToken.text) !=
                                fieldNames.end() ||
                            methodNames.find(sequenceSyntax.identifierToken.text) !=
                                methodNames.end()) {
                            result.diagnostics.report(
                                "RS2464",
                                "sequence '" + sequenceSyntax.identifierToken.text +
                                    "' conflicts with another member",
                                sequenceSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                seqSrc);
                            module.invalid = true;
                            continue;
                        }
                        methodNames.insert(sequenceSyntax.identifierToken.text);
                        const auto yields = sequenceYields(
                            sequenceSyntax,
                            result.diagnostics,
                            seqSrc);
                        const auto singleYieldLoop =
                            isSingleYieldLoopSequence(sequenceSyntax);
                        const auto singleYieldBranch =
                            isSingleYieldBranchSequence(sequenceSyntax);
                        std::vector<const syntax::StatementSyntax*>
                            sequenceCompositionCalls;
                        collectSequenceCompositionCalls(
                            sequenceSyntax.body, ownerSequenceNames,
                            sequenceCompositionCalls);
                        if (std::any_of(
                                sequenceCompositionCalls.begin(),
                                sequenceCompositionCalls.end(),
                                [&](const syntax::StatementSyntax* statement) {
                                    std::string target;
                                    return statement &&
                                        isSequenceCompositionCall(
                                            *statement,
                                            ownerSequenceNames,
                                            &target) &&
                                        target ==
                                            sequenceSyntax.identifierToken.text;
                                })) {
                            result.diagnostics.report(
                                "RS8810",
                                "a sequence cannot compose itself on the same owner",
                                sequenceSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                seqSrc);
                            module.invalid = true;
                        }
                        // Phase 22 uses one explicit program-counter callback
                        // for every sequence. This keeps completion, restart,
                        // cancellation, nesting, snapshots, and result state on
                        // one durable model instead of retaining the bounded
                        // Phase 18 callback-splitting variants.
                        const bool stateMachine = true;
                        if (sequenceSyntax.parameters.empty() ||
                            sequenceSyntax.parameters.front().type.name.text !=
                                "long" ||
                            sequenceSyntax.parameters.front().type.isArray()) {
                            module.invalid = true;
                            continue;
                        }

                        std::vector<InterfaceTypeRef> sequenceParameterTypes;
                        sequenceParameterTypes.reserve(
                            sequenceSyntax.parameters.size());
                        for (std::size_t pIdx = 0; pIdx < sequenceSyntax.parameters.size(); ++pIdx) {
                            const auto& parameter = sequenceSyntax.parameters[pIdx];
                            sequenceParameterTypes.push_back(resolveInterfaceType(
                                parameter.type, module.visibleTypes,
                                result.diagnostics, seqSrc, false));
                            if (sequenceParameterTypes.back().type ==
                                semantic::PrimitiveType::Error) {
                                module.invalid = true;
                            } else if (pIdx > 0 && semantic::isStackOnlyType(
                                    sequenceParameterTypes.back().type,
                                    sequenceParameterTypes.back().typeName,
                                    module.visibleTypes)) {
                                result.diagnostics.report(
                                    "RS2696",
                                    "sequence '" + sequenceSyntax.identifierToken.text +
                                        "' cannot declare stack-only parameter '" +
                                        parameter.identifierToken.text + "'",
                                    parameter.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                            } else if (pIdx > 0 && semantic::isStackOnlyArrayType(
                                    sequenceParameterTypes.back().type,
                                    sequenceParameterTypes.back().typeName,
                                    module.visibleTypes)) {
                                result.diagnostics.report(
                                    "RS2695",
                                    "stack-only struct '" +
                                        semantic::stackOnlyTypeDisplayName(
                                            sequenceParameterTypes.back().type,
                                            sequenceParameterTypes.back().typeName,
                                            module.visibleTypes) +
                                        "' cannot be an array element type",
                                    parameter.type.span(),
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                            }
                        }
                        InterfaceTypeRef sequenceResultType{
                            semantic::PrimitiveType::Void, {}};
                        if (sequenceSyntax.resultType) {
                            sequenceResultType = resolveInterfaceType(
                                *sequenceSyntax.resultType,
                                module.visibleTypes,
                                result.diagnostics,
                                seqSrc,
                                false);
                            if (sequenceResultType.type ==
                                semantic::PrimitiveType::Error) {
                                module.invalid = true;
                            }
                            if (!sequenceStatementAlwaysReturnsResult(
                                    sequenceSyntax.body)) {
                                result.diagnostics.report(
                                    "RS8815",
                                    "not all result-sequence code paths return a value",
                                    sequenceSyntax.body.span(),
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                            }
                        }

                        semantic::FieldSymbol targetField;
                        targetField.name = "$sequence_target_" +
                            sequenceSyntax.identifierToken.text;
                        targetField.type = semantic::PrimitiveType::Long;
                        targetField.index = owner.fields.size();
                        targetField.synthetic = true;
                        targetField.sourceName = seqSrc;
                        targetField.declarationSpan =
                            sequenceSyntax.identifierToken.span;
                        targetField.id = semantic::stableTypeId(
                            semantic::canonicalTypeName(owner) +
                            "::field:" + targetField.name);
                        owner.fields.push_back(targetField);
                        fieldNames.insert(targetField.name);

                        semantic::FieldSymbol timerField;
                        timerField.name = "$sequence_timer_" +
                            sequenceSyntax.identifierToken.text;
                        timerField.type = semantic::PrimitiveType::Long;
                        timerField.index = owner.fields.size();
                        timerField.synthetic = true;
                        timerField.sourceName = seqSrc;
                        timerField.declarationSpan =
                            sequenceSyntax.identifierToken.span;
                        timerField.id = semantic::stableTypeId(
                            semantic::canonicalTypeName(owner) +
                            "::field:" + timerField.name);
                        owner.fields.push_back(timerField);
                        fieldNames.insert(timerField.name);

                        semantic::FieldSymbol stateField;
                        stateField.name = "$sequence_state_" +
                            sequenceSyntax.identifierToken.text;
                        stateField.type = semantic::PrimitiveType::Int;
                        stateField.index = owner.fields.size();
                        stateField.synthetic = true;
                        stateField.sourceName = seqSrc;
                        stateField.declarationSpan =
                            sequenceSyntax.identifierToken.span;
                        stateField.id = semantic::stableTypeId(
                            semantic::canonicalTypeName(owner) +
                            "::field:" + stateField.name);
                        owner.fields.push_back(stateField);
                        fieldNames.insert(stateField.name);

                        semantic::FieldSymbol completedField;
                        completedField.name = "$sequence_completed_" +
                            sequenceSyntax.identifierToken.text;
                        completedField.type = semantic::PrimitiveType::Bool;
                        completedField.index = owner.fields.size();
                        completedField.synthetic = true;
                        completedField.sourceName = seqSrc;
                        completedField.declarationSpan =
                            sequenceSyntax.identifierToken.span;
                        completedField.id = semantic::stableTypeId(
                            semantic::canonicalTypeName(owner) +
                            "::field:" + completedField.name);
                        owner.fields.push_back(completedField);
                        fieldNames.insert(completedField.name);

                        semantic::FieldSymbol resultField;
                        if (sequenceResultType.type !=
                            semantic::PrimitiveType::Void) {
                            resultField.name = "$sequence_result_" +
                                sequenceSyntax.identifierToken.text;
                            resultField.type = sequenceResultType.type;
                            resultField.typeName =
                                sequenceResultType.typeName;
                            resultField.index = owner.fields.size();
                            resultField.synthetic = true;
                            resultField.sourceName = seqSrc;
                            resultField.declarationSpan =
                                sequenceSyntax.resultType->span();
                            resultField.id = semantic::stableTypeId(
                                semantic::canonicalTypeName(owner) +
                                "::field:" + resultField.name);
                            owner.fields.push_back(resultField);
                            fieldNames.insert(resultField.name);
                        }

                        std::vector<std::pair<std::string,
                            semantic::FieldSymbol>> sequenceParameterFields;
                        sequenceParameterFields.emplace_back(
                            sequenceSyntax.parameters.front().identifierToken.text,
                            targetField);
                        for (std::size_t parameterIndex = 1;
                             parameterIndex < sequenceSyntax.parameters.size();
                             ++parameterIndex) {
                            const auto& parameter =
                                sequenceSyntax.parameters[parameterIndex];
                            semantic::FieldSymbol field;
                            field.name = "$sequence_parameter_" +
                                sequenceSyntax.identifierToken.text + "_" +
                                parameter.identifierToken.text;
                            field.type = sequenceParameterTypes[parameterIndex].type;
                            field.typeName =
                                sequenceParameterTypes[parameterIndex].typeName;
                            field.index = owner.fields.size();
                            field.synthetic = true;
                            field.sourceName = seqSrc;
                            field.declarationSpan = parameter.identifierToken.span;
                            field.id = semantic::stableTypeId(
                                semantic::canonicalTypeName(owner) +
                                "::field:" + field.name);
                            owner.fields.push_back(field);
                            fieldNames.insert(field.name);
                            sequenceParameterFields.emplace_back(
                                parameter.identifierToken.text, std::move(field));
                        }

                        std::vector<const
                            syntax::VariableDeclarationStatementSyntax*>
                            localDeclarations;
                        collectSequenceLocals(
                            sequenceSyntax.body, localDeclarations);
                        std::vector<std::pair<std::string,
                            semantic::FieldSymbol>> sequenceLocalFields;
                        std::unordered_set<std::string> sequenceLocalNames;
                        for (const auto& parameter : sequenceSyntax.parameters) {
                            sequenceLocalNames.insert(
                                parameter.identifierToken.text);
                        }
                        for (const auto* declaration : localDeclarations) {
                            if (!sequenceLocalNames.insert(
                                    declaration->identifierToken.text).second) {
                                result.diagnostics.report(
                                    "RS8801",
                                    "sequence local names must be unique across suspension scopes",
                                    declaration->identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                                continue;
                            }
                            auto field = makeSequenceLocalField(
                                owner,
                                sequenceSyntax,
                                *declaration,
                                module,
                                modules,
                                sequenceParameterFields,
                                sequenceLocalFields,
                                seqSrc,
                                result.diagnostics);
                            if (!field) {
                                module.invalid = true;
                                continue;
                            }
                            field->index = owner.fields.size();
                            owner.fields.push_back(*field);
                            fieldNames.insert(field->name);
                            sequenceLocalFields.emplace_back(
                                declaration->identifierToken.text,
                                std::move(*field));
                        }

                        std::vector<const syntax::ForeachStatementSyntax*>
                            suspendingForeach;
                        collectSuspendingSequenceForeach(
                            sequenceSyntax.body, suspendingForeach);
                        std::vector<semantic::FunctionBindingInput::
                            SequenceForeachFields> sequenceForeachFields;
                        const auto findNamedSequenceType =
                            [&](const std::string& name)
                                -> std::optional<InterfaceTypeRef> {
                            for (std::size_t index = 0;
                                 index < sequenceSyntax.parameters.size(); ++index) {
                                if (sequenceSyntax.parameters[index].identifierToken.text ==
                                    name) return sequenceParameterTypes[index];
                            }
                            for (const auto& field : sequenceLocalFields) {
                                if (field.first != name) continue;
                                return InterfaceTypeRef{
                                    field.second.type, field.second.typeName};
                            }
                            for (const auto& field : owner.fields) {
                                if (field.name != name || field.synthetic) continue;
                                return InterfaceTypeRef{field.type, field.typeName};
                            }
                            return std::nullopt;
                        };
                        std::function<std::optional<InterfaceTypeRef>(
                            const syntax::ExpressionSyntax&)>
                            inferSequenceExpressionType;
                        inferSequenceExpressionType =
                            [&](const syntax::ExpressionSyntax& expression)
                                -> std::optional<InterfaceTypeRef> {
                            switch (expression.kind()) {
                            case syntax::SyntaxKind::NameExpression:
                                return findNamedSequenceType(static_cast<const
                                    syntax::NameExpressionSyntax&>(expression)
                                        .identifierToken.text);
                            case syntax::SyntaxKind::ThisExpression:
                                return InterfaceTypeRef{
                                    semantic::PrimitiveType::Object,
                                    semantic::canonicalTypeName(owner)};
                            case syntax::SyntaxKind::ParenthesizedExpression:
                                return inferSequenceExpressionType(*static_cast<const
                                    syntax::ParenthesizedExpressionSyntax&>(
                                        expression).expression);
                            case syntax::SyntaxKind::NewArrayExpression: {
                                const auto& creation = static_cast<const
                                    syntax::NewArrayExpressionSyntax&>(expression);
                                auto element = resolveInterfaceType(
                                    creation.elementType, module.visibleTypes,
                                    result.diagnostics, seqSrc, false);
                                if (element.type == semantic::PrimitiveType::Error) {
                                    return std::nullopt;
                                }
                                return InterfaceTypeRef{
                                    semantic::PrimitiveType::Array,
                                    semantic::arrayTypeName(
                                        element.type, element.typeName)};
                            }
                            case syntax::SyntaxKind::NewObjectExpression:
                                return resolveInterfaceType(static_cast<const
                                    syntax::NewObjectExpressionSyntax&>(expression).type,
                                    module.visibleTypes, result.diagnostics,
                                    seqSrc, false);
                            case syntax::SyntaxKind::MemberAccessExpression: {
                                const auto& access = static_cast<const
                                    syntax::MemberAccessExpressionSyntax&>(expression);
                                if (access.receiver->kind() !=
                                    syntax::SyntaxKind::ThisExpression) {
                                    return std::nullopt;
                                }
                                for (const auto& field : owner.fields) {
                                    if (field.name == access.nameToken.text &&
                                        !field.synthetic) {
                                        return InterfaceTypeRef{
                                            field.type, field.typeName};
                                    }
                                }
                                return std::nullopt;
                            }
                            default:
                                return std::nullopt;
                            }
                        };
                        const auto makeSyntheticSequenceField =
                            [&](std::string name,
                                const InterfaceTypeRef& type,
                                text::TextSpan span) {
                            semantic::FieldSymbol field;
                            field.name = std::move(name);
                            field.declaringTypeId = owner.id;
                            field.declaringTypeName =
                                semantic::canonicalTypeName(owner);
                            field.type = type.type;
                            field.typeName = type.typeName;
                            field.index = owner.fields.size();
                            field.synthetic = true;
                            field.sourceName = seqSrc;
                            field.declarationSpan = span;
                            field.id = semantic::stableTypeId(
                                semantic::canonicalTypeName(owner) +
                                "::field:" + field.name);
                            owner.fields.push_back(field);
                            fieldNames.insert(field.name);
                            return field;
                        };
                        for (std::size_t foreachIndex = 0;
                             foreachIndex < suspendingForeach.size();
                             ++foreachIndex) {
                            const auto& loop = *suspendingForeach[foreachIndex];
                            auto collectionType = inferSequenceExpressionType(
                                *loop.collection);
                            if (!collectionType ||
                                (collectionType->type !=
                                     semantic::PrimitiveType::Array &&
                                 collectionType->type !=
                                     semantic::PrimitiveType::Object &&
                                 collectionType->type !=
                                     semantic::PrimitiveType::Struct)) {
                                result.diagnostics.report(
                                    "RS8807",
                                    "suspending foreach collection must have a statically identifiable array or object type",
                                    loop.collection->span(),
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                                continue;
                            }
                            bool usesEnumerator = false;
                            if (collectionType->type ==
                                    semantic::PrimitiveType::Object ||
                                collectionType->type ==
                                    semantic::PrimitiveType::Struct) {
                                const auto found = module.visibleTypes.find(
                                    collectionType->typeName);
                                bool hasCount = false;
                                bool hasGet = false;
                                std::optional<InterfaceTypeRef> enumeratorType;
                                if (found != module.visibleTypes.end()) {
                                    for (const auto& method : found->second.methods) {
                                        const auto arity = method.parameters.size() -
                                            ((!method.staticMethod && method.method)
                                                ? 1u : 0u);
                                        if (!method.staticMethod &&
                                            method.name == "Count" && arity == 0 &&
                                            method.returnType ==
                                                semantic::PrimitiveType::Int) {
                                            hasCount = true;
                                        }
                                        if (!method.staticMethod &&
                                            (method.name == "Get" || method.name == "GetAt") &&
                                            arity == 1 &&
                                            method.parameters.back().type ==
                                                semantic::PrimitiveType::Int) {
                                            hasGet = true;
                                        }
                                        if (!method.staticMethod &&
                                            method.name == "GetEnumerator" &&
                                            arity == 0 &&
                                            (method.returnType ==
                                                 semantic::PrimitiveType::Object ||
                                             method.returnType ==
                                                 semantic::PrimitiveType::Struct) &&
                                            !method.returnTypeName.empty()) {
                                            enumeratorType = InterfaceTypeRef{
                                                method.returnType,
                                                method.returnTypeName};
                                        }
                                    }
                                }

                                // Member symbols for generated generic and peer
                                // source types may not yet be attached to the
                                // preliminary visible-type table. Inspect the
                                // parsed declarations before choosing which
                                // durable foreach carrier field to emit.
                                if (!(hasCount && hasGet) && !enumeratorType) {
                                    for (const auto* candidateUnit : module.units) {
                                        const auto inspect = [&](const auto& types) {
                                            for (const auto& type : types) {
                                                const auto canonical =
                                                    candidateUnit->moduleName + "::" +
                                                    type.identifierToken.text;
                                                if (canonical !=
                                                    collectionType->typeName) {
                                                    continue;
                                                }
                                                for (const auto& method :
                                                     type.methods) {
                                                    if (method.modifiers.end() !=
                                                        std::find_if(
                                                            method.modifiers.begin(),
                                                            method.modifiers.end(),
                                                            [](const syntax::SyntaxToken& token) {
                                                                return token.kind ==
                                                                    syntax::SyntaxKind::StaticKeyword;
                                                            })) {
                                                        continue;
                                                    }
                                                    if (method.identifierToken.text ==
                                                            "Count" &&
                                                        method.parameters.empty() &&
                                                        method.returnType.name.text ==
                                                            "int" &&
                                                        !method.returnType.isArray()) {
                                                        hasCount = true;
                                                    }
                                                    if ((method.identifierToken.text ==
                                                             "Get" ||
                                                         method.identifierToken.text ==
                                                             "GetAt") &&
                                                        method.parameters.size() == 1 &&
                                                        method.parameters.front()
                                                                .type.name.text ==
                                                            "int" &&
                                                        !method.parameters.front()
                                                             .type.isArray()) {
                                                        hasGet = true;
                                                    }
                                                    if (method.identifierToken.text ==
                                                            "GetEnumerator" &&
                                                        method.parameters.empty()) {
                                                        const auto resolved =
                                                            resolveInterfaceType(
                                                                method.returnType,
                                                                module.visibleTypes,
                                                                result.diagnostics,
                                                                candidateUnit->source
                                                                    ->name(),
                                                                false);
                                                        if ((resolved.type ==
                                                                 semantic::PrimitiveType::
                                                                     Object ||
                                                             resolved.type ==
                                                                 semantic::PrimitiveType::
                                                                     Struct) &&
                                                            !resolved.typeName.empty()) {
                                                            enumeratorType = resolved;
                                                        }
                                                    }
                                                }
                                            }
                                        };
                                        inspect(candidateUnit->syntaxTree->classes);
                                        inspect(candidateUnit->syntaxTree->structs);
                                    }
                                }
                                if (!(hasCount && hasGet) && enumeratorType) {
                                    collectionType = *enumeratorType;
                                    usesEnumerator = true;
                                }
                            }
                            auto iterationType = resolveInterfaceType(
                                loop.type, module.visibleTypes,
                                result.diagnostics, seqSrc, false);
                            if (iterationType.type ==
                                semantic::PrimitiveType::Error) {
                                module.invalid = true;
                                continue;
                            }
                            if (semantic::isStackOnlyType(
                                    iterationType.type,
                                    iterationType.typeName,
                                    module.visibleTypes) ||
                                semantic::isStackOnlyType(
                                    collectionType->type,
                                    collectionType->typeName,
                                    module.visibleTypes)) {
                                result.diagnostics.report(
                                    "RS2696",
                                    "sequence '" + sequenceSyntax.identifierToken.text +
                                        "' cannot capture stack-only foreach variable '" +
                                        loop.identifierToken.text + "'",
                                    loop.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                                continue;
                            }
                            if (semantic::isStackOnlyArrayType(
                                    iterationType.type,
                                    iterationType.typeName,
                                    module.visibleTypes)) {
                                result.diagnostics.report(
                                    "RS2695",
                                    "stack-only struct '" +
                                        semantic::stackOnlyTypeDisplayName(
                                            iterationType.type,
                                            iterationType.typeName,
                                            module.visibleTypes) +
                                        "' cannot be an array element type",
                                    loop.type.span(),
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                                continue;
                            }
                            if (!sequenceLocalNames.insert(
                                    loop.identifierToken.text).second) {
                                result.diagnostics.report(
                                    "RS8801",
                                    "sequence local names must be unique across suspension scopes",
                                    loop.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                                continue;
                            }
                            const auto prefix = "$sequence_foreach_" +
                                sequenceSyntax.identifierToken.text + "_" +
                                std::to_string(foreachIndex);
                            semantic::FunctionBindingInput::
                                SequenceForeachFields fields;
                            fields.syntax = &loop;
                            fields.collection = makeSyntheticSequenceField(
                                prefix + "_collection", *collectionType,
                                loop.collection->span());
                            fields.index = makeSyntheticSequenceField(
                                prefix + "_index",
                                InterfaceTypeRef{
                                    semantic::PrimitiveType::Int, {}},
                                loop.identifierToken.span);
                            fields.iteration = makeSyntheticSequenceField(
                                prefix + "_iteration", iterationType,
                                loop.identifierToken.span);
                            fields.usesEnumerator = usesEnumerator;
                            sequenceLocalFields.emplace_back(
                                loop.identifierToken.text, fields.iteration);
                            sequenceForeachFields.push_back(std::move(fields));
                        }

                        std::vector<semantic::FunctionSymbol> callbacks;
                        const auto callbackCount = stateMachine
                            ? std::size_t{1}
                            : yields.size();
                        callbacks.reserve(callbackCount);
                        for (std::size_t callback = 0;
                             callback < callbackCount;
                             ++callback) {
                            callbacks.push_back(makeSequenceFunction(
                                moduleName,
                                owner,
                                sequenceSyntax,
                                "$sequence_" +
                                    sequenceSyntax.identifierToken.text + "_" +
                                    std::to_string(callback + 1),
                                false,
                                seqSrc));
                        }

                        auto entry = makeSequenceFunction(
                            moduleName,
                            owner,
                            sequenceSyntax,
                            sequenceSyntax.identifierToken.text,
                            true,
                            seqSrc,
                            sequenceParameterTypes);
                        bool seqDeterministic = module.deterministic || owner.deterministic;
                        for (const auto& list : sequenceSyntax.attributes) {
                            for (const auto& attr : list.attributes) {
                                if (attr.nameToken.text == "Deterministic") {
                                    seqDeterministic = true;
                                }
                            }
                        }
                        entry.deterministic = seqDeterministic;
                        for (auto& cb : callbacks) {
                            cb.deterministic = seqDeterministic;
                        }
                        semantic::FunctionBindingInput entryBinding;
                        entryBinding.symbol = entry;
                        entryBinding.sourceName = seqSrc;
                        entryBinding.parameterNames.push_back("this");
                        entryBinding.parameterSpans.push_back(
                            typeSyntax.identifierToken.span);
                        for (const auto& parameter : sequenceSyntax.parameters) {
                            entryBinding.parameterNames.push_back(
                                parameter.identifierToken.text);
                            entryBinding.parameterSpans.push_back(
                                parameter.identifierToken.span);
                        }
                        entryBinding.sequence = &sequenceSyntax;
                        entryBinding.sequenceSegment = 0;
                        entryBinding.sequenceTargetField = targetField;
                        entryBinding.sequenceTimerField = timerField;
                        entryBinding.sequenceStateField = stateField;
                        entryBinding.sequenceCompletedField = completedField;
                        entryBinding.sequenceResultField = resultField;
                        entryBinding.sequenceParameterFields =
                            sequenceParameterFields;
                        entryBinding.sequenceLocalFields = sequenceLocalFields;
                        entryBinding.sequenceForeachFields = sequenceForeachFields;
                        entryBinding.sequenceSingleYieldLoop = singleYieldLoop;
                        entryBinding.sequenceSingleYieldBranch = singleYieldBranch;
                        entryBinding.sequenceStateMachine = stateMachine;
                        if (!callbacks.empty()) {
                            entryBinding.sequenceNextCallback = callbacks.front();
                        }
                        owner.methods.push_back(entry);
                        addBinding(module,
                            std::move(entryBinding),
                            sequenceSyntax.identifierToken.span);

                        for (std::size_t callback = 0;
                             callback < callbacks.size();
                             ++callback) {
                            semantic::FunctionBindingInput binding;
                            binding.symbol = callbacks[callback];
                            binding.sourceName = seqSrc;
                            binding.parameterNames.push_back("this");
                            binding.parameterSpans.push_back(
                                typeSyntax.identifierToken.span);
                            binding.sequence = &sequenceSyntax;
                            binding.sequenceSegment = callback + 1;
                            binding.sequenceTargetField = targetField;
                            binding.sequenceTimerField = timerField;
                            binding.sequenceStateField = stateField;
                            binding.sequenceCompletedField = completedField;
                            binding.sequenceResultField = resultField;
                            binding.sequenceParameterFields =
                                sequenceParameterFields;
                            binding.sequenceLocalFields = sequenceLocalFields;
                            binding.sequenceForeachFields = sequenceForeachFields;
                            binding.sequenceSingleYieldLoop = singleYieldLoop;
                            binding.sequenceSingleYieldBranch = singleYieldBranch;
                            binding.sequenceStateMachine = stateMachine;
                            if ((singleYieldLoop || stateMachine) &&
                                callbacks.size() == 1) {
                                binding.sequenceNextCallback = callbacks[callback];
                            } else if (callback + 1 < callbacks.size()) {
                                binding.sequenceNextCallback =
                                    callbacks[callback + 1];
                            }
                            owner.methods.push_back(callbacks[callback]);
                            addBinding(module,
                                std::move(binding),
                                sequenceSyntax.identifierToken.span);
                        }

                        const auto addSequenceFieldGetter =
                            [&](std::string getterName,
                                const semantic::FieldSymbol& field) {
                            if (methodNames.find(getterName) !=
                                methodNames.end()) {
                                result.diagnostics.report(
                                    "RS8812",
                                    "sequence result method '" + getterName +
                                        "' conflicts with another member",
                                    sequenceSyntax.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    seqSrc);
                                module.invalid = true;
                                return;
                            }
                            methodNames.insert(getterName);
                            auto getter = makeSequenceFunction(
                                moduleName,
                                owner,
                                sequenceSyntax,
                                std::move(getterName),
                                false,
                                seqSrc);
                            getter.returnType = field.type;
                            getter.returnTypeName = field.typeName;
                            getter.synthetic = true;
                            getter.id = semantic::stableFunctionId(getter);
                            for (auto& parameter : getter.parameters) {
                                parameter.id = semantic::stableTypeId(
                                    std::to_string(getter.id) + "::local:" +
                                    std::to_string(parameter.index) + ":" +
                                    parameter.name);
                            }
                            semantic::FunctionBindingInput getterBinding;
                            getterBinding.symbol = getter;
                            getterBinding.sourceName = seqSrc;
                            getterBinding.parameterNames.push_back("this");
                            getterBinding.parameterSpans.push_back(
                                typeSyntax.identifierToken.span);
                            getterBinding.syntheticAutoGetter = true;
                            getterBinding.syntheticField = field;
                            owner.methods.push_back(getter);
                            addBinding(module,
                                std::move(getterBinding),
                                sequenceSyntax.identifierToken.span);
                        };
                        addSequenceFieldGetter(
                            "Is" + sequenceSyntax.identifierToken.text +
                                "Completed",
                            completedField);
                        if (sequenceResultType.type !=
                            semantic::PrimitiveType::Void) {
                            addSequenceFieldGetter(
                                "Get" +
                                    sequenceSyntax.identifierToken.text +
                                    "Result",
                                resultField);
                        }

                        const auto cancellation =
                            makeSequenceCancellationFunction(
                                moduleName,
                                owner,
                                sequenceSyntax,
                                seqSrc);
                        if (methodNames.find(cancellation.name) !=
                            methodNames.end()) {
                            result.diagnostics.report(
                                "RS8803",
                                "sequence cancellation method '" +
                                    cancellation.name + "' conflicts with another member",
                                sequenceSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                seqSrc);
                            module.invalid = true;
                        } else {
                            methodNames.insert(cancellation.name);
                            semantic::FunctionBindingInput cancellationBinding;
                            cancellationBinding.symbol = cancellation;
                            cancellationBinding.sourceName = seqSrc;
                            cancellationBinding.parameterNames.push_back("this");
                            cancellationBinding.parameterSpans.push_back(
                                typeSyntax.identifierToken.span);
                            cancellationBinding.sequenceCancellation = true;
                            cancellationBinding.sequence = &sequenceSyntax;
                            cancellationBinding.sequenceTimerField = timerField;
                            cancellationBinding.sequenceStateField = stateField;
                            cancellationBinding.sequenceCompletedField =
                                completedField;
                            cancellationBinding.sequenceResultField =
                                resultField;
                            cancellationBinding.sequenceParameterFields =
                                sequenceParameterFields;
                            cancellationBinding.sequenceLocalFields =
                                sequenceLocalFields;
                            owner.methods.push_back(cancellation);
                            addBinding(module,
                                std::move(cancellationBinding),
                                sequenceSyntax.identifierToken.span);
                        }

                        LanguageSequenceRecord record;
                        record.typeName = semantic::canonicalTypeName(owner);
                        record.name = sequenceSyntax.identifierToken.text;
                        record.resultTypeName =
                            sequenceResultType.typeName.empty()
                                ? std::string(semantic::primitiveTypeName(
                                      sequenceResultType.type))
                                : sequenceResultType.typeName;
                        record.sourceName = seqSrc;
                        record.offset =
                            sequenceSyntax.identifierToken.span.start;
                        for (const auto& callback : callbacks) {
                            record.callbacks.push_back(callback.name);
                        }
                        result.nativeSequences.push_back(std::move(record));
                    }
                    for (const auto& constructorSyntax : typeSyntax.constructors) {
                        const auto& ctorSrc = constructorSyntax.originSourceName.empty()
                            ? unit->source->name()
                            : constructorSyntax.originSourceName;
                        const bool isStaticCtor = semantic::hasModifier(
                            constructorSyntax.modifiers, syntax::SyntaxKind::StaticKeyword);
                        if (owner.staticType && !isStaticCtor) {
                            result.diagnostics.report(
                                "RS2489",
                                "static class '" + owner.name +
                                    "' cannot declare instance constructors",
                                constructorSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                ctorSrc);
                            module.invalid = true;
                        }
                        semantic::FunctionBindingInput binding;
                        diagnostics::DiagnosticBag ctorDeclBag;
                        binding.symbol = semantic::declareConstructorSymbol(
                            moduleName, constructorSyntax, owner,
                            module.visibleTypes, ctorDeclBag);
                        appendStampedDiagnostics(result.diagnostics, ctorDeclBag, ctorSrc);
                        if (ctorDeclBag.hasErrors()) module.invalid = true;
                        binding.sourceName = ctorSrc;
                        binding.symbol.sourceName = ctorSrc;
                        binding.body = &constructorSyntax.body;
                        binding.constructorSyntax = &constructorSyntax;
                        if (!binding.symbol.staticMethod) {
                            binding.parameterNames.push_back("this");
                            binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                            appendParameters(binding, constructorSyntax.parameters);
                            owner.constructors.push_back(binding.symbol);
                        } else {
                            if (owner.staticConstructor.has_value()) {
                                result.diagnostics.report(
                                    "RS2497",
                                    "type '" + owner.name +
                                        "' cannot declare more than one static constructor",
                                    constructorSyntax.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    ctorSrc);
                                module.invalid = true;
                            }
                            owner.staticConstructor = binding.symbol;
                        }
                        addBinding(module, std::move(binding), constructorSyntax.identifierToken.span);
                    }
                    for (const auto& propertySyntax : typeSyntax.properties) {
                        const auto& propSrc = propertySyntax.originSourceName.empty()
                            ? unit->source->name()
                            : propertySyntax.originSourceName;
                        if (!propertyNames.insert(
                                propertySyntax.identifierToken.text).second ||
                            fieldNames.find(propertySyntax.identifierToken.text) !=
                                fieldNames.end() ||
                            methodNames.find(propertySyntax.identifierToken.text) !=
                                methodNames.end()) {
                            result.diagnostics.report(
                                "RS2464",
                                "property '" + propertySyntax.identifierToken.text +
                                    "' conflicts with another member",
                                propertySyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                propSrc);
                            module.invalid = true;
                        }
                        diagnostics::DiagnosticBag propDeclBag;
                        auto property = semantic::declarePropertySymbol(
                            moduleName, propertySyntax, owner,
                            module.visibleTypes, propDeclBag);
                        appendStampedDiagnostics(result.diagnostics, propDeclBag, propSrc);
                        if (propDeclBag.hasErrors()) module.invalid = true;
                        if (owner.staticType && !property.staticProperty) {
                            result.diagnostics.report(
                                "RS2489",
                                "cannot declare instance property '" +
                                    property.name + "' in static class '" +
                                    owner.name + "'",
                                propertySyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                propSrc);
                            module.invalid = true;
                        }
                        property.sourceName = propSrc;
                        if (property.getter) property.getter->sourceName = propSrc;
                        if (property.setter) property.setter->sourceName = propSrc;
                        const bool getterAuto = propertySyntax.getter &&
                            propertySyntax.getter->semicolonToken.has_value();
                        const bool setterAuto = propertySyntax.setter &&
                            propertySyntax.setter->semicolonToken.has_value();
                        const bool getterExplicit = propertySyntax.getter &&
                            propertySyntax.getter->body != nullptr;
                        const bool setterExplicit = propertySyntax.setter &&
                            propertySyntax.setter->body != nullptr;
                        const bool autoProperty = getterAuto || setterAuto;
                        if (autoProperty && (getterExplicit || setterExplicit)) {
                            result.diagnostics.report(
                                "RS2463",
                                "property accessors cannot mix auto and explicit bodies",
                                propertySyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                propSrc);
                            module.invalid = true;
                        }
                        if (autoProperty && !property.getter) {
                            result.diagnostics.report(
                                "RS2463",
                                "auto-implemented properties must declare a get accessor",
                                propertySyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                propSrc);
                            module.invalid = true;
                        }
                        if (!autoProperty && propertySyntax.initializer) {
                            result.diagnostics.report(
                                "RS2465",
                                "only auto-implemented properties can have initializers",
                                propertySyntax.equalsToken
                                    ? propertySyntax.equalsToken->span
                                    : propertySyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                propSrc);
                            module.invalid = true;
                        }
                        if (autoProperty) {
                            property.backingFieldIndex = owner.fields.size();
                            semantic::FieldSymbol backing;
                            backing.name = "$" + property.name;
                            backing.type = property.type;
                            backing.typeName = property.typeName;
                            backing.index = owner.fields.size();
                            backing.declaringTypeId = owner.id;
                            backing.declaringTypeName = semantic::canonicalTypeName(owner);
                            backing.synthetic = true;
                            backing.staticField = property.staticProperty;
                            backing.readonlyField = !property.setter.has_value();
                            backing.initializerSyntax = propertySyntax.initializer.get();
                            backing.sourceName = propSrc;
                            backing.declarationSpan = propertySyntax.identifierToken.span;
                            backing.id = semantic::stableTypeId(
                                semantic::canonicalTypeName(owner) +
                                "::field:" + backing.name);
                            owner.fields.push_back(std::move(backing));
                            if (propertySyntax.initializer) {
                                module.visibleTypes[owner.name] = owner;
                                module.visibleTypes[semantic::canonicalTypeName(owner)] = owner;
                                (void)semantic::resolveConstantFieldsPass(
                                    owner, module.visibleTypes);
                                module.visibleTypes[owner.name] = owner;
                                module.visibleTypes[semantic::canonicalTypeName(owner)] = owner;
                                diagnostics::DiagnosticBag constDiagnostics;
                                (void)semantic::validateConstantFields(
                                    owner, module.visibleTypes, constDiagnostics);
                                for (const auto& diag : constDiagnostics.items()) {
                                    result.diagnostics.report(
                                        diag.code,
                                        diag.message,
                                        diag.span,
                                        diag.severity,
                                        propSrc);
                                    if (diag.severity == diagnostics::DiagnosticSeverity::Error) {
                                        module.invalid = true;
                                    }
                                }
                            }
                        }
                        if (property.getter) {
                            semantic::FunctionBindingInput binding;
                            binding.symbol = *property.getter;
                            binding.sourceName = propSrc;
                            binding.symbol.sourceName = propSrc;
                            binding.body = propertySyntax.getter->body.get();
                            if (!binding.symbol.staticMethod) {
                                binding.parameterNames.push_back("this");
                                binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                            }
                            binding.syntheticAutoGetter =
                                propertySyntax.getter->semicolonToken.has_value();
                            if (autoProperty) {
                                binding.syntheticField = owner.fields[property.backingFieldIndex];
                            }
                            addBinding(module, std::move(binding), propertySyntax.identifierToken.span);
                        }
                        if (property.setter) {
                            semantic::FunctionBindingInput binding;
                            binding.symbol = *property.setter;
                            binding.sourceName = propSrc;
                            binding.symbol.sourceName = propSrc;
                            binding.body = propertySyntax.setter->body.get();
                            if (!binding.symbol.staticMethod) {
                                binding.parameterNames.push_back("this");
                                binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                            }
                            binding.parameterNames.push_back("value");
                            binding.parameterSpans.push_back(propertySyntax.identifierToken.span);
                            binding.syntheticAutoSetter =
                                propertySyntax.setter->semicolonToken.has_value();
                            if (autoProperty) {
                                binding.syntheticField = owner.fields[property.backingFieldIndex];
                            }
                            addBinding(module, std::move(binding), propertySyntax.identifierToken.span);
                        }
                        owner.properties.push_back(std::move(property));
                    }
                    if (!owner.staticConstructor.has_value()) {
                        bool hasDynamicStaticFieldInit = false;
                        for (const auto& field : owner.fields) {
                            if (field.declaringTypeId == owner.id &&
                                field.staticField && !field.constField &&
                                !field.hasConstantValue && field.initializerSyntax) {
                                hasDynamicStaticFieldInit = true;
                                break;
                            }
                        }
                        if (hasDynamicStaticFieldInit) {
                            semantic::FunctionBindingInput binding;
                            binding.symbol.moduleName = moduleName;
                            binding.symbol.name = ".cctor";
                            binding.symbol.ownerTypeName = owner.name;
                            binding.symbol.ownerTypeId = owner.id;
                            binding.symbol.declaringTypeId = owner.id;
                            binding.symbol.declaringTypeName = semantic::canonicalTypeName(owner);
                            binding.symbol.returnType = semantic::PrimitiveType::Void;
                            binding.symbol.method = true;
                            binding.symbol.staticMethod = true;
                            binding.symbol.constructor = true;
                            binding.symbol.synthetic = true;
                            binding.symbol.deterministic = owner.deterministic;
                            binding.sourceName = unit->source->name();
                            binding.symbol.sourceName = binding.sourceName;
                            binding.symbol.id = semantic::stableFunctionId(binding.symbol);
                            binding.symbol.declarationSpan = typeSyntax.identifierToken.span;
                            binding.symbol.bodySpan = typeSyntax.identifierToken.span;
                            owner.staticConstructor = binding.symbol;
                            addBinding(module, std::move(binding), typeSyntax.identifierToken.span);
                        }
                    }
                    if (owner.kind == semantic::TypeKind::Class &&
                        !owner.staticType && !owner.interfaceType &&
                        owner.constructors.empty()) {
                        bool hasOwnDynamicInstanceFieldInit = false;
                        for (const auto& field : owner.fields) {
                            if (field.declaringTypeId == owner.id &&
                                !field.staticField && !field.constField &&
                                !field.hasConstantValue && field.initializerSyntax) {
                                hasOwnDynamicInstanceFieldInit = true;
                                break;
                            }
                        }
                        bool baseAllowsParameterlessChain = true;
                        bool baseHasParameterlessCtor = false;
                        if (base && !base->constructors.empty()) {
                            baseAllowsParameterlessChain = false;
                            for (const auto& baseCtor : base->constructors) {
                                if (baseCtor.staticMethod ||
                                    baseCtor.accessibility == semantic::Accessibility::Private) {
                                    continue;
                                }
                                bool allDefaulted = true;
                                for (std::size_t pi = 1; pi < baseCtor.parameters.size(); ++pi) {
                                    if (!baseCtor.parameters[pi].hasDefaultValue &&
                                        !baseCtor.parameters[pi].paramsArray) {
                                        allDefaulted = false;
                                        break;
                                    }
                                }
                                if (allDefaulted) {
                                    baseAllowsParameterlessChain = true;
                                    baseHasParameterlessCtor = true;
                                    break;
                                }
                            }
                        }
                        if (baseAllowsParameterlessChain &&
                            (hasOwnDynamicInstanceFieldInit || baseHasParameterlessCtor)) {
                            semantic::FunctionBindingInput binding;
                            binding.symbol.moduleName = moduleName;
                            binding.symbol.name = ".ctor";
                            binding.symbol.ownerTypeName = owner.name;
                            binding.symbol.ownerTypeId = owner.id;
                            binding.symbol.declaringTypeId = owner.id;
                            binding.symbol.declaringTypeName = semantic::canonicalTypeName(owner);
                            binding.symbol.returnType = semantic::PrimitiveType::Void;
                            binding.symbol.method = true;
                            binding.symbol.staticMethod = false;
                            binding.symbol.constructor = true;
                            binding.symbol.synthetic = true;
                            binding.symbol.accessibility = semantic::Accessibility::Public;
                            binding.symbol.deterministic = owner.deterministic;
                            binding.sourceName = unit->source->name();
                            binding.symbol.sourceName = binding.sourceName;
                            semantic::VariableSymbol self;
                            self.name = "this";
                            self.type = semantic::PrimitiveType::Object;
                            self.typeName = semantic::canonicalTypeName(owner);
                            self.index = 0;
                            self.storageType = self.type;
                            self.storageTypeName = self.typeName;
                            self.parameter = true;
                            self.declarationSpan = typeSyntax.identifierToken.span;
                            binding.symbol.parameters.push_back(std::move(self));
                            binding.symbol.id = semantic::stableFunctionId(binding.symbol);
                            binding.symbol.parameters[0].id = semantic::stableTypeId(
                                std::to_string(binding.symbol.id) + "::local:0:this");
                            binding.symbol.declarationSpan = typeSyntax.identifierToken.span;
                            binding.symbol.bodySpan = typeSyntax.identifierToken.span;
                            binding.parameterNames.push_back("this");
                            binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                            owner.constructors.push_back(binding.symbol);
                            addBinding(module, std::move(binding), typeSyntax.identifierToken.span);
                        }
                    }
                    if (!typeSyntax.events.empty()) {
                        if (owner.kind == semantic::TypeKind::Struct) {
                            result.diagnostics.report(
                                "RS8315",
                                "events require a class owner",
                                typeSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                unit->source->name());
                            module.invalid = true;
                        } else {
                            std::unordered_map<std::string, std::size_t>
                                eventIndices;
                            for (const auto& eventSyntax : typeSyntax.events) {
                                const auto& eventSrc = eventSyntax.originSourceName.empty()
                                    ? unit->source->name()
                                    : eventSyntax.originSourceName;
                                if (fieldNames.find(
                                        eventSyntax.identifierToken.text) !=
                                        fieldNames.end() ||
                                    methodNames.find(
                                        eventSyntax.identifierToken.text) !=
                                        methodNames.end() ||
                                    propertyNames.find(
                                        eventSyntax.identifierToken.text) !=
                                        propertyNames.end() ||
                                    eventIndices.find(
                                        eventSyntax.identifierToken.text) !=
                                        eventIndices.end()) {
                                    result.diagnostics.report(
                                        "RS2464",
                                        "event '" +
                                            eventSyntax.identifierToken.text +
                                            "' conflicts with another member",
                                        eventSyntax.identifierToken.span,
                                        diagnostics::DiagnosticSeverity::Error,
                                        eventSrc);
                                    module.invalid = true;
                                    continue;
                                }
                                const auto delegate =
                                    module.visibleDelegates.find(
                                        eventSyntax.delegateType.name.text);
                                if (delegate ==
                                    module.visibleDelegates.end()) {
                                    result.diagnostics.report(
                                        "RS8301",
                                        "unknown event delegate '" +
                                            eventSyntax.delegateType.name.text +
                                            "'",
                                        eventSyntax.delegateType.span(),
                                        diagnostics::DiagnosticSeverity::Error,
                                        eventSrc);
                                    module.invalid = true;
                                    continue;
                                }
                                if (delegate->second.returnType.type !=
                                    semantic::PrimitiveType::Void) {
                                    result.diagnostics.report(
                                        "RS8302",
                                        "event delegate must return void",
                                        eventSyntax.delegateType.span(),
                                        diagnostics::DiagnosticSeverity::Error,
                                        eventSrc);
                                    module.invalid = true;
                                }
                                semantic::EventSymbol event;
                                event.accessibility =
                                    semantic::accessibilityFromModifiers(
                                        eventSyntax.modifiers);
                                event.declaringTypeId = owner.id;
                                event.declaringTypeName =
                                    semantic::canonicalTypeName(owner);
                                event.name = eventSyntax.identifierToken.text;
                                event.delegateName = canonicalDelegateName(
                                    delegate->second.moduleName,
                                    delegate->second.name);
                                event.sourceName = eventSrc;
                                event.declarationSpan =
                                    eventSyntax.identifierToken.span;
                                event.id = semantic::stableTypeId(
                                    semantic::canonicalTypeName(owner) +
                                    "::event:" + event.name);
                                for (std::size_t parameter = 0;
                                     parameter <
                                         delegate->second.parameters.size();
                                     ++parameter) {
                                    semantic::VariableSymbol value;
                                    value.name = "$arg" +
                                        std::to_string(parameter);
                                    value.type = delegate->second
                                        .parameters[parameter].type;
                                    value.typeName = delegate->second
                                        .parameters[parameter].typeName;
                                    value.storageType = value.type;
                                    value.storageTypeName = value.typeName;
                                    value.index = parameter;
                                    value.parameter = true;
                                    event.parameters.push_back(
                                        std::move(value));
                                }
                                semantic::FieldSymbol storage;
                                storage.name = "$event_" + event.name +
                                    "_delegate";
                                storage.accessibility =
                                    semantic::Accessibility::Private;
                                storage.declaringTypeId = owner.id;
                                storage.declaringTypeName =
                                    semantic::canonicalTypeName(owner);
                                storage.type =
                                    semantic::PrimitiveType::Object;
                                storage.typeName = event.delegateName;
                                storage.index = owner.fields.size();
                                storage.synthetic = true;
                                storage.sourceName = eventSrc;
                                storage.declarationSpan =
                                    eventSyntax.identifierToken.span;
                                storage.id = semantic::stableTypeId(
                                    semantic::canonicalTypeName(owner) +
                                    "::field:" + storage.name);
                                event.storageField = storage;
                                owner.fields.push_back(storage);
                                eventIndices.emplace(
                                    event.name,
                                    owner.events.size());
                                owner.events.push_back(std::move(event));
                            }

                            std::vector<const
                                syntax::EventSubscriptionStatementSyntax*>
                                subscriptions;
                            for (const auto& method : typeSyntax.methods) {
                                collectEventSubscriptions(
                                    method.body, subscriptions);
                            }
                            for (const auto& constructor :
                                 typeSyntax.constructors) {
                                collectEventSubscriptions(
                                    constructor.body, subscriptions);
                            }
                            for (const auto& property :
                                 typeSyntax.properties) {
                                if (property.getter &&
                                    property.getter->body) {
                                    collectEventSubscriptions(
                                        *property.getter->body,
                                        subscriptions);
                                }
                                if (property.setter &&
                                    property.setter->body) {
                                    collectEventSubscriptions(
                                        *property.setter->body,
                                        subscriptions);
                                }
                            }
                            for (const auto& sequence :
                                 typeSyntax.sequences) {
                                collectEventSubscriptions(
                                    sequence.body, subscriptions);
                            }
                            std::stable_sort(
                                subscriptions.begin(),
                                subscriptions.end(),
                                [](const auto* left, const auto* right) {
                                    return left->span().start <
                                        right->span().start;
                                });
                            // Phase 20 binds each subscription as a normal,
                            // target-typed delegate expression. The Phase 18
                            // precomputed boolean-slot pass remains below only
                            // for source compatibility with older compiler
                            // inputs and intentionally receives no work.
                            subscriptions.clear();

                            std::unordered_map<std::string,
                                semantic::FieldSymbol> handlerSlots;
                            std::size_t lambdaOrdinal = 0;
                            for (const auto* subscription : subscriptions) {
                                const auto foundEvent = eventIndices.find(
                                    subscription->eventNameToken.text);
                                if (foundEvent == eventIndices.end()) {
                                    result.diagnostics.report(
                                        "RS8303",
                                        "unknown event '" +
                                            subscription->eventNameToken.text +
                                            "'",
                                        subscription->eventNameToken.span,
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit->source->name());
                                    module.invalid = true;
                                    continue;
                                }
                                auto& event = owner.events[
                                    foundEvent->second];
                                const auto delegate =
                                    module.visibleDelegates.find(
                                        event.delegateName);
                                if (delegate ==
                                    module.visibleDelegates.end()) {
                                    module.invalid = true;
                                    continue;
                                }

                                semantic::FunctionSymbol handler;
                                std::string slotKey;
                                if (subscription->handler->kind() ==
                                    syntax::SyntaxKind::NameExpression) {
                                    const auto& name = static_cast<const
                                        syntax::NameExpressionSyntax&>(
                                            *subscription->handler);
                                    bool matched = false;
                                    for (const auto& candidate :
                                         owner.methods) {
                                        if (candidate.name ==
                                                name.identifierToken.text &&
                                            eventMethodMatches(
                                                candidate,
                                                delegate->second)) {
                                            if (matched) {
                                                result.diagnostics.report(
                                                    "RS8304",
                                                    "event method group is ambiguous",
                                                    name.span(),
                                                    diagnostics::DiagnosticSeverity::Error,
                                                    unit->source->name());
                                                module.invalid = true;
                                                break;
                                            }
                                            handler = candidate;
                                            matched = true;
                                        }
                                    }
                                    if (!matched) {
                                        result.diagnostics.report(
                                            "RS8305",
                                            "event handler method does not match delegate",
                                            name.span(),
                                            diagnostics::DiagnosticSeverity::Error,
                                            unit->source->name());
                                        module.invalid = true;
                                        continue;
                                    }
                                    slotKey = "method:" +
                                        std::to_string(handler.id);
                                } else if (subscription->handler->kind() ==
                                    syntax::SyntaxKind::LambdaExpression) {
                                    if (subscription->operatorToken.kind ==
                                        syntax::SyntaxKind::MinusEqualsToken) {
                                        result.diagnostics.report(
                                            "RS8306",
                                            "lambda event handlers cannot be removed in the bounded Phase 18 model",
                                            subscription->span(),
                                            diagnostics::DiagnosticSeverity::Error,
                                            unit->source->name());
                                        module.invalid = true;
                                        continue;
                                    }
                                    const auto& lambda = static_cast<const
                                        syntax::LambdaExpressionSyntax&>(
                                            *subscription->handler);
                                    handler = makeEventLambdaFunction(
                                        moduleName,
                                        owner,
                                        delegate->second,
                                        lambda,
                                        event.name,
                                        ++lambdaOrdinal,
                                        unit->source->name(),
                                        result.diagnostics);
                                    semantic::FunctionBindingInput binding;
                                    binding.symbol = handler;
                                    binding.sourceName =
                                        unit->source->name();
                                    binding.eventLambda = &lambda;
                                    binding.parameterNames.push_back("this");
                                    binding.parameterSpans.push_back(
                                        typeSyntax.identifierToken.span);
                                    for (std::size_t parameter = 0;
                                         parameter <
                                             delegate->second.parameters.size();
                                         ++parameter) {
                                        binding.parameterNames.push_back(
                                            parameter <
                                                    lambda.parameterTokens.size()
                                                ? lambda.parameterTokens[
                                                    parameter].text
                                                : "$arg" +
                                                    std::to_string(parameter));
                                        binding.parameterSpans.push_back(
                                            parameter <
                                                    lambda.parameterTokens.size()
                                                ? lambda.parameterTokens[
                                                    parameter].span
                                                : lambda.arrowToken.span);
                                    }
                                    owner.methods.push_back(handler);
                                    addBinding(module,
                                        std::move(binding),
                                        lambda.arrowToken.span);
                                    slotKey = "lambda:" +
                                        std::to_string(lambda.span().start);
                                } else {
                                    result.diagnostics.report(
                                        "RS8307",
                                        "event handler must be a method group or lambda",
                                        subscription->handler->span(),
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit->source->name());
                                    module.invalid = true;
                                    continue;
                                }

                                auto slot = handlerSlots.find(
                                    event.name + ":" + slotKey);
                                if (slot == handlerSlots.end()) {
                                    semantic::FieldSymbol field;
                                    field.name = "$event_" + event.name +
                                        "_slot_" +
                                        std::to_string(event.handlers.size());
                                    field.type =
                                        semantic::PrimitiveType::Bool;
                                    field.index = owner.fields.size();
                                    field.synthetic = true;
                                    field.sourceName = unit->source->name();
                                    field.declarationSpan =
                                        subscription->eventNameToken.span;
                                    field.id = semantic::stableTypeId(
                                        semantic::canonicalTypeName(owner) +
                                        "::field:" + field.name);
                                    owner.fields.push_back(field);
                                    event.handlers.push_back(
                                        semantic::EventHandlerSymbol{
                                            handler, field});
                                    slot = handlerSlots.emplace(
                                        event.name + ":" + slotKey,
                                        field).first;
                                }
                                event.subscriptions.push_back(
                                    semantic::EventSubscriptionSymbol{
                                        subscription->span(),
                                        slot->second,
                                        subscription->operatorToken.kind ==
                                            syntax::SyntaxKind::PlusEqualsToken});
                            }
                        }
                    }
                    // Synthesize default interface implementations (Trait Mixins)
                    if (!owner.interfaceType && !owner.delegateType && !owner.staticType) {
                        std::map<std::string, const InterfaceContract*> allContractsByName;
                        const auto findGlobalInterfaceContractById =
                            [&](semantic::SymbolId ifaceId) -> const InterfaceContract* {
                            for (const auto& [mName, mWork] : modules) {
                                (void)mName;
                                for (const auto& [iName, c] : mWork.interfaces) {
                                    (void)iName;
                                    if (semantic::stableTypeId(
                                            canonicalInterfaceName(c.moduleName, c.name)) == ifaceId) {
                                        return &c;
                                    }
                                }
                            }
                            return nullptr;
                        };
                        for (const auto& inheritedMap : owner.interfaceDispatchMaps) {
                            if (const auto* ic = findGlobalInterfaceContractById(
                                    inheritedMap.interfaceTypeId)) {
                                allContractsByName[canonicalInterfaceName(ic->moduleName, ic->name)] = ic;
                            }
                        }
                        for (std::size_t ifaceIdx = 0;
                             ifaceIdx < typeSyntax.interfaces.size();
                             ++ifaceIdx) {
                            const auto& ifaceSyn = typeSyntax.interfaces[ifaceIdx];
                            if (ifaceIdx == 0 && !owner.baseTypeName.empty()) {
                                const auto cand = module.visibleTypes.find(ifaceSyn.name.text);
                                if (cand != module.visibleTypes.end() &&
                                    semantic::canonicalTypeName(cand->second) == owner.baseTypeName) {
                                    continue;
                                }
                            }
                            const auto foundIface = module.visibleInterfaces.find(ifaceSyn.name.text);
                            if (foundIface != module.visibleInterfaces.end()) {
                                const auto canonIface = canonicalInterfaceName(
                                    foundIface->second.moduleName, foundIface->second.name);
                                allContractsByName[canonIface] = &foundIface->second;
                            }
                        }

                        if (!allContractsByName.empty()) {
                            auto matchesReq = [](const semantic::FunctionSymbol& candidate,
                                                 const InterfaceMethodContract& required) -> bool {
                                if (candidate.staticMethod || candidate.name != required.name) {
                                    return false;
                                }
                                const std::size_t paramOffset =
                                    candidate.method && !candidate.staticMethod ? 1u : 0u;
                                if (candidate.parameters.size() < paramOffset ||
                                    candidate.parameters.size() - paramOffset !=
                                        required.parameters.size()) {
                                    return false;
                                }
                                if (!sameInterfaceType(
                                        required.returnType,
                                        candidate.returnType,
                                        candidate.returnTypeName)) {
                                    return false;
                                }
                                for (std::size_t p = 0; p < required.parameters.size(); ++p) {
                                    const auto& actual = candidate.parameters[p + paramOffset];
                                    const auto reqMod = p < required.parameterModifiers.size()
                                        ? required.parameterModifiers[p]
                                        : semantic::ParameterModifier::None;
                                    if (actual.modifier != reqMod ||
                                        !sameInterfaceType(
                                            required.parameters[p],
                                            actual.type,
                                            actual.typeName)) {
                                        return false;
                                    }
                                }
                                return true;
                            };

                            struct DefaultProvider {
                                std::string canonicalInterface;
                                const InterfaceMethodContract* method = nullptr;
                            };
                            std::map<std::string, std::vector<DefaultProvider>> providersBySig;
                            std::map<std::string, const InterfaceMethodContract*> sampleBySig;
                            for (const auto& [canonIface, contractPtr] : allContractsByName) {
                                for (const auto& req : contractPtr->methods) {
                                    const auto sig = interfaceMethodSignature(req);
                                    sampleBySig.try_emplace(sig, &req);
                                    if (req.hasDefaultImplementation &&
                                        req.methodSyntax &&
                                        req.methodSyntax->body) {
                                        providersBySig[sig].push_back(
                                            DefaultProvider{canonIface, &req});
                                    }
                                }
                            }

                            for (const auto& [sig, providers] : providersBySig) {
                                if (providers.empty()) continue;
                                const auto* reqSample = sampleBySig.at(sig);
                                bool hasExplicitImpl = false;
                                std::optional<std::size_t> existingSynthIdx;
                                std::optional<std::size_t> existingAbstractIdx;
                                for (std::size_t mIdx = 0; mIdx < owner.methods.size(); ++mIdx) {
                                    const auto& cand = owner.methods[mIdx];
                                    if (!matchesReq(cand, *reqSample)) continue;
                                    if (!cand.synthetic) {
                                        if (!cand.abstractMethod || owner.abstractType) {
                                            hasExplicitImpl = true;
                                            break;
                                        }
                                        existingAbstractIdx = mIdx;
                                    } else {
                                        existingSynthIdx = mIdx;
                                    }
                                }
                                if (hasExplicitImpl) continue;

                                if (providers.size() > 1) {
                                    std::ostringstream conflictList;
                                    for (std::size_t pi = 0; pi < providers.size(); ++pi) {
                                        if (pi != 0) conflictList << "' and '";
                                        conflictList << providers[pi].canonicalInterface;
                                    }
                                    result.diagnostics.report(
                                        "RS3415",
                                        "type '" + semantic::canonicalTypeName(owner) +
                                            "' does not implement interface member '" +
                                            reqSample->name +
                                            "' and inherits ambiguous default implementations from '" +
                                            conflictList.str() + "'",
                                        typeSyntax.identifierToken.span,
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit->source->name());
                                    module.invalid = true;
                                    continue;
                                }

                                if (existingSynthIdx.has_value()) {
                                    continue;
                                }

                                const auto& chosen = *providers.front().method;
                                const auto& ifaceMethodSyn = *chosen.methodSyntax;
                                bool addedHyg = false;
                                for (const auto& hm : chosen.hygienicModules) {
                                    if (hm != moduleName &&
                                        module.hygienicModules.insert(hm).second) {
                                        addedHyg = true;
                                    }
                                }
                                if (addedHyg) {
                                    refreshVisibleTypes(modules, module);
                                    refreshVisibleInterfaces(modules, module);
                                    refreshVisibleDelegates(modules, module);
                                }

                                auto synthFn = std::make_unique<syntax::FunctionDeclarationSyntax>();
                                synthFn->modifiers.push_back(syntax::SyntaxToken{
                                    syntax::SyntaxKind::PublicKeyword,
                                    ifaceMethodSyn.identifierToken.span,
                                    "public",
                                    {},
                                    true});
                                synthFn->returnType = ifaceMethodSyn.returnType;
                                synthFn->identifierToken = ifaceMethodSyn.identifierToken;
                                synthFn->openParenToken = ifaceMethodSyn.openParenToken;
                                for (const auto& p : ifaceMethodSyn.parameters) {
                                    synthFn->parameters.push_back(syntax::cloneParameterSyntax(p));
                                }
                                synthFn->commaTokens = ifaceMethodSyn.commaTokens;
                                synthFn->closeParenToken = ifaceMethodSyn.closeParenToken;
                                if (auto clonedBody = syntax::cloneStatementSyntax(*ifaceMethodSyn.body)) {
                                    synthFn->body = std::move(
                                        static_cast<syntax::BlockStatementSyntax&>(*clonedBody));
                                }

                                diagnostics::DiagnosticBag synthDeclBag;
                                auto synthSym = semantic::declareFunctionSymbol(
                                    moduleName,
                                    *synthFn,
                                    module.visibleTypes,
                                    synthDeclBag,
                                    &owner);
                                appendStampedDiagnostics(
                                    result.diagnostics, synthDeclBag, unit->source->name());
                                if (synthDeclBag.hasErrors()) module.invalid = true;
                                synthSym.accessibility = semantic::Accessibility::Public;
                                synthSym.synthetic = true;
                                synthSym.sourceName = unit->source->name();

                                if (owner.kind == semantic::TypeKind::Class) {
                                    synthSym.virtualMethod = true;
                                    if (existingAbstractIdx.has_value() &&
                                        owner.methods[*existingAbstractIdx].virtualSlot !=
                                            std::numeric_limits<std::uint32_t>::max()) {
                                        synthSym.virtualSlot =
                                            owner.methods[*existingAbstractIdx].virtualSlot;
                                        if (owner.virtualDispatchTable.size() <= synthSym.virtualSlot) {
                                            owner.virtualDispatchTable.resize(
                                                static_cast<std::size_t>(synthSym.virtualSlot) + 1, 0);
                                        }
                                        owner.virtualDispatchTable[synthSym.virtualSlot] = synthSym.id;
                                        owner.methods[*existingAbstractIdx] = synthSym;
                                    } else {
                                        synthSym.virtualSlot = static_cast<std::uint32_t>(
                                            owner.virtualDispatchTable.size());
                                        owner.virtualDispatchTable.push_back(synthSym.id);
                                        owner.methods.push_back(synthSym);
                                    }
                                } else {
                                    owner.methods.push_back(synthSym);
                                }

                                semantic::FunctionBindingInput synthBinding;
                                synthBinding.symbol = synthSym;
                                synthBinding.sourceName = unit->source->name();
                                synthBinding.body = &synthFn->body;
                                synthBinding.parameterNames.push_back("this");
                                synthBinding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                                synthBinding.hygienicAccessibleModules.insert(
                                    chosen.hygienicModules.begin(),
                                    chosen.hygienicModules.end());
                                appendParameters(synthBinding, synthFn->parameters);
                                module.synthesizedMethods.push_back(std::move(synthFn));
                                addBinding(module, std::move(synthBinding), typeSyntax.identifierToken.span);
                            }
                        }
                    }

                    validateConcreteClassType(owner, module, result);
                    *ownerPointer = std::move(owner);
                    module.visibleTypes[ownerPointer->name] = *ownerPointer;
                    module.visibleTypes[semantic::canonicalTypeName(*ownerPointer)] = *ownerPointer;
            };

    struct MemberDeclarationWork {
        std::size_t depth = 0;
        std::string canonicalName;
        std::string sourceName;
        std::string moduleName;
        ModuleWork* module = nullptr;
        const ParsedUnit* unit = nullptr;
        const syntax::ClassDeclarationSyntax* classSyntax = nullptr;
        const syntax::StructDeclarationSyntax* structSyntax = nullptr;
    };
    const auto memberDepth = [&](const semantic::TypeSymbol& owner) {
        std::size_t depth = 0;
        std::unordered_set<semantic::SymbolId> visited;
        auto current = owner.baseTypeName;
        while (!current.empty()) {
            const auto* base = findGlobalType(modules, current);
            if (!base || !visited.insert(base->id).second) break;
            ++depth;
            current = base->baseTypeName;
        }
        return depth;
    };
    std::vector<MemberDeclarationWork> memberDeclarations;
    for (auto& [moduleName, module] : modules) {
        if (module.frontendCached) continue;
        for (const auto* unit : module.units) {
            for (const auto& declaration : unit->syntaxTree->classes) {
                if (!declaration.typeParameters.empty()) continue;
                const auto* owner = findOwnType(
                    module, declaration.identifierToken.text);
                if (!owner) continue;
                memberDeclarations.push_back({
                    memberDepth(*owner),
                    semantic::canonicalTypeName(*owner),
                    unit->source->name(),
                    moduleName, &module, unit, &declaration, nullptr});
            }
            for (const auto& declaration : unit->syntaxTree->structs) {
                if (!declaration.typeParameters.empty()) continue;
                const auto* owner = findOwnType(
                    module, declaration.identifierToken.text);
                if (!owner) continue;
                memberDeclarations.push_back({
                    0, semantic::canonicalTypeName(*owner),
                    unit->source->name(),
                    moduleName, &module, unit, nullptr, &declaration});
            }
        }
    }
    std::sort(
        memberDeclarations.begin(), memberDeclarations.end(),
        [](const auto& left, const auto& right) {
            return std::tie(
                       left.depth, left.canonicalName, left.sourceName) <
                std::tie(
                       right.depth, right.canonicalName, right.sourceName);
        });
    for (const auto& work : memberDeclarations) {
        if (work.classSyntax) {
            processMembers(
                *work.classSyntax, work.unit,
                work.moduleName, *work.module);
        } else if (work.structSyntax) {
            processMembers(
                *work.structSyntax, work.unit,
                work.moduleName, *work.module);
        }
    }

    for (auto& moduleEntry : modules) {
        const auto& moduleName = moduleEntry.first;
        auto& module = moduleEntry.second;
        if (module.frontendCached) continue;

        // Materialize compiler-owned reference boxes. They are real runtime
        // descriptors but synthetic source symbols, so they stay out of the
        // public language surface and tooling occurrences.
        for (const auto& declaration : module.declarations) {
            for (const auto& parameter : declaration.parameters) {
                if (parameter.modifier !=
                        semantic::ParameterModifier::Ref &&
                    parameter.modifier !=
                        semantic::ParameterModifier::Out) {
                    continue;
                }
                const auto canonicalName =
                    semantic::storageTypeNameOf(parameter);
                const auto prefix = moduleName.empty()
                    ? std::string{}
                    : moduleName + "::";
                const auto simpleName =
                    canonicalName.rfind(prefix, 0) == 0
                        ? canonicalName.substr(prefix.size())
                        : canonicalName;
                if (findOwnType(module, simpleName)) continue;

                semantic::TypeSymbol wrapper;
                wrapper.kind = semantic::TypeKind::Class;
                wrapper.synthetic = true;
                wrapper.moduleName = moduleName;
                wrapper.name = simpleName;
                wrapper.id = semantic::stableTypeId(wrapper);

                semantic::FieldSymbol valueField;
                valueField.name = "Value";
                valueField.type = parameter.type;
                valueField.typeName = parameter.typeName;
                valueField.index = 0;
                valueField.synthetic = true;
                valueField.id = semantic::stableTypeId(
                    semantic::canonicalTypeName(wrapper) +
                    "::field:Value");
                wrapper.fields.push_back(std::move(valueField));
                module.types.push_back(std::move(wrapper));
            }
        }
        refreshVisibleTypes(modules, module);

        // Collect native attributes from original syntax declarations.
        for (const auto* unit : module.units) {
            const auto collectTypeAttributes = [&](auto const& declarations) {
                for (const auto& declaration : declarations) {
                    const auto* owner =
                        findOwnType(module, declaration.identifierToken.text);
                    if (!owner) continue;
                    const auto ownerName = semantic::canonicalTypeName(*owner);
                    appendNativeAttributes(
                        result.nativeAttributes,
                        declaration.attributes,
                        ownerName,
                        *unit->source);
                    for (const auto& field : declaration.fields) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            field.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "field",
                                field.identifierToken.text),
                            *unit->source);
                    }
                    for (const auto& method : declaration.methods) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            method.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "method",
                                method.identifierToken.text,
                                method.parameters.size()),
                            *unit->source);
                    }
                    for (const auto& sequence : declaration.sequences) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            sequence.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "method",
                                sequence.identifierToken.text,
                                sequence.parameters.size()),
                            *unit->source);
                    }
                    for (const auto& constructor : declaration.constructors) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            constructor.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "ctor",
                                declaration.identifierToken.text,
                                constructor.parameters.size()),
                            *unit->source);
                    }
                    for (const auto& property : declaration.properties) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            property.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "property",
                                property.identifierToken.text),
                            *unit->source);
                    }
                }
            };
            collectTypeAttributes(unit->syntaxTree->classes);
            collectTypeAttributes(unit->syntaxTree->structs);
            for (const auto& enumeration : unit->syntaxTree->enums) {
                const auto* owner =
                    findOwnType(module, enumeration.identifierToken.text);
                if (!owner) continue;
                const auto ownerName = semantic::canonicalTypeName(*owner);
                appendNativeAttributes(
                    result.nativeAttributes,
                    enumeration.attributes,
                    ownerName,
                    *unit->source);
                for (const auto& member : enumeration.members) {
                    appendNativeAttributes(
                        result.nativeAttributes,
                        member.attributes,
                        memberAttributeTarget(
                            ownerName,
                            "enum",
                            member.identifierToken.text),
                        *unit->source);
                }
            }
            for (const auto& interfaceSyntax : unit->syntaxTree->interfaces) {
                if (!interfaceSyntax.typeParameters.empty()) continue;
                const auto interfaceName = canonicalInterfaceName(
                    moduleName,
                    interfaceSyntax.identifierToken.text);
                appendNativeAttributes(
                    result.nativeAttributes,
                    interfaceSyntax.attributes,
                    interfaceName,
                    *unit->source);
                for (const auto& method : interfaceSyntax.methods) {
                    appendNativeAttributes(
                        result.nativeAttributes,
                        method.attributes,
                        memberAttributeTarget(
                            interfaceName,
                            "method",
                            method.identifierToken.text,
                            method.parameters.size()),
                        *unit->source);
                }
            }
            for (const auto& function : unit->syntaxTree->functions) {
                appendNativeAttributes(
                    result.nativeAttributes,
                    function.attributes,
                    memberAttributeTarget(
                        moduleName,
                        "function",
                        function.identifierToken.text,
                        function.parameters.size()),
                    *unit->source);
            }
        }

        const auto findInterfaceType =
            [&](semantic::SymbolId interfaceTypeId)
                -> const semantic::TypeSymbol* {
                for (const auto& [name, type] : module.visibleTypes) {
                    (void)name;
                    if (type.id == interfaceTypeId &&
                        type.interfaceType) {
                        return &type;
                    }
                }
                return nullptr;
            };
        const auto findInterfaceContract =
            [&](semantic::SymbolId interfaceTypeId)
                -> const InterfaceContract* {
                for (const auto& [name, contract] :
                     module.visibleInterfaces) {
                    (void)name;
                    if (semantic::stableTypeId(canonicalInterfaceName(
                            contract.moduleName, contract.name)) ==
                        interfaceTypeId) {
                        return &contract;
                    }
                }
                    return nullptr;
                };
        const auto validateInterfaces = [&](auto const& declarations) {
            for (const auto& declaration : declarations) {
                const auto& typeSyntax = declarationReference(declaration);
                auto* owner = findOwnType(
                    module, typeSyntax.identifierToken.text);
                if (!owner) continue;
                LanguageInterfaceImplementation implementation;
                implementation.typeName =
                    semantic::canonicalTypeName(*owner);
                std::unordered_set<std::string> implementedNames;
                std::map<
                    semantic::SymbolId,
                    std::pair<
                        const InterfaceContract*,
                        const semantic::TypeSymbol*>>
                    runtimeContracts;

                const auto inheritRuntimeContracts =
                    [&](const semantic::TypeSymbol& source) {
                        for (const auto& inherited :
                             source.interfaceDispatchMaps) {
                            const auto* contract = findInterfaceContract(
                                inherited.interfaceTypeId);
                            const auto* descriptor = findInterfaceType(
                                inherited.interfaceTypeId);
                            if (contract && descriptor) {
                                runtimeContracts[
                                    inherited.interfaceTypeId] = {
                                        contract, descriptor};
                            }
                        }
                    };
                inheritRuntimeContracts(*owner);
                if (!owner->baseTypeName.empty()) {
                    const auto* base = findGlobalType(
                        modules, owner->baseTypeName);
                    if (base) inheritRuntimeContracts(*base);
                }

                for (std::size_t interfaceIndex = 0;
                     interfaceIndex < typeSyntax.interfaces.size();
                     ++interfaceIndex) {
                    const auto& interfaceSyntax =
                        typeSyntax.interfaces[interfaceIndex];
                    if (interfaceIndex == 0 &&
                        !owner->baseTypeName.empty()) {
                        const auto candidate = module.visibleTypes.find(
                            interfaceSyntax.name.text);
                        if (candidate != module.visibleTypes.end() &&
                            semantic::canonicalTypeName(candidate->second) ==
                                owner->baseTypeName) {
                            continue;
                        }
                    }
                    const auto found =
                        module.visibleInterfaces.find(
                            interfaceSyntax.name.text);
                    if (found == module.visibleInterfaces.end()) {
                        result.diagnostics.report(
                            "RS2473",
                            "unknown interface '" +
                                interfaceSyntax.name.text + "'",
                            interfaceSyntax.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            owner->sourceName);
                        module.invalid = true;
                        continue;
                    }
                    const auto canonicalName = canonicalInterfaceName(
                        found->second.moduleName,
                        found->second.name);
                    if (!implementedNames.insert(canonicalName).second) {
                        result.diagnostics.report(
                            "RS2474",
                            "interface '" + canonicalName +
                                "' is implemented more than once",
                            interfaceSyntax.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            owner->sourceName);
                        module.invalid = true;
                        continue;
                    }
                    const auto interfaceTypeId =
                        semantic::stableTypeId(canonicalName);
                    const auto* descriptor =
                        findInterfaceType(interfaceTypeId);
                    if (!descriptor) {
                        result.diagnostics.report(
                            "RS2531",
                            "interface type descriptor '" +
                                canonicalName + "' is unavailable",
                            interfaceSyntax.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            owner->sourceName);
                        module.invalid = true;
                        continue;
                    }
                    runtimeContracts[interfaceTypeId] = {
                        &found->second, descriptor};
                    implementation.interfaces.push_back(canonicalName);
                }

                std::vector<semantic::InterfaceDispatchMap> dispatchMaps;
                for (const auto& [interfaceTypeId, runtimeContract] :
                     runtimeContracts) {
                    const auto& contract = *runtimeContract.first;
                    const auto& descriptor = *runtimeContract.second;
                    semantic::InterfaceDispatchMap dispatchMap;
                    dispatchMap.interfaceTypeId = interfaceTypeId;
                    dispatchMap.slots.assign(
                        descriptor.methods.size(), 0);

                    for (std::size_t requiredIndex = 0;
                         requiredIndex < contract.methods.size();
                         ++requiredIndex) {
                        const auto& required =
                            contract.methods[requiredIndex];
                        const semantic::FunctionSymbol* matched = nullptr;
                        for (const auto& candidate : owner->methods) {
                            if (candidate.staticMethod ||
                                candidate.name != required.name) {
                                continue;
                            }
                            const auto parameterOffset =
                                candidate.method &&
                                    !candidate.staticMethod
                                    ? std::size_t{1}
                                    : std::size_t{0};
                            if (candidate.parameters.size() <
                                    parameterOffset ||
                                candidate.parameters.size() -
                                    parameterOffset !=
                                    required.parameters.size()) {
                                continue;
                            }
                            bool compatible = sameInterfaceType(
                                required.returnType,
                                candidate.returnType,
                                candidate.returnTypeName);
                            for (std::size_t parameter = 0;
                                 compatible &&
                                 parameter <
                                     required.parameters.size();
                                 ++parameter) {
                                const auto& actual =
                                    candidate.parameters[
                                        parameter +
                                        parameterOffset];
                                const auto reqMod = parameter < required.parameterModifiers.size()
                                    ? required.parameterModifiers[parameter]
                                    : semantic::ParameterModifier::None;
                                compatible = (actual.modifier == reqMod) &&
                                    sameInterfaceType(
                                        required.parameters[parameter],
                                        actual.type,
                                        actual.typeName);
                            }
                            if (compatible) {
                                matched = &candidate;
                                break;
                            }
                        }

                        const auto canonicalName =
                            canonicalInterfaceName(
                                contract.moduleName, contract.name);
                        if (!matched ||
                            (matched->abstractMethod &&
                             !owner->abstractType)) {
                            result.diagnostics.report(
                                "RS2475",
                                "type '" + implementation.typeName +
                                    "' does not implement interface method '" +
                                    canonicalName + "." +
                                    interfaceMethodSignature(required) + "'",
                                typeSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                owner->sourceName);
                            module.invalid = true;
                            continue;
                        }

                        if (owner->kind ==
                                semantic::TypeKind::Class &&
                            requiredIndex <
                                descriptor.methods.size()) {
                            const auto slot =
                                descriptor.methods[requiredIndex]
                                    .interfaceSlot;
                            if (slot >= dispatchMap.slots.size()) {
                                result.diagnostics.report(
                                    "RS2532",
                                    "interface method slot is invalid for '" +
                                        canonicalName + "'",
                                    typeSyntax.identifierToken.span,
                                    diagnostics::DiagnosticSeverity::Error,
                                    owner->sourceName);
                                module.invalid = true;
                            } else {
                                dispatchMap.slots[slot] =
                                    matched->abstractMethod
                                        ? 0
                                        : matched->id;
                            }
                        }
                    }

                    // Struct interface conformance remains compile-time-only
                    // until Phase 23 introduces boxing.
                    if (owner->kind == semantic::TypeKind::Class) {
                        dispatchMaps.push_back(
                            std::move(dispatchMap));
                    }
                }
                if (owner->kind == semantic::TypeKind::Class) {
                    owner->interfaceDispatchMaps =
                        std::move(dispatchMaps);
                }

                if (!implementation.interfaces.empty()) {
                    std::sort(
                        implementation.interfaces.begin(),
                        implementation.interfaces.end());
                    result.nativeInterfaces.push_back(
                        std::move(implementation));
                }
            }
        };
        std::vector<const syntax::ClassDeclarationSyntax*>
            classDeclarations;
        std::vector<const syntax::StructDeclarationSyntax*>
            structDeclarations;
        for (const auto* unit : module.units) {
            for (const auto& declaration :
                 unit->syntaxTree->classes) {
                classDeclarations.push_back(&declaration);
            }
            for (const auto& declaration :
                 unit->syntaxTree->structs) {
                structDeclarations.push_back(&declaration);
            }
        }
        const auto classDepth =
            [&](const syntax::ClassDeclarationSyntax* declaration) {
                const auto* owner = findOwnType(
                    module, declaration->identifierToken.text);
                std::size_t depth = 0;
                std::unordered_set<semantic::SymbolId> visited;
                auto current = owner
                    ? owner->baseTypeName
                    : std::string{};
                while (!current.empty()) {
                    const auto* base =
                        findGlobalType(modules, current);
                    if (!base ||
                        !visited.insert(base->id).second) {
                        break;
                    }
                    ++depth;
                    current = base->baseTypeName;
                }
                return depth;
            };
        std::stable_sort(
            classDeclarations.begin(),
            classDeclarations.end(),
            [&](const auto* left, const auto* right) {
                return classDepth(left) < classDepth(right);
            });
        validateInterfaces(classDeclarations);
        validateInterfaces(structDeclarations);
        refreshVisibleTypes(modules, module);

        std::vector<std::string> signatures;
        signatures.reserve(
            module.declarations.size() + module.types.size() +
            module.interfaces.size());
        for (const auto& type : module.types) {
            if (!type.synthetic) {
                signatures.push_back(typeSignature(type));
            }
        }
        for (const auto& [delegateName, contract] : module.delegates) {
            (void)delegateName;
            std::ostringstream delegateSignature;
            delegateSignature << "delegate:"
                << canonicalDelegateName(contract.moduleName, contract.name)
                << '(';
            for (std::size_t index = 0;
                 index < contract.parameters.size(); ++index) {
                const auto& parameter = contract.parameters[index];
                delegateSignature << semantic::primitiveTypeName(
                    parameter.type) << '#' << parameter.typeName << ':'
                    << static_cast<int>(
                        contract.parameterModifiers[index]) << ';';
            }
            delegateSignature << ")->"
                << semantic::primitiveTypeName(contract.returnType.type)
                << '#' << contract.returnType.typeName;
            signatures.push_back(delegateSignature.str());
        }
        for (const auto& [interfaceName, contract] : module.interfaces) {
            (void)interfaceName;
            std::ostringstream interfaceSignature;
            interfaceSignature << "interface:"
                << canonicalInterfaceName(contract.moduleName, contract.name)
                << '{';
            for (const auto& method : contract.methods) {
                interfaceSignature << interfaceMethodSignature(method)
                    << (method.hasDefaultImplementation ? "=default;" : ";");
            }
            interfaceSignature << '}';
            signatures.push_back(interfaceSignature.str());
        }
        for (const auto& attribute : result.nativeAttributes) {
            if (attribute.target.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream attributeSignature;
            attributeSignature << "attribute:"
                << attribute.target << ':' << attribute.name << '(';
            for (const auto& argument : attribute.arguments) {
                attributeSignature << argument.name << '='
                    << argument.value << ',';
            }
            attributeSignature << ')';
            signatures.push_back(attributeSignature.str());
        }
        for (const auto& sequence : result.nativeSequences) {
            if (sequence.typeName.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream sequenceSignature;
            sequenceSignature << "sequence:"
                << sequence.typeName << ':' << sequence.name << ':'
                << sequence.resultTypeName << '(';
            for (const auto& callback : sequence.callbacks) {
                sequenceSignature << callback << ',';
            }
            sequenceSignature << ')';
            signatures.push_back(sequenceSignature.str());
        }
        for (const auto& implementation : result.nativeInterfaces) {
            if (implementation.typeName.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream implementationSignature;
            implementationSignature << "implements:"
                << implementation.typeName << ':';
            for (const auto& interfaceName : implementation.interfaces) {
                implementationSignature << interfaceName << ',';
            }
            signatures.push_back(implementationSignature.str());
        }
        for (const auto& function : module.declarations) {
            signatures.push_back(semantic::canonicalFunctionSignature(function));
        }
        std::sort(signatures.begin(), signatures.end());
        std::ostringstream publicSurface;
        for (const auto& signature : signatures) publicSurface << signature << '\n';
        module.publicFingerprint = stableFingerprint(publicSurface.str());
    }
}

} // namespace realscript::compiler::detail
