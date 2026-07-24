#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testSourceTextLineMap() {
    realscript::text::SourceText source("first\r\nsecond\nthird", "lines.rs");
    require(source.lineCount() == 3, "line map must recognize CRLF and LF");
    const auto position = source.linePosition(8);
    require(position.line == 1 && position.column == 1, "line/column mapping is incorrect");
}

void testLexerRecognizesCoreTokens() {
    realscript::text::SourceText source(
        "int add(int a, int b) { return a + b * 2; }",
        "lexer.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.lexAll();

    require(!diagnostics.hasErrors(), "valid token stream produced diagnostics");
    require(tokens.front().kind == realscript::syntax::SyntaxKind::IntKeyword,
            "first token must be int keyword");
    require(tokens[1].kind == realscript::syntax::SyntaxKind::IdentifierToken,
            "function name must be an identifier");
    require(tokens[tokens.size() - 2].kind == realscript::syntax::SyntaxKind::CloseBraceToken,
            "function must end with a close brace");
    require(tokens.back().kind == realscript::syntax::SyntaxKind::EndOfFileToken,
            "lexer must append EOF");
}

void testLexerReportsInvalidCharacter() {
    realscript::text::SourceText source("@", "bad.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.lexAll();
    (void)tokens;
    require(diagnostics.items().size() == 1, "invalid character must produce one diagnostic");
    require(diagnostics.items().front().code == "RS1000", "invalid character code changed");
}

void testParserBuildsCompilationUnit() {
    realscript::text::SourceText source(
        "module Demo.Core;\n"
        "import Engine.Math;\n"
        "int add(int a, int b) { return a + b; }\n",
        "parser.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();

    require(!diagnostics.hasErrors(), "valid compilation unit produced diagnostics");
    require(unit.moduleDeclaration != nullptr, "module declaration was not parsed");
    require(unit.moduleDeclaration->fullName() == "Demo.Core", "module name is incorrect");
    require(unit.imports.size() == 1 && unit.imports[0].fullName() == "Engine.Math",
            "import declaration is incorrect");
    require(unit.functions.size() == 1, "expected one function");
    require(unit.functions[0].parameters.size() == 2, "expected two parameters");
}

void testBinderAndMirLowering() {
    realscript::text::SourceText source(
        "module Demo.Core;\n"
        "int add(int a, int b) {\n"
        "    int doubled = b * 2;\n"
        "    return a + doubled;\n"
        "}\n",
        "mir.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();
    realscript::semantic::Binder binder(diagnostics);
    auto model = binder.bind(unit);

    require(!diagnostics.hasErrors(), "valid function failed semantic binding");
    require(model.functions.size() == 1, "semantic model must contain one function");
    require(model.functions[0].variableCount == 3, "parameter/local variable indexing is incorrect");

    realscript::mir::Lowerer lowerer;
    const auto text = realscript::mir::printModule(lowerer.lower(model));
    require(text.find("mul.i32") != std::string::npos, "MIR must contain integer multiply");
    require(text.find("add.i32") != std::string::npos, "MIR must contain integer add");
    require(text.find("ret %") != std::string::npos, "MIR must contain value return");
}

void testUndefinedNameDiagnostic() {
    realscript::text::SourceText source(
        "int broken(int value) { return missing + value; }",
        "undefined.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();
    realscript::semantic::Binder binder(diagnostics);
    const auto model = binder.bind(unit);
    (void)model;

    bool found = false;
    for (const auto& diagnostic : diagnostics.items()) {
        found = found || diagnostic.code == "RS2102";
    }
    require(found, "undefined name must produce RS2102");
}

void testOperatorPrecedence() {
    realscript::text::SourceText source(
        "int value() { return 1 + 2 * 3; }",
        "precedence.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();

    require(!diagnostics.hasErrors(), "precedence sample failed to parse");
    const auto& function = unit.functions.front();
    const auto& returnStatement = static_cast<const realscript::syntax::ReturnStatementSyntax&>(
        *function.body.statements.front());
    const auto& root = static_cast<const realscript::syntax::BinaryExpressionSyntax&>(
        *returnStatement.expression);
    require(root.operatorToken.kind == realscript::syntax::SyntaxKind::PlusToken,
            "addition must be the root expression");
    const auto& right = static_cast<const realscript::syntax::BinaryExpressionSyntax&>(*root.right);
    require(right.operatorToken.kind == realscript::syntax::SyntaxKind::StarToken,
            "multiplication must bind tighter than addition");
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };

    run("SourceText line map", testSourceTextLineMap);
    run("Lexer core tokens", testLexerRecognizesCoreTokens);
    run("Lexer invalid character", testLexerReportsInvalidCharacter);
    run("Parser compilation unit", testParserBuildsCompilationUnit);
    run("Binder and MIR", testBinderAndMirLowering);
    run("Undefined name diagnostic", testUndefinedNameDiagnostic);
    run("Operator precedence", testOperatorPrecedence);

    return failures == 0 ? 0 : 1;
}
