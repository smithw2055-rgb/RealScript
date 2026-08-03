#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool has(const std::vector<realscript::syntax::SyntaxToken>& values,
         realscript::syntax::SyntaxKind kind) {
    return std::any_of(values.begin(), values.end(),
        [&](const auto& value) { return value.kind == kind; });
}
void testSyntax() {
    const char* source = R"(
module Phase19.Syntax;
public interface IEntity { public int Kind(); }
public abstract class Unit : IEntity
{
    protected int health;
    protected Unit(int initial) : base() { health = initial; }
    public virtual int Power() { return health; }
    public abstract int Kind();
}
internal sealed class Marine : Unit, IEntity
{
    public Marine(int initial) : base(initial) { }
    public sealed override int Power() { return base.Power() + 2; }
    public override int Kind() { return 1; }
}
)";
    realscript::text::SourceText text(source, "phase19.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(text, diagnostics);
    auto unit = parser.parseCompilationUnit();
    require(!diagnostics.hasErrors(), "Phase 19 parser produced diagnostics");
    require(unit.classes.size() == 2 && unit.interfaces.size() == 1,
        "Phase 19 declarations were not parsed");
    const auto& base = unit.classes.front();
    require(has(base.modifiers, realscript::syntax::SyntaxKind::PublicKeyword) &&
            has(base.modifiers, realscript::syntax::SyntaxKind::AbstractKeyword),
        "class modifiers were lost");
    require(base.interfaces.size() == 1 &&
            base.fields.size() == 1 &&
            has(base.fields.front().modifiers,
                realscript::syntax::SyntaxKind::ProtectedKeyword),
        "base list or field modifiers were lost");
    require(base.constructors.front().baseKeyword.has_value() &&
            base.methods.back().semicolonToken.has_value() &&
            has(base.methods.back().modifiers,
                realscript::syntax::SyntaxKind::AbstractKeyword),
        "base initializer or abstract declaration was lost");
    const auto& derived = unit.classes.back();
    require(derived.interfaces.size() == 2 &&
            derived.constructors.front().baseArguments.size() == 1 &&
            has(derived.methods.front().modifiers,
                realscript::syntax::SyntaxKind::OverrideKeyword),
        "derived syntax was not retained");
    const auto& returned = static_cast<const realscript::syntax::ReturnStatementSyntax&>(
        *derived.methods.front().body.statements.front());
    const auto& binary = static_cast<const realscript::syntax::BinaryExpressionSyntax&>(
        *returned.expression);
    const auto& call = static_cast<const realscript::syntax::MemberCallExpressionSyntax&>(
        *binary.left);
    require(call.receiver->kind() == realscript::syntax::SyntaxKind::BaseExpression,
        "base expression did not survive parsing");
}
void testDuplicateModifier() {
    realscript::text::SourceText text(
        "public public class Bad {}", "bad.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(text, diagnostics);
    (void)parser.parseCompilationUnit();
    require(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
        [](const auto& value) { return value.code == "RS1116"; }),
        "duplicate modifier did not report RS1116");
}
}
int main() {
    try {
        testSyntax();
        testDuplicateModifier();
        std::cout << "Phase 19 frontend tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
