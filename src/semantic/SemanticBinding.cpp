#include "realscript/semantic/Semantic.h"
#include "FlowAnalysis.h"

#include <unordered_set>
#include <utility>

namespace realscript::semantic {

Binder::Binder(diagnostics::DiagnosticBag& diagnostics)
    : diagnostics_(diagnostics) {}

SemanticModel Binder::bind(const syntax::CompilationUnitSyntax& syntaxTree) {
    ModuleBindingInput input;
    input.moduleName = syntaxTree.moduleDeclaration
        ? syntaxTree.moduleDeclaration->fullName()
        : "";
    input.units = {&syntaxTree};

    for (const auto& classSyntax : syntaxTree.classes) {
        auto type = declareTypeShell(input.moduleName, classSyntax);
        input.visibleTypes[type.name] = type;
        input.visibleTypes[canonicalTypeName(type)] = type;
        input.types.push_back(type);
    }
    for (std::size_t index = 0; index < syntaxTree.classes.size(); ++index) {
        (void)populateTypeFields(
            input.types[index],
            syntaxTree.classes[index],
            input.visibleTypes,
            diagnostics_);
        input.visibleTypes[input.types[index].name] = input.types[index];
        input.visibleTypes[canonicalTypeName(input.types[index])] = input.types[index];
    }

    std::unordered_set<std::string> functionKeys;
    for (const auto& functionSyntax : syntaxTree.functions) {
        auto symbol = declareFunctionSymbol(
            input.moduleName,
            functionSyntax,
            input.visibleTypes,
            diagnostics_);
        const auto key = canonicalFunctionKey(symbol);
        if (!functionKeys.insert(key).second) {
            diagnostics_.report(
                "RS2000",
                "function overload '" + key + "' is already declared",
                functionSyntax.identifierToken.span);
        }
        input.visibleFunctions[symbol.name].push_back(symbol);
        input.declarations.push_back(std::move(symbol));
    }

    return bindModule(input);
}

SemanticModel Binder::bindModule(const ModuleBindingInput& input) {
    visibleFunctions_ = input.visibleFunctions;
    visibleTypes_ = input.visibleTypes;

    SemanticModel result;
    result.moduleName = input.moduleName;
    result.types = input.types;

    std::size_t declarationIndex = 0;
    for (const auto* unit : input.units) {
        for (const auto& functionSyntax : unit->functions) {
            if (declarationIndex >= input.declarations.size()) {
                diagnostics_.report(
                    "RS2009",
                    "function declaration table is incomplete",
                    functionSyntax.identifierToken.span);
                continue;
            }
            result.functions.push_back(bindFunction(
                functionSyntax,
                input.declarations[declarationIndex++]));
        }
    }

    return result;
}

BoundFunction Binder::bindFunction(
    const syntax::FunctionDeclarationSyntax& syntaxTree,
    const FunctionSymbol& symbol) {
    BoundFunction result;
    result.symbol = symbol;

    scopes_.clear();
    pushScope();
    currentReturnType_ = symbol.returnType;
    currentReturnTypeName_ = symbol.returnTypeName;
    nextVariableIndex_ = symbol.parameters.size();

    for (std::size_t i = 0; i < symbol.parameters.size(); ++i) {
        auto parameter = symbol.parameters[i];
        parameter.name = syntaxTree.parameters[i].identifierToken.text;
        (void)declareVariable(
            parameter,
            syntaxTree.parameters[i].identifierToken.span);
        result.symbol.parameters[i].name = parameter.name;
    }

    result.body = bindBlockStatement(syntaxTree.body, false);
    result.variableCount = nextVariableIndex_;

    if (result.symbol.returnType != PrimitiveType::Void &&
        result.symbol.returnType != PrimitiveType::Error &&
        detail::canReachFunctionEnd(result, diagnostics_)) {
        diagnostics_.report(
            "RS2001",
            "not all control-flow paths in function '" +
                result.symbol.name + "' return a value",
            syntaxTree.identifierToken.span);
    }

    popScope();
    return result;
}

std::unique_ptr<BoundBlockStatement> Binder::bindBlockStatement(
    const syntax::BlockStatementSyntax& syntaxTree,
    bool createScope) {
    if (createScope) {
        pushScope();
    }

    auto result = std::make_unique<BoundBlockStatement>();
    result->span = syntaxTree.span();
    for (const auto& statement : syntaxTree.statements) {
        result->statements.push_back(bindStatement(*statement));
    }

    if (createScope) {
        popScope();
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindEmbeddedStatement(
    const syntax::StatementSyntax& syntaxTree) {
    pushScope();
    auto result = bindStatement(syntaxTree);
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindStatement(
    const syntax::StatementSyntax& syntaxTree) {
    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::BlockStatement:
        return bindBlockStatement(
            static_cast<const syntax::BlockStatementSyntax&>(syntaxTree),
            true);
    case syntax::SyntaxKind::ReturnStatement:
        return bindReturnStatement(
            static_cast<const syntax::ReturnStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::IfStatement:
        return bindIfStatement(
            static_cast<const syntax::IfStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::WhileStatement:
        return bindWhileStatement(
            static_cast<const syntax::WhileStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::VariableDeclarationStatement:
        return bindVariableDeclaration(
            static_cast<const syntax::VariableDeclarationStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ExpressionStatement:
        return bindExpressionStatement(
            static_cast<const syntax::ExpressionStatementSyntax&>(syntaxTree));
    default:
        diagnostics_.report(
            "RS2099",
            "unsupported statement kind",
            syntaxTree.span());
        return std::make_unique<BoundExpressionStatement>();
    }
}

std::unique_ptr<BoundStatement> Binder::bindReturnStatement(
    const syntax::ReturnStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundReturnStatement>();
    result->span = syntaxTree.span();

    if (currentReturnType_ == PrimitiveType::Void) {
        if (syntaxTree.expression) {
            diagnostics_.report(
                "RS2002",
                "void function cannot return a value",
                syntaxTree.expression->span());
            result->expression = bindExpression(*syntaxTree.expression);
        }
        return result;
    }

    if (!syntaxTree.expression) {
        diagnostics_.report(
            "RS2003",
            "function returning '" +
                std::string(primitiveTypeName(currentReturnType_)) +
                "' must return a value",
            syntaxTree.returnKeyword.span);
        return result;
    }

    result->expression = convertExpression(
        bindExpression(*syntaxTree.expression),
        currentReturnType_,
        syntaxTree.expression->span(),
        "return value",
        currentReturnTypeName_);
    if (result->expression &&
        (currentReturnType_ == PrimitiveType::Object ||
         currentReturnType_ == PrimitiveType::Array ||
         currentReturnType_ == PrimitiveType::Handle)) {
        result->expression->typeName = currentReturnTypeName_;
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindIfStatement(
    const syntax::IfStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundIfStatement>();
    result->span = syntaxTree.span();
    result->condition = convertExpression(
        bindExpression(*syntaxTree.condition),
        PrimitiveType::Bool,
        syntaxTree.condition->span(),
        "if condition");
    result->thenStatement = bindEmbeddedStatement(*syntaxTree.thenStatement);
    if (syntaxTree.elseStatement) {
        result->elseStatement = bindEmbeddedStatement(*syntaxTree.elseStatement);
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindWhileStatement(
    const syntax::WhileStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundWhileStatement>();
    result->span = syntaxTree.span();
    result->condition = convertExpression(
        bindExpression(*syntaxTree.condition),
        PrimitiveType::Bool,
        syntaxTree.condition->span(),
        "while condition");
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration(
    const syntax::VariableDeclarationStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundVariableDeclarationStatement>();
    result->span = syntaxTree.span();
    std::string declaredTypeName;
    result->variable = {
        syntaxTree.identifierToken.text,
        bindType(syntaxTree.type, false, &declaredTypeName),
        declaredTypeName,
        nextVariableIndex_++,
        false,
    };

    (void)declareVariable(result->variable, syntaxTree.identifierToken.span);
    if (syntaxTree.initializer) {
        result->initializer = convertExpression(
            bindExpression(*syntaxTree.initializer),
            result->variable.type,
            syntaxTree.initializer->span(),
            "initializer",
            result->variable.typeName);
        if (result->initializer &&
            (result->variable.type == PrimitiveType::Object ||
             result->variable.type == PrimitiveType::Array ||
             result->variable.type == PrimitiveType::Handle)) {
            if (!result->initializer->typeName.empty() &&
                result->initializer->typeName != result->variable.typeName) {
                diagnostics_.report(
                    "RS2410",
                    "cannot initialize '" + result->variable.typeName +
                        "' with '" + result->initializer->typeName + "'",
                    syntaxTree.initializer->span());
            }
            result->initializer->typeName = result->variable.typeName;
        }
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindExpressionStatement(
    const syntax::ExpressionStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundExpressionStatement>();
    result->span = syntaxTree.span();
    result->expression = bindExpression(*syntaxTree.expression);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindExpression(
    const syntax::ExpressionSyntax& syntaxTree) {
    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::LiteralExpression:
        return bindLiteralExpression(
            static_cast<const syntax::LiteralExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::NameExpression:
        return bindNameExpression(
            static_cast<const syntax::NameExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::UnaryExpression:
        return bindUnaryExpression(
            static_cast<const syntax::UnaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::BinaryExpression:
        return bindBinaryExpression(
            static_cast<const syntax::BinaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::AssignmentExpression:
        return bindAssignmentExpression(
            static_cast<const syntax::AssignmentExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ParenthesizedExpression:
        return bindExpression(
            *static_cast<const syntax::ParenthesizedExpressionSyntax&>(
                syntaxTree).expression);
    case syntax::SyntaxKind::CallExpression:
        return bindCallExpression(
            static_cast<const syntax::CallExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::NewObjectExpression:
        return bindNewObjectExpression(
            static_cast<const syntax::NewObjectExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::NewArrayExpression:
        return bindNewArrayExpression(
            static_cast<const syntax::NewArrayExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ElementAccessExpression:
        return bindElementAccessExpression(
            static_cast<const syntax::ElementAccessExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::MemberAccessExpression:
        return bindMemberAccessExpression(
            static_cast<const syntax::MemberAccessExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::MemberAssignmentExpression:
        return bindMemberAssignmentExpression(
            static_cast<const syntax::MemberAssignmentExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ElementAssignmentExpression:
        return bindElementAssignmentExpression(
            static_cast<const syntax::ElementAssignmentExpressionSyntax&>(syntaxTree));
    default:
        diagnostics_.report(
            "RS2199",
            "unsupported expression kind",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
}

PrimitiveType Binder::bindType(
    const syntax::TypeSyntax& syntaxTree,
    bool allowVoid,
    std::string* typeName) {
    if (typeName) typeName->clear();
    PrimitiveType baseType = resolvePrimitiveType(syntaxTree.name.text);
    std::string baseTypeName;
    if (baseType == PrimitiveType::Error) {
        const auto found = visibleTypes_.find(syntaxTree.name.text);
        if (found != visibleTypes_.end()) {
            baseType = PrimitiveType::Object;
            baseTypeName = canonicalTypeName(found->second);
        } else {
            diagnostics_.report(
                "RS2200",
                "unknown type '" + syntaxTree.name.text + "'",
                syntaxTree.span());
            return PrimitiveType::Error;
        }
    }

    if (syntaxTree.isArray()) {
        if (baseType == PrimitiveType::Void ||
            baseType == PrimitiveType::Array) {
            diagnostics_.report(
                "RS2201",
                "invalid array element type",
                syntaxTree.span());
            return PrimitiveType::Error;
        }
        if (typeName) *typeName = arrayTypeName(baseType, baseTypeName);
        return PrimitiveType::Array;
    }

    if (baseType == PrimitiveType::Void && !allowVoid) {
        diagnostics_.report(
            "RS2201",
            "void is not valid in this type position",
            syntaxTree.span());
        return PrimitiveType::Error;
    }
    if (typeName &&
        (baseType == PrimitiveType::Object ||
         baseType == PrimitiveType::Handle)) {
        *typeName = baseTypeName;
    }
    return baseType;
}

const VariableSymbol* Binder::lookupVariable(
    const std::string& name) const noexcept {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        const auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

bool Binder::declareVariable(VariableSymbol variable, text::TextSpan span) {
    auto& scope = scopes_.back();
    if (scope.find(variable.name) != scope.end()) {
        diagnostics_.report(
            "RS2202",
            "name '" + variable.name + "' is already declared in this scope",
            span);
        return false;
    }
    scope.emplace(variable.name, std::move(variable));
    return true;
}

void Binder::pushScope() {
    scopes_.emplace_back();
}

void Binder::popScope() {
    scopes_.pop_back();
}

std::unique_ptr<BoundErrorExpression> Binder::makeError(
    text::TextSpan span) const {
    auto result = std::make_unique<BoundErrorExpression>();
    result->type = PrimitiveType::Error;
    result->span = span;
    return result;
}

} // namespace realscript::semantic
