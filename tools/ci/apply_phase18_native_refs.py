#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    text = read(path)
    if new in text:
        return
    if old not in text:
        raise RuntimeError(f"anchor not found in {path}: {old[:100]!r}")
    write(path, text.replace(old, new, 1))


def replace_count(path: str, old: str, new: str, count: int) -> None:
    text = read(path)
    if text.count(new) >= count:
        return
    if text.count(old) < count:
        raise RuntimeError(f"expected {count} anchors in {path}: {old[:100]!r}")
    write(path, text.replace(old, new, count))


def replace_regex(path: str, pattern: str, replacement: str) -> None:
    text = read(path)
    if replacement in text:
        return
    updated, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"regex anchor not found in {path}: {pattern[:100]!r}")
    write(path, updated)


# Syntax model and parser.
replace_once(
    "include/realscript/syntax/Syntax.h",
    "    InKeyword,\n    DoKeyword,",
    "    InKeyword,\n    RefKeyword,\n    OutKeyword,\n    DoKeyword,")
replace_count(
    "include/realscript/syntax/Syntax.h",
    "    SyntaxToken openParenToken;\n    std::vector<std::unique_ptr<ExpressionSyntax>> arguments;",
    "    SyntaxToken openParenToken;\n    std::vector<std::optional<SyntaxToken>> argumentModifiers;\n    std::vector<std::unique_ptr<ExpressionSyntax>> arguments;",
    2)
replace_once(
    "include/realscript/syntax/Syntax.h",
    "struct ParameterSyntax final : SyntaxNode {\n    TypeSyntax type;",
    "struct ParameterSyntax final : SyntaxNode {\n    std::optional<SyntaxToken> modifierToken;\n    TypeSyntax type;")
replace_once(
    "include/realscript/syntax/Syntax.h",
    "    void parseArgumentList(\n        std::vector<std::unique_ptr<ExpressionSyntax>>& arguments,\n        std::vector<SyntaxToken>& commaTokens,\n        SyntaxToken& closeParenToken);",
    "    void parseArgumentList(\n        std::vector<std::unique_ptr<ExpressionSyntax>>& arguments,\n        std::vector<std::optional<SyntaxToken>>* argumentModifiers,\n        std::vector<SyntaxToken>& commaTokens,\n        SyntaxToken& closeParenToken);")

replace_once(
    "src/syntax/SyntaxFacts.cpp",
    "        RS_KIND(InKeyword);\n        RS_KIND(DoKeyword);",
    "        RS_KIND(InKeyword);\n        RS_KIND(RefKeyword);\n        RS_KIND(OutKeyword);\n        RS_KIND(DoKeyword);")
replace_once(
    "src/syntax/SyntaxFacts.cpp",
    "        {\"in\", SyntaxKind::InKeyword},\n        {\"do\", SyntaxKind::DoKeyword},",
    "        {\"in\", SyntaxKind::InKeyword},\n        {\"ref\", SyntaxKind::RefKeyword},\n        {\"out\", SyntaxKind::OutKeyword},\n        {\"do\", SyntaxKind::DoKeyword},")

replace_once(
    "src/syntax/SyntaxNodes.cpp",
    "text::TextSpan ParameterSyntax::span() const noexcept {\n    return combine(type.span(), identifierToken.span);\n}",
    "text::TextSpan ParameterSyntax::span() const noexcept {\n    return combine(\n        modifierToken ? modifierToken->span : type.span(),\n        identifierToken.span);\n}")

replace_once(
    "src/syntax/Parser.cpp",
    "ParameterSyntax Parser::parseParameter() {\n    ParameterSyntax result;\n    result.type = parseType();",
    "ParameterSyntax Parser::parseParameter() {\n    ParameterSyntax result;\n    if (current().kind == SyntaxKind::RefKeyword ||\n        current().kind == SyntaxKind::OutKeyword ||\n        current().kind == SyntaxKind::InKeyword) {\n        result.modifierToken = nextToken();\n    }\n    result.type = parseType();")
replace_regex(
    "src/syntax/Parser.cpp",
    r"void Parser::parseArgumentList\(\n    std::vector<std::unique_ptr<ExpressionSyntax>>& arguments,\n    std::vector<SyntaxToken>& commaTokens,\n    SyntaxToken& closeParenToken\) \{.*?\n\}",
    """void Parser::parseArgumentList(
    std::vector<std::unique_ptr<ExpressionSyntax>>& arguments,
    std::vector<std::optional<SyntaxToken>>* argumentModifiers,
    std::vector<SyntaxToken>& commaTokens,
    SyntaxToken& closeParenToken) {
    const auto parseArgument = [&]() {
        std::optional<SyntaxToken> modifier;
        if (current().kind == SyntaxKind::RefKeyword ||
            current().kind == SyntaxKind::OutKeyword ||
            current().kind == SyntaxKind::InKeyword) {
            modifier = nextToken();
        }
        if (argumentModifiers) argumentModifiers->push_back(modifier);
        arguments.push_back(parseExpression());
    };
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        parseArgument();
        while (current().kind == SyntaxKind::CommaToken) {
            commaTokens.push_back(nextToken());
            parseArgument();
        }
    }
    closeParenToken = match(SyntaxKind::CloseParenToken);
}""")
replace_once(
    "src/syntax/Parser.cpp",
    "                parseArgumentList(call->arguments, call->commaTokens, call->closeParenToken);",
    "                parseArgumentList(\n                    call->arguments,\n                    &call->argumentModifiers,\n                    call->commaTokens,\n                    call->closeParenToken);")
replace_once(
    "src/syntax/Parser.cpp",
    "        parseArgumentList(result->arguments, result->commaTokens, result->closeParenToken);\n        return result;\n    }\n\n    auto result = std::make_unique<NewArrayExpressionSyntax>();",
    "        parseArgumentList(\n            result->arguments, nullptr, result->commaTokens,\n            result->closeParenToken);\n        return result;\n    }\n\n    auto result = std::make_unique<NewArrayExpressionSyntax>();")
replace_once(
    "src/syntax/Parser.cpp",
    "    parseArgumentList(result->arguments, result->commaTokens, result->closeParenToken);\n    return result;\n}\n\nbool Parser::isVariableDeclarationStart()",
    "    parseArgumentList(\n        result->arguments, &result->argumentModifiers,\n        result->commaTokens, result->closeParenToken);\n    return result;\n}\n\nbool Parser::isVariableDeclarationStart()")

# Semantic symbols and Bound tree.
replace_once(
    "include/realscript/semantic/Semantic.h",
    "enum class ConversionKind {\n    None,",
    "enum class ParameterModifier {\n    None,\n    Ref,\n    Out,\n    In,\n};\n\nenum class ConversionKind {\n    None,")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    PrimitiveType type = PrimitiveType::Error;\n    std::string typeName;\n    std::size_t index = 0;",
    "    PrimitiveType type = PrimitiveType::Error;\n    std::string typeName;\n    PrimitiveType storageType = PrimitiveType::Error;\n    std::string storageTypeName;\n    ParameterModifier modifier = ParameterModifier::None;\n    std::size_t index = 0;")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "struct FieldSymbol {",
    "[[nodiscard]] inline PrimitiveType storageTypeOf(\n    const VariableSymbol& variable) noexcept {\n    return variable.storageType == PrimitiveType::Error\n        ? variable.type\n        : variable.storageType;\n}\n\n[[nodiscard]] inline const std::string& storageTypeNameOf(\n    const VariableSymbol& variable) noexcept {\n    return variable.storageType == PrimitiveType::Error\n        ? variable.typeName\n        : variable.storageTypeName;\n}\n\nstruct FieldSymbol {")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    TypeKind kind = TypeKind::Class;\n    std::string moduleName;",
    "    TypeKind kind = TypeKind::Class;\n    bool synthetic = false;\n    std::string moduleName;")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "[[nodiscard]] SymbolId stableTypeId(const std::string& canonicalName);",
    "[[nodiscard]] SymbolId stableTypeId(const std::string& canonicalName);\n[[nodiscard]] std::string referenceWrapperTypeName(\n    const std::string& moduleName,\n    PrimitiveType sourceType,\n    const std::string& sourceTypeName = {});")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    CallExpression,\n    NewObjectExpression,",
    "    CallExpression,\n    ReferenceCallExpression,\n    NewObjectExpression,")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "struct BoundNewObjectExpression final : BoundExpression {",
    "struct BoundReferenceCallArgument {\n    ParameterModifier modifier = ParameterModifier::None;\n    std::unique_ptr<BoundExpression> value;\n    VariableSymbol variable;\n    TypeSymbol wrapperType;\n    FieldSymbol valueField;\n    bool forwarded = false;\n};\n\nstruct BoundReferenceCallExpression final : BoundExpression {\n    FunctionSymbol function;\n    std::vector<BoundReferenceCallArgument> arguments;\n    [[nodiscard]] BoundNodeKind kind() const noexcept override {\n        return BoundNodeKind::ReferenceCallExpression;\n    }\n};\n\nstruct BoundNewObjectExpression final : BoundExpression {")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    [[nodiscard]] std::unique_ptr<BoundExpression> bindCallExpression(\n        const syntax::CallExpressionSyntax& syntax);",
    "    [[nodiscard]] std::unique_ptr<BoundExpression> bindCallExpression(\n        const syntax::CallExpressionSyntax& syntax);\n    [[nodiscard]] std::unique_ptr<BoundExpression> bindSelectedCall(\n        const FunctionSymbol& function,\n        std::vector<std::unique_ptr<BoundExpression>> arguments,\n        const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& syntaxArguments,\n        const std::vector<std::optional<syntax::SyntaxToken>>& argumentModifiers,\n        std::unique_ptr<BoundExpression> receiver,\n        text::TextSpan span,\n        const std::string& context);")

# Symbol declaration and stable source-level signatures.
replace_once(
    "src/semantic/Symbols.cpp",
    "ResolvedType resolveType(\n    const syntax::TypeSyntax& syntaxTree,",
    "ParameterModifier parameterModifier(\n    const std::optional<syntax::SyntaxToken>& token) noexcept {\n    if (!token) return ParameterModifier::None;\n    switch (token->kind) {\n    case syntax::SyntaxKind::RefKeyword: return ParameterModifier::Ref;\n    case syntax::SyntaxKind::OutKeyword: return ParameterModifier::Out;\n    case syntax::SyntaxKind::InKeyword: return ParameterModifier::In;\n    default: return ParameterModifier::None;\n    }\n}\n\nconst char* parameterModifierName(ParameterModifier modifier) noexcept {\n    switch (modifier) {\n    case ParameterModifier::None: return \"\";\n    case ParameterModifier::Ref: return \"ref \";\n    case ParameterModifier::Out: return \"out \";\n    case ParameterModifier::In: return \"in \";\n    }\n    return \"\";\n}\n\nResolvedType resolveType(\n    const syntax::TypeSyntax& syntaxTree,")
replace_once(
    "src/semantic/Symbols.cpp",
    "    self.parameter = true;\n    result.parameters.push_back(std::move(self));",
    "    self.storageType = self.type;\n    self.storageTypeName = self.typeName;\n    self.parameter = true;\n    result.parameters.push_back(std::move(self));")
replace_regex(
    "src/semantic/Symbols.cpp",
    r"void appendSyntaxParameters\(\n    FunctionSymbol& result,\n    const std::vector<syntax::ParameterSyntax>& parameters,\n    const TypeSymbolMap& visibleTypes,\n    diagnostics::DiagnosticBag& diagnostics\) \{.*?\n\}",
    """void appendSyntaxParameters(
    FunctionSymbol& result,
    const std::vector<syntax::ParameterSyntax>& parameters,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    for (const auto& parameterSyntax : parameters) {
        const auto parameterType = resolveType(
            parameterSyntax.type, visibleTypes, false);
        if (parameterType.type == PrimitiveType::Error) {
            diagnostics.report(
                "RS2201",
                "invalid parameter type '" +
                    parameterSyntax.type.name.text + "'",
                parameterSyntax.type.span());
        }
        VariableSymbol parameter;
        parameter.name = parameterSyntax.identifierToken.text;
        parameter.type = parameterType.type;
        parameter.typeName = parameterType.name;
        parameter.modifier = parameterModifier(
            parameterSyntax.modifierToken);
        if (parameter.modifier == ParameterModifier::Ref ||
            parameter.modifier == ParameterModifier::Out) {
            parameter.storageType = PrimitiveType::Object;
            parameter.storageTypeName = referenceWrapperTypeName(
                result.moduleName,
                parameter.type,
                parameter.typeName);
        } else {
            parameter.storageType = parameter.type;
            parameter.storageTypeName = parameter.typeName;
        }
        parameter.index = result.parameters.size();
        parameter.parameter = true;
        parameter.declarationSpan =
            parameterSyntax.identifierToken.span;
        result.parameters.push_back(std::move(parameter));
    }
}""")
replace_once(
    "src/semantic/Symbols.cpp",
    "SymbolId stableTypeId(const TypeSymbol& type) { return stableTypeId(canonicalTypeName(type)); }\nSymbolId stableTypeId(const std::string& canonicalName) {",
    "SymbolId stableTypeId(const TypeSymbol& type) { return stableTypeId(canonicalTypeName(type)); }\nSymbolId stableTypeId(const std::string& canonicalName) {")
replace_once(
    "src/semantic/Symbols.cpp",
    "    return canonicalName.empty() ? 0 : fnv1a(canonicalName);\n}\n\nTypeSymbol declareTypeShell(",
    "    return canonicalName.empty() ? 0 : fnv1a(canonicalName);\n}\n\nstd::string referenceWrapperTypeName(\n    const std::string& moduleName,\n    PrimitiveType sourceType,\n    const std::string& sourceTypeName) {\n    std::string key = isExactType(sourceType) &&\n            !sourceTypeName.empty()\n        ? sourceTypeName\n        : primitiveTypeName(sourceType);\n    for (auto& character : key) {\n        const auto alphaNumeric =\n            (character >= 'a' && character <= 'z') ||\n            (character >= 'A' && character <= 'Z') ||\n            (character >= '0' && character <= '9') ||\n            character == '_';\n        if (!alphaNumeric) character = '_';\n    }\n    std::string moduleKey = moduleName.empty() ? \"Global\" : moduleName;\n    for (auto& character : moduleKey) {\n        const auto alphaNumeric =\n            (character >= 'a' && character <= 'z') ||\n            (character >= 'A' && character <= 'Z') ||\n            (character >= '0' && character <= '9') ||\n            character == '_';\n        if (!alphaNumeric) character = '_';\n    }\n    const auto simpleName =\n        \"__RsRef__\" + moduleKey + \"__\" + key;\n    return moduleName.empty()\n        ? simpleName\n        : moduleName + \"::\" + simpleName;\n}\n\nTypeSymbol declareTypeShell(")
replace_regex(
    "src/semantic/Symbols.cpp",
    r"std::string canonicalFunctionKey\(const FunctionSymbol& function\) \{.*?\n\}",
    """std::string canonicalFunctionKey(const FunctionSymbol& function) {
    std::ostringstream out;
    out << function.moduleName << "::";
    if (!function.ownerTypeName.empty()) out << function.ownerTypeName << '.';
    out << function.name << '(';
    const auto firstVisible = function.method && !function.staticMethod &&
            !function.parameters.empty()
        ? 1u
        : 0u;
    for (std::size_t i = firstVisible;
         i < function.parameters.size(); ++i) {
        if (i != firstVisible) out << ',';
        out << parameterModifierName(function.parameters[i].modifier)
            << displayType(
                function.parameters[i].type,
                function.parameters[i].typeName);
    }
    out << ')';
    return out.str();
}""")
replace_once(
    "src/semantic/Symbols.cpp",
    "        valueParameter.parameter = true;\n        valueParameter.declarationSpan = syntaxTree.identifierToken.span;",
    "        valueParameter.storageType = valueParameter.type;\n        valueParameter.storageTypeName = valueParameter.typeName;\n        valueParameter.parameter = true;\n        valueParameter.declarationSpan = syntaxTree.identifierToken.span;")

# Native wrapper descriptors and public-surface filtering.
replace_once(
    "src/compiler/Compilation.cpp",
    "        // Collect native attributes from original syntax declarations.",
    "        // Materialize compiler-owned reference boxes. They are real runtime\n        // descriptors but synthetic source symbols, so they stay out of the\n        // public language surface and tooling occurrences.\n        for (const auto& declaration : module.declarations) {\n            for (const auto& parameter : declaration.parameters) {\n                if (parameter.modifier !=\n                        semantic::ParameterModifier::Ref &&\n                    parameter.modifier !=\n                        semantic::ParameterModifier::Out) {\n                    continue;\n                }\n                const auto canonicalName =\n                    semantic::storageTypeNameOf(parameter);\n                const auto prefix = moduleName.empty()\n                    ? std::string{}\n                    : moduleName + \"::\";\n                const auto simpleName =\n                    canonicalName.rfind(prefix, 0) == 0\n                        ? canonicalName.substr(prefix.size())\n                        : canonicalName;\n                if (findOwnType(module, simpleName)) continue;\n\n                semantic::TypeSymbol wrapper;\n                wrapper.kind = semantic::TypeKind::Class;\n                wrapper.synthetic = true;\n                wrapper.moduleName = moduleName;\n                wrapper.name = simpleName;\n                wrapper.id = semantic::stableTypeId(wrapper);\n\n                semantic::FieldSymbol valueField;\n                valueField.name = \"Value\";\n                valueField.type = parameter.type;\n                valueField.typeName = parameter.typeName;\n                valueField.index = 0;\n                valueField.synthetic = true;\n                valueField.id = semantic::stableTypeId(\n                    semantic::canonicalTypeName(wrapper) +\n                    \"::field:Value\");\n                wrapper.fields.push_back(std::move(valueField));\n                module.types.push_back(std::move(wrapper));\n            }\n        }\n        refreshVisibleTypes(modules, module);\n\n        // Collect native attributes from original syntax declarations.")
replace_once(
    "src/compiler/Compilation.cpp",
    "        for (const auto& type : module.types) {\n            signatures.push_back(typeSignature(type));\n        }",
    "        for (const auto& type : module.types) {\n            if (!type.synthetic) {\n                signatures.push_back(typeSignature(type));\n            }\n        }")

# Binder selection, source access, call construction.
replace_once(
    "src/semantic/SemanticExpressions.cpp",
    "std::unique_ptr<BoundVariableExpression> variableExpression(\n    const VariableSymbol& variable,\n    text::TextSpan span = {}) {",
    "ParameterModifier syntaxModifier(\n    const std::optional<syntax::SyntaxToken>& token) noexcept {\n    if (!token) return ParameterModifier::None;\n    switch (token->kind) {\n    case syntax::SyntaxKind::RefKeyword: return ParameterModifier::Ref;\n    case syntax::SyntaxKind::OutKeyword: return ParameterModifier::Out;\n    case syntax::SyntaxKind::InKeyword: return ParameterModifier::In;\n    default: return ParameterModifier::None;\n    }\n}\n\nstd::unique_ptr<BoundVariableExpression> variableExpression(\n    const VariableSymbol& variable,\n    text::TextSpan span = {}) {")
replace_once(
    "src/semantic/SemanticExpressions.cpp",
    "    result->variable = variable;\n    return result;\n}\n\nstd::size_t visibleParameterOffset",
    "    result->variable = variable;\n    return result;\n}\n\nstd::unique_ptr<BoundVariableExpression> storageVariableExpression(\n    const VariableSymbol& variable,\n    text::TextSpan span = {}) {\n    auto result = std::make_unique<BoundVariableExpression>();\n    result->span = span;\n    result->type = storageTypeOf(variable);\n    result->typeName = storageTypeNameOf(variable);\n    result->variable = variable;\n    result->variable.type = result->type;\n    result->variable.typeName = result->typeName;\n    return result;\n}\n\nstd::size_t visibleParameterOffset")
replace_regex(
    "src/semantic/SemanticExpressions.cpp",
    r"const FunctionSymbol\* selectBest\(\n    const std::vector<const FunctionSymbol\*>& candidates,\n    const std::vector<std::unique_ptr<BoundExpression>>& arguments,\n    bool& ambiguous\) \{.*?\n\}",
    """const FunctionSymbol* selectBest(
    const std::vector<const FunctionSymbol*>& candidates,
    const std::vector<std::unique_ptr<BoundExpression>>& arguments,
    const std::vector<std::optional<syntax::SyntaxToken>>* modifiers,
    bool& ambiguous) {
    const FunctionSymbol* best = nullptr;
    int bestScore = std::numeric_limits<int>::max();
    ambiguous = false;
    for (const auto* candidate : candidates) {
        const auto offset = visibleParameterOffset(*candidate);
        if (candidate->parameters.size() != arguments.size() + offset) {
            continue;
        }
        int score = 0;
        bool applicable = true;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto& parameter = candidate->parameters[i + offset];
            const auto suppliedModifier = modifiers && i < modifiers->size()
                ? syntaxModifier((*modifiers)[i])
                : ParameterModifier::None;
            if (suppliedModifier != parameter.modifier) {
                applicable = false;
                break;
            }
            if (parameter.modifier == ParameterModifier::None) {
                const auto rank = conversionRank(
                    arguments[i]->type, parameter.type);
                if (rank < 0 ||
                    (arguments[i]->type == parameter.type &&
                     !exactParameterMatch(*arguments[i], parameter))) {
                    applicable = false;
                    break;
                }
                score += rank;
            } else if (!exactParameterMatch(
                           *arguments[i], parameter)) {
                applicable = false;
                break;
            }
        }
        if (!applicable) continue;
        if (score < bestScore) {
            best = candidate;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore && best &&
                   best->id != candidate->id) {
            ambiguous = true;
        }
    }
    return best;
}""")
# Constructor selection does not accept reference modifiers yet.
text = read("src/semantic/SemanticExpressions.cpp")
text = text.replace("selectBest(constructors, arguments, ambiguous)",
                    "selectBest(constructors, arguments, nullptr, ambiguous)")
write("src/semantic/SemanticExpressions.cpp", text)

replace_once(
    "src/semantic/SemanticExpressions.cpp",
    "    if (const auto* variable = lookupVariable(syntaxTree.identifierToken.text)) {\n        return variableExpression(*variable, syntaxTree.span());\n    }",
    "    if (const auto* variable = lookupVariable(\n            syntaxTree.identifierToken.text)) {\n        if (variable->modifier == ParameterModifier::Ref ||\n            variable->modifier == ParameterModifier::Out) {\n            const auto wrapper = visibleTypes_.find(\n                storageTypeNameOf(*variable));\n            if (wrapper == visibleTypes_.end() ||\n                wrapper->second.fields.empty()) {\n                diagnostics_.report(\n                    \"RS8705\",\n                    \"reference parameter storage descriptor is unavailable\",\n                    syntaxTree.span());\n                return makeError(syntaxTree.span());\n            }\n            auto result =\n                std::make_unique<BoundMemberAccessExpression>();\n            result->span = syntaxTree.span();\n            result->type = variable->type;\n            result->typeName = variable->typeName;\n            result->receiver = storageVariableExpression(\n                *variable, syntaxTree.span());\n            result->ownerType = wrapper->second;\n            result->field = wrapper->second.fields.front();\n            return result;\n        }\n        return variableExpression(*variable, syntaxTree.span());\n    }")
replace_once(
    "src/semantic/SemanticExpressions.cpp",
    "    if (const auto* variable = lookupVariable(syntaxTree.identifierToken.text)) {\n        auto value = convertExpression(\n            bindExpression(*syntaxTree.expression), variable->type,\n            syntaxTree.expression->span(), \"assignment\", variable->typeName);\n        auto result = std::make_unique<BoundAssignmentExpression>();\n        result->span = syntaxTree.span();\n        result->type = variable->type;\n        result->typeName = variable->typeName;\n        result->variable = *variable;\n        result->expression = std::move(value);\n        return result;\n    }",
    "    if (const auto* variable = lookupVariable(\n            syntaxTree.identifierToken.text)) {\n        if (variable->modifier == ParameterModifier::In) {\n            diagnostics_.report(\n                \"RS8702\",\n                \"cannot assign to in parameter '\" +\n                    variable->name + \"'\",\n                syntaxTree.identifierToken.span);\n            return makeError(syntaxTree.span());\n        }\n        auto value = convertExpression(\n            bindExpression(*syntaxTree.expression), variable->type,\n            syntaxTree.expression->span(), \"assignment\",\n            variable->typeName);\n        if (variable->modifier == ParameterModifier::Ref ||\n            variable->modifier == ParameterModifier::Out) {\n            const auto wrapper = visibleTypes_.find(\n                storageTypeNameOf(*variable));\n            if (wrapper == visibleTypes_.end() ||\n                wrapper->second.fields.empty()) {\n                diagnostics_.report(\n                    \"RS8705\",\n                    \"reference parameter storage descriptor is unavailable\",\n                    syntaxTree.span());\n                return makeError(syntaxTree.span());\n            }\n            auto result =\n                std::make_unique<BoundMemberAssignmentExpression>();\n            result->span = syntaxTree.span();\n            result->type = variable->type;\n            result->typeName = variable->typeName;\n            result->receiver = storageVariableExpression(\n                *variable, syntaxTree.span());\n            result->ownerType = wrapper->second;\n            result->field = wrapper->second.fields.front();\n            result->expression = std::move(value);\n            return result;\n        }\n        auto result = std::make_unique<BoundAssignmentExpression>();\n        result->span = syntaxTree.span();\n        result->type = variable->type;\n        result->typeName = variable->typeName;\n        result->variable = *variable;\n        result->expression = std::move(value);\n        return result;\n    }")

replace_regex(
    "src/semantic/SemanticExpressions.cpp",
    r"std::unique_ptr<BoundExpression> Binder::bindCallExpression\(\n    const syntax::CallExpressionSyntax& syntaxTree\) \{.*?\n\}\n\nstd::unique_ptr<BoundExpression> Binder::bindMemberCallExpression",
    """std::unique_ptr<BoundExpression> Binder::bindSelectedCall(
    const FunctionSymbol& function,
    std::vector<std::unique_ptr<BoundExpression>> arguments,
    const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>&
        syntaxArguments,
    const std::vector<std::optional<syntax::SyntaxToken>>&
        argumentModifiers,
    std::unique_ptr<BoundExpression> receiver,
    text::TextSpan span,
    const std::string& context) {
    const auto offset = visibleParameterOffset(function);
    bool hasModifiers = false;
    for (std::size_t index = offset;
         index < function.parameters.size(); ++index) {
        hasModifiers = hasModifiers ||
            function.parameters[index].modifier !=
                ParameterModifier::None;
    }
    if (!hasModifiers) {
        auto result = std::make_unique<BoundCallExpression>();
        result->span = span;
        result->type = function.returnType;
        result->typeName = function.returnTypeName;
        result->function = function;
        if (receiver) result->arguments.push_back(std::move(receiver));
        for (std::size_t index = 0;
             index < arguments.size(); ++index) {
            const auto& parameter =
                function.parameters[index + offset];
            result->arguments.push_back(convertExpression(
                std::move(arguments[index]),
                parameter.type,
                syntaxArguments[index]->span(),
                context,
                parameter.typeName));
        }
        return result;
    }

    auto result = std::make_unique<BoundReferenceCallExpression>();
    result->span = span;
    result->type = function.returnType;
    result->typeName = function.returnTypeName;
    result->function = function;
    if (receiver) {
        BoundReferenceCallArgument receiverArgument;
        receiverArgument.value = std::move(receiver);
        result->arguments.push_back(std::move(receiverArgument));
    }

    for (std::size_t index = 0;
         index < arguments.size(); ++index) {
        const auto& parameter = function.parameters[index + offset];
        BoundReferenceCallArgument argument;
        argument.modifier = parameter.modifier;
        if (parameter.modifier == ParameterModifier::None) {
            argument.value = convertExpression(
                std::move(arguments[index]),
                parameter.type,
                syntaxArguments[index]->span(),
                context,
                parameter.typeName);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        if (index >= argumentModifiers.size() ||
            syntaxModifier(argumentModifiers[index]) !=
                parameter.modifier ||
            syntaxArguments[index]->kind() !=
                syntax::SyntaxKind::NameExpression) {
            diagnostics_.report(
                "RS8703",
                "reference argument must use the matching modifier "
                "and name a variable",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        const auto& name = static_cast<
            const syntax::NameExpressionSyntax&>(
                *syntaxArguments[index]);
        const auto* variable = lookupVariable(
            name.identifierToken.text);
        if (!variable) {
            diagnostics_.report(
                "RS8703",
                "reference argument must name a local variable or "
                "parameter",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        if (variable->modifier == ParameterModifier::In &&
            parameter.modifier != ParameterModifier::In) {
            diagnostics_.report(
                "RS8703",
                "an in parameter cannot be forwarded as ref or out",
                syntaxArguments[index]->span());
        }
        argument.variable = *variable;
        if (parameter.modifier == ParameterModifier::In) {
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        const auto wrapper = visibleTypes_.find(
            storageTypeNameOf(parameter));
        if (wrapper == visibleTypes_.end() ||
            wrapper->second.fields.empty()) {
            diagnostics_.report(
                "RS8705",
                "reference argument storage descriptor is unavailable",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        argument.wrapperType = wrapper->second;
        argument.valueField = wrapper->second.fields.front();
        if (variable->modifier == ParameterModifier::Ref ||
            variable->modifier == ParameterModifier::Out) {
            if (storageTypeNameOf(*variable) !=
                storageTypeNameOf(parameter)) {
                diagnostics_.report(
                    "RS8703",
                    "forwarded reference parameter has an "
                    "incompatible storage type",
                    syntaxArguments[index]->span());
            }
            argument.forwarded = true;
            argument.value = storageVariableExpression(
                *variable, syntaxArguments[index]->span());
        } else if (parameter.modifier == ParameterModifier::Ref) {
            argument.value = std::move(arguments[index]);
        }
        result->arguments.push_back(std::move(argument));
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindCallExpression(
    const syntax::CallExpressionSyntax& syntaxTree) {
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (const auto& argument : syntaxTree.arguments) {
        arguments.push_back(bindExpression(*argument));
    }

    std::vector<const FunctionSymbol*> candidates;
    const auto globals = visibleFunctions_.find(
        syntaxTree.identifierToken.text);
    if (globals != visibleFunctions_.end()) {
        for (const auto& function : globals->second) {
            if (!function.method) candidates.push_back(&function);
        }
    }
    if (currentOwnerType_ && !currentStaticMethod_) {
        auto methods = findMethods(
            *currentOwnerType_,
            syntaxTree.identifierToken.text,
            false);
        candidates.insert(
            candidates.end(), methods.begin(), methods.end());
    }

    bool ambiguous = false;
    const auto* best = selectBest(
        candidates,
        arguments,
        &syntaxTree.argumentModifiers,
        ambiguous);
    if (!best) {
        diagnostics_.report(
            "RS2107",
            "no applicable overload for function '" +
                syntaxTree.identifierToken.text + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2108",
            "call to function '" +
                syntaxTree.identifierToken.text +
                "' is ambiguous",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    std::unique_ptr<BoundExpression> receiver;
    if (best->method && !best->staticMethod) {
        const auto* thisVariable = lookupVariable("this");
        if (!thisVariable) return makeError(syntaxTree.span());
        receiver = variableExpression(
            *thisVariable, syntaxTree.span());
    }
    return bindSelectedCall(
        *best,
        std::move(arguments),
        syntaxTree.arguments,
        syntaxTree.argumentModifiers,
        std::move(receiver),
        syntaxTree.span(),
        "argument");
}

std::unique_ptr<BoundExpression> Binder::bindMemberCallExpression""")
replace_regex(
    "src/semantic/SemanticExpressions.cpp",
    r"std::unique_ptr<BoundExpression> Binder::bindMemberCallExpression\(\n    const syntax::MemberCallExpressionSyntax& syntaxTree\) \{.*?\n\}\n\nstd::unique_ptr<BoundExpression> Binder::bindNewObjectExpression",
    """std::unique_ptr<BoundExpression> Binder::bindMemberCallExpression(
    const syntax::MemberCallExpressionSyntax& syntaxTree) {
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (const auto& argument : syntaxTree.arguments) {
        arguments.push_back(bindExpression(*argument));
    }

    bool staticCall = false;
    TypeSymbol owner;
    std::unique_ptr<BoundExpression> receiver;
    if (syntaxTree.receiver->kind() ==
        syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<
            const syntax::NameExpressionSyntax&>(
                *syntaxTree.receiver);
        const auto type = visibleTypes_.find(
            name.identifierToken.text);
        if (!lookupVariable(name.identifierToken.text) &&
            type != visibleTypes_.end()) {
            owner = type->second;
            staticCall = true;
        }
    }
    if (!staticCall) {
        receiver = bindExpression(*syntaxTree.receiver);
        if ((receiver->type != PrimitiveType::Object &&
             receiver->type != PrimitiveType::Struct) ||
            receiver->typeName.empty()) {
            diagnostics_.report(
                "RS2471",
                "member call requires a class or struct receiver",
                syntaxTree.receiver->span());
            return makeError(syntaxTree.span());
        }
        const auto type = visibleTypes_.find(
            receiver->typeName);
        if (type == visibleTypes_.end()) {
            diagnostics_.report(
                "RS2405",
                "type descriptor '" + receiver->typeName +
                    "' is unavailable",
                syntaxTree.receiver->span());
            return makeError(syntaxTree.span());
        }
        owner = type->second;
    }

    auto methods = findMethods(
        owner, syntaxTree.nameToken.text, staticCall);
    bool ambiguous = false;
    const auto* best = selectBest(
        methods,
        arguments,
        &syntaxTree.argumentModifiers,
        ambiguous);
    if (!best) {
        diagnostics_.report(
            "RS2472",
            "no applicable method '" +
                syntaxTree.nameToken.text +
                "' on type '" + canonicalTypeName(owner) + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2473",
            "method call '" + syntaxTree.nameToken.text +
                "' is ambiguous",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    return bindSelectedCall(
        *best,
        std::move(arguments),
        syntaxTree.arguments,
        syntaxTree.argumentModifiers,
        std::move(receiver),
        syntaxTree.span(),
        "method argument");
}

std::unique_ptr<BoundExpression> Binder::bindNewObjectExpression""")

# Flow analysis for copy-in/copy-out calls.
replace_once(
    "src/semantic/FlowAnalysis.cpp",
    "        case BoundNodeKind::CallExpression:\n            for (const auto& argument : static_cast<const BoundCallExpression&>(expression).arguments)\n                analyzeExpression(*argument, assigned);\n            return;",
    "        case BoundNodeKind::CallExpression:\n            for (const auto& argument :\n                 static_cast<const BoundCallExpression&>(expression).arguments) {\n                analyzeExpression(*argument, assigned);\n            }\n            return;\n        case BoundNodeKind::ReferenceCallExpression: {\n            const auto& call = static_cast<\n                const BoundReferenceCallExpression&>(expression);\n            for (const auto& argument : call.arguments) {\n                if (argument.value &&\n                    argument.modifier != ParameterModifier::Out) {\n                    analyzeExpression(*argument.value, assigned);\n                }\n                if (!argument.forwarded &&\n                    (argument.modifier == ParameterModifier::Ref ||\n                     argument.modifier == ParameterModifier::Out)) {\n                    assigned.insert(argument.variable.index);\n                }\n            }\n            return;\n        }")

# MIR uses storage signatures internally and lowers reference calls through
# existing object/field/call instructions.
replace_once(
    "src/mir/MirLowerer.cpp",
    "    for (const auto& parameter : function.symbol.parameters) {\n        result.parameterTypes.push_back(parameter.type);\n        result.parameterTypeIds.push_back(semantic::isExactType(parameter.type)\n            ? semantic::stableTypeId(parameter.typeName) : 0);\n        result.localTypes.at(parameter.index) = parameter.type;\n        result.localTypeIds.at(parameter.index) = semantic::isExactType(parameter.type)\n            ? semantic::stableTypeId(parameter.typeName) : 0;\n    }",
    "    for (const auto& parameter : function.symbol.parameters) {\n        const auto storageType = semantic::storageTypeOf(parameter);\n        const auto& storageTypeName =\n            semantic::storageTypeNameOf(parameter);\n        result.parameterTypes.push_back(storageType);\n        result.parameterTypeIds.push_back(\n            semantic::isExactType(storageType)\n                ? semantic::stableTypeId(storageTypeName)\n                : 0);\n        result.localTypes.at(parameter.index) = storageType;\n        result.localTypeIds.at(parameter.index) =\n            semantic::isExactType(storageType)\n                ? semantic::stableTypeId(storageTypeName)\n                : 0;\n    }")
replace_once(
    "src/mir/MirLowerer.cpp",
    "        const auto value = emitValue(Opcode::Parameter, parameter.type, {}, {});",
    "        const auto storageType = semantic::storageTypeOf(parameter);\n        const auto& storageTypeName =\n            semantic::storageTypeNameOf(parameter);\n        const auto value = emitValue(\n            Opcode::Parameter, storageType, {}, {});")
replace_once(
    "src/mir/MirLowerer.cpp",
    "        instruction.resultTypeId = semantic::isExactType(parameter.type)\n            ? semantic::stableTypeId(parameter.typeName) : 0;",
    "        instruction.resultTypeId = semantic::isExactType(storageType)\n            ? semantic::stableTypeId(storageTypeName) : 0;")

replace_once(
    "src/mir/MirExpressions.cpp",
    "        for (const auto& parameter : function.parameters) {\n            instruction.parameterTypes.push_back(parameter.type);\n            instruction.parameterTypeIds.push_back(\n                semantic::isExactType(parameter.type)\n                    ? semantic::stableTypeId(parameter.typeName)\n                    : 0);\n        }",
    "        for (const auto& parameter : function.parameters) {\n            const auto storageType =\n                semantic::storageTypeOf(parameter);\n            const auto& storageTypeName =\n                semantic::storageTypeNameOf(parameter);\n            instruction.parameterTypes.push_back(storageType);\n            instruction.parameterTypeIds.push_back(\n                semantic::isExactType(storageType)\n                    ? semantic::stableTypeId(storageTypeName)\n                    : 0);\n        }")
replace_once(
    "src/mir/MirExpressions.cpp",
    "    case semantic::BoundNodeKind::CallExpression: {",
    "    case semantic::BoundNodeKind::ReferenceCallExpression: {\n        const auto& call = static_cast<const\n            semantic::BoundReferenceCallExpression&>(expression);\n        std::vector<ValueId> arguments;\n        struct Writeback {\n            semantic::VariableSymbol variable;\n            semantic::TypeSymbol wrapperType;\n            semantic::FieldSymbol field;\n            ValueId wrapper = -1;\n        };\n        std::vector<Writeback> writebacks;\n        for (const auto& argument : call.arguments) {\n            if (argument.modifier ==\n                    semantic::ParameterModifier::None ||\n                argument.modifier ==\n                    semantic::ParameterModifier::In ||\n                argument.forwarded) {\n                arguments.push_back(\n                    lowerExpression(*argument.value));\n                continue;\n            }\n\n            const auto wrapper = emitValue(\n                Opcode::NewObject,\n                semantic::PrimitiveType::Object,\n                {},\n                expression.span);\n            auto& allocation =\n                block(*currentBlockId_).instructions.back();\n            allocation.typeId = argument.wrapperType.id;\n            allocation.resultTypeId = argument.wrapperType.id;\n            allocation.symbolName =\n                semantic::canonicalTypeName(argument.wrapperType);\n            if (argument.modifier ==\n                    semantic::ParameterModifier::Ref &&\n                argument.value) {\n                const auto initial =\n                    lowerExpression(*argument.value);\n                Instruction store;\n                store.resultType =\n                    semantic::PrimitiveType::Void;\n                store.opcode = Opcode::StoreField;\n                store.operands = {wrapper, initial};\n                store.typeId = argument.wrapperType.id;\n                store.fieldIndex = argument.valueField.index;\n                store.symbolName = semantic::canonicalTypeName(\n                    argument.wrapperType);\n                store.sourceSpan = expression.span;\n                block(*currentBlockId_).instructions.push_back(\n                    std::move(store));\n            }\n            arguments.push_back(wrapper);\n            writebacks.push_back(Writeback{\n                argument.variable,\n                argument.wrapperType,\n                argument.valueField,\n                wrapper});\n        }\n\n        const auto callResult = emitCallInstruction(\n            call.function,\n            call.type,\n            call.function.returnTypeName,\n            std::move(arguments),\n            expression.span);\n        for (const auto& writeback : writebacks) {\n            const auto value = emitValue(\n                Opcode::LoadField,\n                writeback.field.type,\n                {writeback.wrapper},\n                expression.span);\n            auto& load = block(\n                *currentBlockId_).instructions.back();\n            load.typeId = writeback.wrapperType.id;\n            load.resultTypeId = semantic::isExactType(\n                    writeback.field.type)\n                ? semantic::stableTypeId(\n                    writeback.field.typeName)\n                : 0;\n            load.fieldIndex = writeback.field.index;\n            load.symbolName = semantic::canonicalTypeName(\n                writeback.wrapperType);\n            emitStoreLocal(\n                writeback.variable.index, value, expression.span);\n        }\n        return callResult;\n    }\n    case semantic::BoundNodeKind::CallExpression: {")

# Disable the legacy source expansion path.
replace_once(
    "include/realscript/compiler/LanguageExpansion.h",
    "    bool referenceParameters = true;",
    "    bool referenceParameters = false;")

# Native regression coverage.
replace_once(
    "tests/phase18_native_control_flow_tests.cpp",
    "void testNativeSequenceDiagnostics() {",
    "void testNativeReferenceParameters() {\n    const auto result = execute(R\"(\nmodule Phase18;\nvoid Bump(ref int value, out int doubled, in int amount)\n{\n    value = value + amount;\n    doubled = value + value;\n}\nint main()\n{\n    int value = 1;\n    int doubled;\n    Bump(ref value, out doubled, in 2);\n    return value + doubled;\n}\n)\");\n    require(\n        result.succeeded &&\n            std::get<std::int64_t>(result.value) == 9,\n        \"native ref/out/in execution failed\");\n}\n\nvoid testNativeReferenceDiagnostics() {\n    realscript::compiler::Compilation compilation({{\n        \"bad-reference.rs\",\n        R\"(\nmodule Phase18.ReferenceBad;\nvoid Mutate(in int value)\n{\n    value = 4;\n}\nint main()\n{\n    int value = 1;\n    Mutate(in value);\n    return value;\n}\n)\"}});\n    const auto build = compilation.build();\n    require(build.diagnostics.hasErrors(),\n        \"assignment to in parameter was accepted\");\n    bool found = false;\n    for (const auto& diagnostic : build.diagnostics.items()) {\n        found = found || diagnostic.code == \"RS8702\";\n    }\n    require(found,\n        \"assignment to in parameter did not produce RS8702\");\n}\n\nvoid testReferencesBypassExpansion() {\n    const auto expansion =\n        realscript::compiler::expandLanguageSource(\n            \"references.rs\",\n            \"module Native; void Bump(ref int value){\"\n            \"value=value+1;} int main(){int value=1;\"\n            \"Bump(ref value);return value;}\");\n    require(!expansion.changed,\n        \"native reference parameters still used source expansion\");\n}\n\nvoid testNativeSequenceDiagnostics() {")
replace_once(
    "tests/phase18_native_control_flow_tests.cpp",
    "    run(\"native sequence diagnostics\", testNativeSequenceDiagnostics);",
    "    run(\"native reference parameters\", testNativeReferenceParameters);\n    run(\"native reference diagnostics\", testNativeReferenceDiagnostics);\n    run(\"references bypass expansion\", testReferencesBypassExpansion);\n    run(\"native sequence diagnostics\", testNativeSequenceDiagnostics);")

# Documentation status.
replace_once(
    "docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md",
    "### 18F — reference modifiers and exact aliases\n\n- Native `ref`, `out`, and `in` parameter/argument symbols.\n- Definite assignment and l-value checking.\n- Remove generated wrapper classes from source-level metadata.\n- Exact-width value types are completed in Phase 23.",
    "### 18F — reference modifiers and exact aliases — in progress\n\nImplemented and validated in the native reference slice:\n\n- native `ref`, `out`, and `in` parameter and argument tokens;\n- source-level function signatures retain modifiers and underlying types;\n- compiler-owned synthetic reference boxes use existing object/field/GC/bytecode support;\n- Binder reads and writes `ref`/`out` parameters through the internal `Value` field;\n- call sites perform typed copy-in/copy-out without generated source text;\n- forwarding of compatible `ref`/`out` parameters;\n- `out` arguments become definitely assigned after a successful call;\n- assignment to `in` parameters reports `RS8702`;\n- `LanguageExpansionOptions::referenceParameters` disabled by default.\n\nStill pending in 18F:\n\n- reference member/indexer l-values and complete alias analysis;\n- native exact-width aliases;\n- complete reference semantics, nullable values, and boxing in Phase 23.")

print("native Phase 18 reference parameter migration applied")
