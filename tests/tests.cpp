#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot read fixture: " + path);
    }
    std::ostringstream out;
    out << stream.rdbuf();
    return out.str();
}

bool containsDiagnostic(
    const realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& code) {
    for (const auto& diagnostic : diagnostics.items()) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

std::string lowerSource(
    const std::string& sourceText,
    realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& name = "test.rs") {
    realscript::text::SourceText source(sourceText, name);
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();
    realscript::semantic::Binder binder(diagnostics);
    auto model = binder.bind(unit);
    if (diagnostics.hasErrors()) {
        return {};
    }
    realscript::mir::Lowerer lowerer;
    const auto module = lowerer.lower(model);
    (void)realscript::mir::verifyModule(module, diagnostics);
    return diagnostics.hasErrors() ? std::string{} : realscript::mir::printModule(module);
}

void testSourceTextLineMap() {
    realscript::text::SourceText source("first\r\nsecond\nthird", "lines.rs");
    require(source.lineCount() == 3, "line map must recognize CRLF and LF");
    const auto position = source.linePosition(8);
    require(position.line == 1 && position.column == 1, "line/column mapping is incorrect");
}

void testLexerRecognizesControlFlowKeywords() {
    realscript::text::SourceText source("if else while", "lexer.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.lexAll();
    require(!diagnostics.hasErrors(), "control-flow keywords produced diagnostics");
    require(tokens[0].kind == realscript::syntax::SyntaxKind::IfKeyword, "if keyword missing");
    require(tokens[1].kind == realscript::syntax::SyntaxKind::ElseKeyword, "else keyword missing");
    require(tokens[2].kind == realscript::syntax::SyntaxKind::WhileKeyword, "while keyword missing");
}

void testParserBuildsControlFlowAndAssignment() {
    realscript::text::SourceText source(
        "int choose(bool flag) { int value; if (flag) value = 1; else value = 2; while (value < 3) value = value + 1; return value; }",
        "parser.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();
    require(!diagnostics.hasErrors(), "valid control-flow syntax produced diagnostics");
    require(unit.functions.size() == 1, "expected one function");
    const auto& statements = unit.functions.front().body.statements;
    require(statements.size() == 4, "unexpected control-flow statement count");
    require(statements[1]->kind() == realscript::syntax::SyntaxKind::IfStatement,
            "if statement was not parsed");
    require(statements[2]->kind() == realscript::syntax::SyntaxKind::WhileStatement,
            "while statement was not parsed");
}

void testDefiniteAssignmentRejectsPartialBranch() {
    realscript::diagnostics::DiagnosticBag diagnostics;
    (void)lowerSource(
        "int choose(bool flag) { int value; if (flag) value = 1; return value; }",
        diagnostics,
        "partial.rs");
    require(containsDiagnostic(diagnostics, "RS2300"),
            "partially assigned local must produce RS2300");
}

void testDefiniteAssignmentAcceptsBothBranches() {
    realscript::diagnostics::DiagnosticBag diagnostics;
    const auto mir = lowerSource(
        "int choose(bool flag) { int value; if (flag) value = 1; else value = 2; return value; }",
        diagnostics,
        "complete.rs");
    require(!diagnostics.hasErrors(), "both assignment branches must satisfy definite assignment");
    require(mir.find("store.local 1") != std::string::npos, "assignment must lower to local stores");
    require(mir.find("load.local 1") != std::string::npos, "return must load the mutable local");
}

void testAllPathReturnAnalysis() {
    realscript::diagnostics::DiagnosticBag completeDiagnostics;
    (void)lowerSource(
        "int choose(bool flag) { if (flag) return 1; else return 2; }",
        completeDiagnostics,
        "returns.rs");
    require(!containsDiagnostic(completeDiagnostics, "RS2001"),
            "if/else returning on both paths must satisfy return analysis");

    realscript::diagnostics::DiagnosticBag missingDiagnostics;
    (void)lowerSource(
        "int choose(bool flag) { if (flag) return 1; }",
        missingDiagnostics,
        "missing-return.rs");
    require(containsDiagnostic(missingDiagnostics, "RS2001"),
            "reachable function end must produce RS2001");
}

void testMultiBlockMirAndVerifier() {
    realscript::diagnostics::DiagnosticBag diagnostics;
    const auto mir = lowerSource(
        "int count(int value) { while (value > 0) value = value - 1; return value; }",
        diagnostics,
        "loop.rs");
    require(!diagnostics.hasErrors(), "loop MIR must pass structural verification");
    require(mir.find("br %") != std::string::npos, "loop MIR must contain a conditional branch");
    require(mir.find("jmp bb") != std::string::npos, "loop MIR must contain jumps");
    require(mir.find("store.local") != std::string::npos, "assignment must store a local");
}

void testShortCircuitUsesBlockParameter() {
    realscript::diagnostics::DiagnosticBag diagnostics;
    const auto mir = lowerSource(
        "bool guarded(bool enabled, int value) { return enabled && value > 0; }",
        diagnostics,
        "short-circuit.rs");
    require(!diagnostics.hasErrors(), "short-circuit MIR must verify");
    require(mir.find("bb2(%") != std::string::npos, "short-circuit merge must use a block parameter");
    require(mir.find("and.bool") == std::string::npos, "short-circuit must not lower to eager and.bool");
}


void testShortCircuitAssignmentIsNotDefinite() {
    realscript::diagnostics::DiagnosticBag diagnostics;
    (void)lowerSource(
        "int guarded(bool enabled) { int value; bool used = enabled && (value = 1) > 0; return value; }",
        diagnostics,
        "short-circuit-assignment.rs");
    require(containsDiagnostic(diagnostics, "RS2300"),
            "assignment in a conditional RHS must not be definitely assigned afterward");
}

void testInfiniteLoopMakesEndpointUnreachable() {
    realscript::diagnostics::DiagnosticBag diagnostics;
    const auto mir = lowerSource(
        "int forever() { while (true) { } }",
        diagnostics,
        "forever.rs");
    require(!containsDiagnostic(diagnostics, "RS2001"),
            "literal infinite loop must make the function endpoint unreachable");
    require(!diagnostics.hasErrors(), "infinite loop MIR must verify");
    require(mir.find("jmp bb1") != std::string::npos,
            "infinite loop must lower to a cycle without a synthetic return");
}

void testVerifierRejectsMissingTarget() {
    realscript::mir::Module module;
    realscript::mir::Function function;
    function.name = "broken";
    function.returnType = realscript::semantic::PrimitiveType::Void;
    realscript::mir::BasicBlock block;
    block.id = 0;
    block.terminator.kind = realscript::mir::TerminatorKind::Jump;
    block.terminator.target = 99;
    function.blocks.push_back(block);
    module.functions.push_back(function);

    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::mir::verifyModule(module, diagnostics), "invalid MIR must fail verification");
    require(containsDiagnostic(diagnostics, "RS3009"), "missing target must produce RS3009");
}


void testVerifierHandlesMalformedOperands() {
    realscript::mir::Module module;
    realscript::mir::Function function;
    function.name = "bad-operands";
    function.returnType = realscript::semantic::PrimitiveType::Void;
    realscript::mir::BasicBlock block;
    block.id = 0;
    realscript::mir::Instruction instruction;
    instruction.result = 0;
    instruction.resultType = realscript::semantic::PrimitiveType::Int;
    instruction.opcode = realscript::mir::Opcode::AddInt;
    block.instructions.push_back(instruction);
    block.terminator.kind = realscript::mir::TerminatorKind::ReturnVoid;
    function.blocks.push_back(block);
    module.functions.push_back(function);

    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::mir::verifyModule(module, diagnostics),
            "malformed operands must fail verification without crashing");
    require(containsDiagnostic(diagnostics, "RS3012"),
            "malformed operands must produce RS3012");
}

void testControlFlowSnapshot() {
    const std::string root = REALSCRIPT_SOURCE_DIR;
    const auto source = readFile(root + "/tests/fixtures/control_flow.rs");
    const auto expected = readFile(root + "/tests/snapshots/control_flow.mir.txt");
    realscript::diagnostics::DiagnosticBag diagnostics;
    const auto actual = lowerSource(source, diagnostics, "control_flow.rs");
    require(!diagnostics.hasErrors(), "control-flow fixture failed to compile");
    require(actual == expected, "control-flow MIR snapshot changed");
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
    run("Lexer control-flow keywords", testLexerRecognizesControlFlowKeywords);
    run("Parser control flow", testParserBuildsControlFlowAndAssignment);
    run("Definite assignment partial branch", testDefiniteAssignmentRejectsPartialBranch);
    run("Definite assignment both branches", testDefiniteAssignmentAcceptsBothBranches);
    run("All-path return analysis", testAllPathReturnAnalysis);
    run("Multi-block MIR verifier", testMultiBlockMirAndVerifier);
    run("Short-circuit block parameter", testShortCircuitUsesBlockParameter);
    run("Short-circuit definite assignment", testShortCircuitAssignmentIsNotDefinite);
    run("Infinite loop endpoint", testInfiniteLoopMakesEndpointUnreachable);
    run("Verifier missing target", testVerifierRejectsMissingTarget);
    run("Verifier malformed operands", testVerifierHandlesMalformedOperands);
    run("Control-flow MIR snapshot", testControlFlowSnapshot);

    return failures == 0 ? 0 : 1;
}
