#include "realscript/tooling/GovernanceLinter.h"

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realscript::tooling {
namespace {

std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

std::string toUpperAscii(std::string_view input) {
    std::string out(input);
    for (char& ch : out) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return out;
}

debug::SourceFileInfo buildSourceFileInfo(const std::string& path, const std::string& content) {
    debug::SourceFileInfo info;
    info.id = 0;
    info.path = path;
    info.lineStarts.push_back(0);
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            info.lineStarts.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
    return info;
}

struct SuppressionMap {
    // line0 -> set of canonical rule codes ("*" means all RSG rules)
    std::unordered_map<std::size_t, std::unordered_set<std::string>> byLine;

    [[nodiscard]] bool isSuppressed(std::size_t line0, const std::string& ruleCode) const {
        const auto it = byLine.find(line0);
        if (it == byLine.end()) return false;
        return it->second.find("*") != it->second.end() ||
            it->second.find(ruleCode) != it->second.end();
    }
};

SuppressionMap parseInlineSuppressions(const std::string& content) {
    SuppressionMap map;
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || content[i] == '\n') {
            std::string_view lv(content.data() + start, i - start);
            if (!lv.empty() && lv.back() == '\r') lv.remove_suffix(1);
            lines.push_back(lv);
            start = i + 1;
        }
    }

    const auto isAttributeOnlyLine = [](std::string_view line) noexcept {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        return line.size() >= 2 && line.front() == '[' && line.back() == ']';
    };

    const auto parseDirectiveRules = [](std::string_view tail) -> std::unordered_set<std::string> {
        // Strip optional explanation after `--` or `//`
        if (const auto dashPos = tail.find("--"); dashPos != std::string_view::npos) {
            tail = tail.substr(0, dashPos);
        }
        if (const auto slashPos = tail.find("//"); slashPos != std::string_view::npos) {
            tail = tail.substr(0, slashPos);
        }

        std::unordered_set<std::string> codes;
        std::size_t pos = 0;
        while (pos < tail.size()) {
            while (pos < tail.size() &&
                   (tail[pos] == ' ' || tail[pos] == '\t' || tail[pos] == ',')) {
                ++pos;
            }
            if (pos >= tail.size()) break;
            std::size_t end = pos;
            while (end < tail.size() &&
                   tail[end] != ' ' && tail[end] != '\t' && tail[end] != ',') {
                ++end;
            }
            const std::string_view rawTok = tail.substr(pos, end - pos);
            const std::string lower = toLowerAscii(rawTok);
            if (rawTok == "*" || lower == "all") {
                codes.insert("*");
            } else {
                const std::string canon = canonicalLintRuleCode(rawTok);
                if (!canon.empty()) {
                    codes.insert(canon);
                }
            }
            pos = end;
        }
        if (codes.empty()) {
            codes.insert("*");
        }
        return codes;
    };

    for (std::size_t lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        const std::string_view lv = lines[lineIdx];
        // Find `//` outside string literals on this line
        bool inQuote = false;
        std::size_t commentPos = std::string_view::npos;
        for (std::size_t i = 0; i < lv.size(); ++i) {
            if (lv[i] == '"' && (i == 0 || lv[i - 1] != '\\')) {
                inQuote = !inQuote;
            } else if (!inQuote && i + 1 < lv.size() && lv[i] == '/' && lv[i + 1] == '/') {
                commentPos = i + 2;
                break;
            }
        }
        if (commentPos == std::string_view::npos) continue;

        std::string_view body = lv.substr(commentPos);
        while (!body.empty() && (body.front() == ' ' || body.front() == '\t')) {
            body.remove_prefix(1);
        }

        constexpr std::string_view kDisableNext = "realscript-disable-next-line";
        constexpr std::string_view kDisableSame = "realscript-disable-line";

        if (body.rfind(kDisableNext, 0) == 0 &&
            (body.size() == kDisableNext.size() ||
             body[kDisableNext.size()] == ' ' ||
             body[kDisableNext.size()] == '\t' ||
             body[kDisableNext.size()] == ',')) {
            const auto rules = parseDirectiveRules(body.substr(kDisableNext.size()));
            std::size_t target = lineIdx + 1;
            if (target < lines.size()) {
                map.byLine[target].insert(rules.begin(), rules.end());
                // Also propagate across contiguous `[Attribute]` lines immediately below the directive
                while (target < lines.size() && isAttributeOnlyLine(lines[target])) {
                    ++target;
                    if (target < lines.size()) {
                        map.byLine[target].insert(rules.begin(), rules.end());
                    }
                }
            }
        } else if (body.rfind(kDisableSame, 0) == 0 &&
                   (body.size() == kDisableSame.size() ||
                    body[kDisableSame.size()] == ' ' ||
                    body[kDisableSame.size()] == '\t' ||
                    body[kDisableSame.size()] == ',')) {
            const auto rules = parseDirectiveRules(body.substr(kDisableSame.size()));
            map.byLine[lineIdx].insert(rules.begin(), rules.end());
        }
    }
    return map;
}

bool isHotLifecycleMethodName(std::string_view name) noexcept {
    return name == "OnUpdate" ||
        name == "OnFixedUpdate" ||
        name == "OnLateUpdate" ||
        name == "Update" ||
        name == "FixedUpdate";
}

std::string stripModulePrefix(std::string_view name) {
    const auto pos = name.rfind("::");
    if (pos != std::string_view::npos) {
        return std::string(name.substr(pos + 2));
    }
    return std::string(name);
}

std::string renderTypeSyntaxSimple(const syntax::TypeSyntax& type) {
    if (type.isNullable() && !type.typeArguments.empty()) {
        return renderTypeSyntaxSimple(type.typeArguments.front()) + "?";
    }
    std::string base = stripModulePrefix(type.name.text);
    if (type.isArray()) {
        base += "[]";
    }
    return base;
}

bool isFloatOrDoubleTypeName(std::string_view typeName) noexcept {
    return typeName == "float" || typeName == "double" ||
        typeName == "float?" || typeName == "double?" ||
        typeName == "Nullable<float>" || typeName == "Nullable<double>";
}

// ============================================================================
// RSG002: Cyclomatic Complexity Counter
// ============================================================================

std::size_t countExpressionComplexity(const syntax::ExpressionSyntax* expr) {
    if (!expr) return 0;
    std::size_t count = 0;
    switch (expr->kind()) {
    case syntax::SyntaxKind::UnaryExpression: {
        const auto& u = static_cast<const syntax::UnaryExpressionSyntax&>(*expr);
        count += countExpressionComplexity(u.operand.get());
        break;
    }
    case syntax::SyntaxKind::BinaryExpression: {
        const auto& b = static_cast<const syntax::BinaryExpressionSyntax&>(*expr);
        if (b.operatorToken.kind == syntax::SyntaxKind::AmpersandAmpersandToken ||
            b.operatorToken.kind == syntax::SyntaxKind::PipePipeToken ||
            b.operatorToken.kind == syntax::SyntaxKind::QuestionQuestionToken) {
            ++count;
        }
        count += countExpressionComplexity(b.left.get());
        count += countExpressionComplexity(b.right.get());
        break;
    }
    case syntax::SyntaxKind::ConditionalExpression: {
        const auto& c = static_cast<const syntax::ConditionalExpressionSyntax&>(*expr);
        ++count;
        count += countExpressionComplexity(c.condition.get());
        count += countExpressionComplexity(c.whenTrue.get());
        count += countExpressionComplexity(c.whenFalse.get());
        break;
    }
    case syntax::SyntaxKind::SwitchExpression: {
        const auto& sw = static_cast<const syntax::SwitchExpressionSyntax&>(*expr);
        count += countExpressionComplexity(sw.expression.get());
        for (const auto& arm : sw.arms) {
            if (!arm.discardToken.has_value()) {
                ++count;
            }
            count += countExpressionComplexity(arm.label.get());
            count += countExpressionComplexity(arm.guard.get());
            count += countExpressionComplexity(arm.value.get());
        }
        break;
    }
    case syntax::SyntaxKind::LambdaExpression: {
        const auto& lam = static_cast<const syntax::LambdaExpressionSyntax&>(*expr);
        count += countExpressionComplexity(lam.body.get());
        break;
    }
    case syntax::SyntaxKind::AssignmentExpression: {
        const auto& a = static_cast<const syntax::AssignmentExpressionSyntax&>(*expr);
        count += countExpressionComplexity(a.expression.get());
        break;
    }
    case syntax::SyntaxKind::MemberAssignmentExpression: {
        const auto& a = static_cast<const syntax::MemberAssignmentExpressionSyntax&>(*expr);
        count += countExpressionComplexity(a.receiver.get());
        count += countExpressionComplexity(a.expression.get());
        break;
    }
    case syntax::SyntaxKind::ElementAssignmentExpression: {
        const auto& a = static_cast<const syntax::ElementAssignmentExpressionSyntax&>(*expr);
        count += countExpressionComplexity(a.receiver.get());
        count += countExpressionComplexity(a.index.get());
        count += countExpressionComplexity(a.expression.get());
        break;
    }
    case syntax::SyntaxKind::ParenthesizedExpression: {
        const auto& p = static_cast<const syntax::ParenthesizedExpressionSyntax&>(*expr);
        count += countExpressionComplexity(p.expression.get());
        break;
    }
    case syntax::SyntaxKind::CastExpression: {
        const auto& c = static_cast<const syntax::CastExpressionSyntax&>(*expr);
        count += countExpressionComplexity(c.expression.get());
        break;
    }
    case syntax::SyntaxKind::TypeBinaryExpression: {
        const auto& tb = static_cast<const syntax::TypeBinaryExpressionSyntax&>(*expr);
        count += countExpressionComplexity(tb.expression.get());
        break;
    }
    case syntax::SyntaxKind::CallExpression: {
        const auto& c = static_cast<const syntax::CallExpressionSyntax&>(*expr);
        for (const auto& arg : c.arguments) count += countExpressionComplexity(arg.get());
        break;
    }
    case syntax::SyntaxKind::MemberCallExpression: {
        const auto& c = static_cast<const syntax::MemberCallExpressionSyntax&>(*expr);
        count += countExpressionComplexity(c.receiver.get());
        for (const auto& arg : c.arguments) count += countExpressionComplexity(arg.get());
        break;
    }
    case syntax::SyntaxKind::MemberAccessExpression: {
        const auto& m = static_cast<const syntax::MemberAccessExpressionSyntax&>(*expr);
        count += countExpressionComplexity(m.receiver.get());
        break;
    }
    case syntax::SyntaxKind::ElementAccessExpression: {
        const auto& e = static_cast<const syntax::ElementAccessExpressionSyntax&>(*expr);
        count += countExpressionComplexity(e.receiver.get());
        count += countExpressionComplexity(e.index.get());
        break;
    }
    case syntax::SyntaxKind::NewObjectExpression: {
        const auto& n = static_cast<const syntax::NewObjectExpressionSyntax&>(*expr);
        for (const auto& arg : n.arguments) count += countExpressionComplexity(arg.get());
        for (const auto& init : n.initializers) {
            for (const auto& ie : init.expressions) count += countExpressionComplexity(ie.get());
        }
        break;
    }
    case syntax::SyntaxKind::NewArrayExpression: {
        const auto& n = static_cast<const syntax::NewArrayExpressionSyntax&>(*expr);
        if (n.explicitLength) count += countExpressionComplexity(n.length.get());
        for (const auto& iv : n.initialValues) count += countExpressionComplexity(iv.get());
        break;
    }
    default:
        break;
    }
    return count;
}

std::size_t countStatementComplexity(const syntax::StatementSyntax* stmt) {
    if (!stmt) return 0;
    std::size_t count = 0;
    switch (stmt->kind()) {
    case syntax::SyntaxKind::BlockStatement: {
        const auto& b = static_cast<const syntax::BlockStatementSyntax&>(*stmt);
        for (const auto& s : b.statements) count += countStatementComplexity(s.get());
        break;
    }
    case syntax::SyntaxKind::IfStatement: {
        const auto& ifs = static_cast<const syntax::IfStatementSyntax&>(*stmt);
        ++count;
        count += countExpressionComplexity(ifs.condition.get());
        count += countStatementComplexity(ifs.thenStatement.get());
        count += countStatementComplexity(ifs.elseStatement.get());
        break;
    }
    case syntax::SyntaxKind::WhileStatement: {
        const auto& ws = static_cast<const syntax::WhileStatementSyntax&>(*stmt);
        ++count;
        count += countExpressionComplexity(ws.condition.get());
        count += countStatementComplexity(ws.body.get());
        break;
    }
    case syntax::SyntaxKind::ForStatement: {
        const auto& fs = static_cast<const syntax::ForStatementSyntax&>(*stmt);
        ++count;
        count += countStatementComplexity(fs.initializer.get());
        count += countExpressionComplexity(fs.condition.get());
        count += countExpressionComplexity(fs.increment.get());
        count += countStatementComplexity(fs.body.get());
        break;
    }
    case syntax::SyntaxKind::ForeachStatement: {
        const auto& fe = static_cast<const syntax::ForeachStatementSyntax&>(*stmt);
        ++count;
        count += countExpressionComplexity(fe.collection.get());
        count += countStatementComplexity(fe.body.get());
        break;
    }
    case syntax::SyntaxKind::DoWhileStatement: {
        const auto& dw = static_cast<const syntax::DoWhileStatementSyntax&>(*stmt);
        ++count;
        count += countStatementComplexity(dw.body.get());
        count += countExpressionComplexity(dw.condition.get());
        break;
    }
    case syntax::SyntaxKind::SwitchStatement: {
        const auto& sw = static_cast<const syntax::SwitchStatementSyntax&>(*stmt);
        count += countExpressionComplexity(sw.expression.get());
        for (const auto& sec : sw.sections) {
            if (sec.caseKeyword.has_value()) {
                ++count;
            }
            count += countExpressionComplexity(sec.label.get());
            count += countExpressionComplexity(sec.guard.get());
            for (const auto& s : sec.statements) count += countStatementComplexity(s.get());
        }
        break;
    }
    case syntax::SyntaxKind::TryStatement: {
        const auto& tr = static_cast<const syntax::TryStatementSyntax&>(*stmt);
        count += countStatementComplexity(&tr.body);
        for (const auto& c : tr.catches) {
            ++count;
            count += countStatementComplexity(&c.body);
        }
        if (tr.finallyBody &&
            !(tr.finallyKeyword && tr.finallyKeyword->text == "$using_finally")) {
            count += countStatementComplexity(&(*tr.finallyBody));
        }
        break;
    }
    case syntax::SyntaxKind::ReturnStatement: {
        const auto& r = static_cast<const syntax::ReturnStatementSyntax&>(*stmt);
        count += countExpressionComplexity(r.expression.get());
        break;
    }
    case syntax::SyntaxKind::ThrowStatement: {
        const auto& t = static_cast<const syntax::ThrowStatementSyntax&>(*stmt);
        count += countExpressionComplexity(t.expression.get());
        break;
    }
    case syntax::SyntaxKind::YieldWaitStatement: {
        const auto& y = static_cast<const syntax::YieldWaitStatementSyntax&>(*stmt);
        count += countExpressionComplexity(y.delay.get());
        count += countExpressionComplexity(y.secondArgument.get());
        break;
    }
    case syntax::SyntaxKind::EventSubscriptionStatement: {
        const auto& ev = static_cast<const syntax::EventSubscriptionStatementSyntax&>(*stmt);
        count += countExpressionComplexity(ev.receiver.get());
        count += countExpressionComplexity(ev.handler.get());
        break;
    }
    case syntax::SyntaxKind::VariableDeclarationStatement: {
        const auto& vd = static_cast<const syntax::VariableDeclarationStatementSyntax&>(*stmt);
        count += countExpressionComplexity(vd.initializer.get());
        break;
    }
    case syntax::SyntaxKind::ExpressionStatement: {
        const auto& es = static_cast<const syntax::ExpressionStatementSyntax&>(*stmt);
        count += countExpressionComplexity(es.expression.get());
        break;
    }
    default:
        break;
    }
    return count;
}

// ============================================================================
// RSG005 Helpers: Token-Canonical Statement Equivalence & Empty Catch Check
// ============================================================================

bool statementsAreTokenEquivalent(
    const text::SourceText& source,
    const syntax::StatementSyntax& left,
    const syntax::StatementSyntax& right) {
    const auto extractCanonicalTokens = [&](const syntax::StatementSyntax& stmt)
        -> std::vector<std::pair<syntax::SyntaxKind, std::string>> {
        const syntax::StatementSyntax* target = &stmt;
        while (target->kind() == syntax::SyntaxKind::BlockStatement) {
            const auto& blk = static_cast<const syntax::BlockStatementSyntax&>(*target);
            if (blk.statements.size() == 1 &&
                blk.statements.front() != nullptr &&
                blk.statements.front()->kind() != syntax::SyntaxKind::VariableDeclarationStatement) {
                target = blk.statements.front().get();
            } else {
                break;
            }
        }
        const auto sp = target->span();
        if (sp.empty() || sp.end() > source.size()) return {};
        text::SourceText sub(std::string(source.view(sp)), "<branch>");
        diagnostics::DiagnosticBag bag;
        syntax::Lexer lex(sub, bag);
        if (bag.hasErrors()) return {};
        std::vector<std::pair<syntax::SyntaxKind, std::string>> out;
        for (const auto& t : lex.lexAll()) {
            if (t.kind == syntax::SyntaxKind::EndOfFileToken) break;
            out.emplace_back(t.kind, t.text);
        }
        return out;
    };

    const auto lhsToks = extractCanonicalTokens(left);
    const auto rhsToks = extractCanonicalTokens(right);
    return !lhsToks.empty() && lhsToks == rhsToks;
}

bool catchBlockHasComment(
    const std::string& content,
    const syntax::BlockStatementSyntax& block) noexcept {
    if (block.openBraceToken.missing || block.closeBraceToken.missing) {
        return true;
    }
    const std::size_t start = block.openBraceToken.span.end();
    const std::size_t end = block.closeBraceToken.span.start;
    if (end <= start || end > content.size()) return false;
    const std::string_view inner(content.data() + start, end - start);
    return inner.find("//") != std::string_view::npos ||
        inner.find("/*") != std::string_view::npos;
}

// ============================================================================
// Workspace / Build Symbol Knowledge Base
// ============================================================================

struct WorkspaceKnowledge {
    std::unordered_set<std::string> structNames;
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleSymbols;
    std::unordered_map<std::string, std::string> functionReturnTypes; // name -> returnType
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> typeMemberTypes; // Type -> (member -> type)
    std::unordered_map<semantic::SymbolId, std::string> symbolDefModule;

    WorkspaceKnowledge() {
        for (const char* builtinStruct : {
                 "Fixed", "Fixed16", "Vec2", "Vec3", "Quat",
                 "Color", "Rect", "Transform2D", "Transform3D",
                 "ResourceHandle", "AssetGuid", "Nullable"}) {
            structNames.insert(builtinStruct);
        }
        moduleSymbols["RealScript.Math"] = {
            "Fixed", "Fixed16", "Vec2", "Vec3", "Quat", "Math", "DeterministicMath"
        };
        moduleSymbols["RealScript.Fiber"] = {
            "Fiber", "FiberHandle", "FiberScheduler", "Parallel"
        };
        moduleSymbols["RealScript.Async"] = {
            "Task", "CancellationToken", "Async"
        };
        moduleSymbols["RealScript.Resource"] = {
            "Resource", "ResourceHandle", "AssetGuid"
        };
    }

    void ingestCompilationUnit(const syntax::CompilationUnitSyntax& unit) {
        const std::string modName = unit.moduleDeclaration
            ? unit.moduleDeclaration->fullName()
            : std::string{"$global"};

        for (const auto& s : unit.structs) {
            structNames.insert(s.identifierToken.text);
            moduleSymbols[modName].insert(s.identifierToken.text);
            auto& memMap = typeMemberTypes[s.identifierToken.text];
            for (const auto& f : s.fields) {
                memMap[f.identifierToken.text] = renderTypeSyntaxSimple(f.type);
            }
            for (const auto& p : s.properties) {
                memMap[p.identifierToken.text] = renderTypeSyntaxSimple(p.type);
            }
            for (const auto& m : s.methods) {
                memMap[m.identifierToken.text] = renderTypeSyntaxSimple(m.returnType);
            }
        }
        for (const auto& c : unit.classes) {
            moduleSymbols[modName].insert(c.identifierToken.text);
            auto& memMap = typeMemberTypes[c.identifierToken.text];
            for (const auto& f : c.fields) {
                memMap[f.identifierToken.text] = renderTypeSyntaxSimple(f.type);
            }
            for (const auto& p : c.properties) {
                memMap[p.identifierToken.text] = renderTypeSyntaxSimple(p.type);
            }
            for (const auto& m : c.methods) {
                memMap[m.identifierToken.text] = renderTypeSyntaxSimple(m.returnType);
                if (semantic::hasModifier(m.modifiers, syntax::SyntaxKind::StaticKeyword) &&
                    !m.parameters.empty() &&
                    m.parameters.front().modifierToken &&
                    m.parameters.front().modifierToken->kind == syntax::SyntaxKind::ThisKeyword) {
                    // Extension method exported by `modName`!
                    moduleSymbols[modName].insert(m.identifierToken.text);
                }
            }
        }
        for (const auto& e : unit.enums) {
            moduleSymbols[modName].insert(e.identifierToken.text);
        }
        for (const auto& iface : unit.interfaces) {
            moduleSymbols[modName].insert(iface.identifierToken.text);
        }
        for (const auto& del : unit.delegates) {
            moduleSymbols[modName].insert(del.identifierToken.text);
        }
        for (const auto& fn : unit.functions) {
            moduleSymbols[modName].insert(fn.identifierToken.text);
            functionReturnTypes[fn.identifierToken.text] = renderTypeSyntaxSimple(fn.returnType);
        }
    }

    void ingestMirModule(const mir::Module& mod) {
        auto& exports = moduleSymbols[mod.name];
        for (const auto& t : mod.types) {
            if (t.kind == semantic::TypeKind::Struct) {
                structNames.insert(t.name);
                structNames.insert(semantic::canonicalTypeName(t));
            }
            if (!t.synthetic) {
                exports.insert(t.name);
                const auto dunder = t.name.find("__");
                if (dunder != std::string::npos) {
                    exports.insert(t.name.substr(0, dunder));
                }
                if (t.id != 0) symbolDefModule[t.id] = mod.name;
            }
            auto& memMap = typeMemberTypes[t.name];
            for (const auto& f : t.fields) {
                if (f.synthetic) continue;
                memMap[f.name] = f.typeName.empty()
                    ? std::string(semantic::primitiveTypeName(f.type))
                    : stripModulePrefix(f.typeName);
                if (f.id != 0) symbolDefModule[f.id] = mod.name;
            }
            for (const auto& p : t.properties) {
                memMap[p.name] = p.typeName.empty()
                    ? std::string(semantic::primitiveTypeName(p.type))
                    : stripModulePrefix(p.typeName);
                if (p.id != 0) symbolDefModule[p.id] = mod.name;
            }
            for (const auto& m : t.methods) {
                if (m.synthetic) continue;
                memMap[m.name] = m.returnTypeName.empty()
                    ? std::string(semantic::primitiveTypeName(m.returnType))
                    : stripModulePrefix(m.returnTypeName);
                if (m.extensionMethod) exports.insert(m.name);
                if (m.id != 0) symbolDefModule[m.id] = mod.name;
            }
        }
        for (const auto& iface : mod.languageMetadata.interfaces) {
            const std::string shortType = stripModulePrefix(iface.typeName);
            exports.insert(shortType);
        }
        for (const auto& fn : mod.functions) {
            if (fn.name.find('$') == std::string::npos) {
                exports.insert(fn.name);
                const auto dunder = fn.name.find("__");
                if (dunder != std::string::npos) {
                    exports.insert(fn.name.substr(0, dunder));
                }
                functionReturnTypes[fn.name] = std::string(semantic::primitiveTypeName(fn.returnType));
                if (fn.symbolId != 0) symbolDefModule[fn.symbolId] = mod.name;
            }
        }
    }

    void ingestBuildResult(const compiler::BuildResult& build) {
        for (const auto& m : build.modules) {
            ingestMirModule(m);
        }
        for (const auto& [modName, cached] : build.snapshot.modules) {
            (void)modName;
            ingestMirModule(cached.module);
        }
    }
};

} // namespace

// ============================================================================
// Public Helpers & Config Parsing
// ============================================================================

const char* lintSeverityName(LintSeverity severity) noexcept {
    switch (severity) {
    case LintSeverity::Off: return "off";
    case LintSeverity::Info: return "info";
    case LintSeverity::Warning: return "warning";
    case LintSeverity::Error: return "error";
    }
    return "warning";
}

std::optional<LintSeverity> parseLintSeverity(std::string_view text) noexcept {
    const std::string lower = toLowerAscii(text);
    if (lower == "off" || lower == "none" || lower == "false" || lower == "disable" || lower == "disabled") {
        return LintSeverity::Off;
    }
    if (lower == "info" || lower == "information" || lower == "hint") {
        return LintSeverity::Info;
    }
    if (lower == "warn" || lower == "warning") {
        return LintSeverity::Warning;
    }
    if (lower == "err" || lower == "error") {
        return LintSeverity::Error;
    }
    return std::nullopt;
}

std::string canonicalLintRuleCode(std::string_view codeOrSlug) {
    const std::string upper = toUpperAscii(codeOrSlug);
    if (upper == "RSG001" || upper == "RSG002" || upper == "RSG003" ||
        upper == "RSG004" || upper == "RSG005") {
        return upper;
    }
    const std::string lower = toLowerAscii(codeOrSlug);
    if (lower == "hot-path-allocation") return "RSG001";
    if (lower == "cyclomatic-complexity") return "RSG002";
    if (lower == "unused-symbol") return "RSG003";
    if (lower == "sequence-float-equality") return "RSG004";
    if (lower == "suspicious-control-flow") return "RSG005";
    return {};
}

const char* lintRuleSlug(std::string_view ruleCode) noexcept {
    if (ruleCode == "RSG001") return "hot-path-allocation";
    if (ruleCode == "RSG002") return "cyclomatic-complexity";
    if (ruleCode == "RSG003") return "unused-symbol";
    if (ruleCode == "RSG004") return "sequence-float-equality";
    if (ruleCode == "RSG005") return "suspicious-control-flow";
    return "governance";
}

LintSeverity LintConfig::severityFor(std::string_view ruleCode) const noexcept {
    const std::string canon = canonicalLintRuleCode(ruleCode);
    if (canon.empty()) return LintSeverity::Off;
    const auto it = ruleSeverities.find(canon);
    if (it != ruleSeverities.end()) return it->second;
    return LintSeverity::Warning;
}

void LintConfig::setRuleSeverity(std::string_view codeOrSlug, LintSeverity severity) {
    const std::string canon = canonicalLintRuleCode(codeOrSlug);
    if (!canon.empty()) {
        ruleSeverities[canon] = severity;
    }
}

bool parseLintConfigFromProjectJson(
    const Json& rootObject,
    LintConfig& ioConfig,
    bool merge) {
    if (!rootObject.isObject()) return false;
    bool touched = false;

    if (!merge) {
        ioConfig = LintConfig{};
    }

    if (const auto* enableTop = rootObject.find("enableGovernanceLint");
        enableTop && enableTop->isBool()) {
        ioConfig.enableGovernanceLint = enableTop->boolValue(false);
        touched = true;
    }

    if (const auto* lintObj = rootObject.find("lint");
        lintObj && lintObj->isObject()) {
        touched = true;
        if (const auto* en = lintObj->find("enableGovernanceLint"); en && en->isBool()) {
            ioConfig.enableGovernanceLint = en->boolValue(ioConfig.enableGovernanceLint);
        } else if (const auto* en2 = lintObj->find("enabled"); en2 && en2->isBool()) {
            ioConfig.enableGovernanceLint = en2->boolValue(ioConfig.enableGovernanceLint);
        }

        if (const auto* maxCc = lintObj->find("maxCyclomaticComplexity");
            maxCc && maxCc->isNumber()) {
            const std::int64_t val = maxCc->integerValue(20);
            ioConfig.maxCyclomaticComplexity = static_cast<std::size_t>(std::max<std::int64_t>(1, val));
        }

        if (const auto* rulesObj = lintObj->find("rules");
            rulesObj && rulesObj->isObject()) {
            for (const auto& [key, val] : rulesObj->objectValue()) {
                if (!val.isString()) continue;
                if (const auto sev = parseLintSeverity(val.stringValue())) {
                    ioConfig.setRuleSeverity(key, *sev);
                }
            }
        }
    }

    return touched;
}

// ============================================================================
// GovernanceLinter Core Analysis
// ============================================================================

GovernanceLinter::GovernanceLinter(LintConfig config)
    : config_(std::move(config)) {}

std::vector<LintDiagnostic> GovernanceLinter::lintSource(
    const std::string& path,
    const std::string& content,
    const compiler::BuildResult* buildContext,
    const std::vector<compiler::SourceFile>* workspaceSources) const {
    // Skip host declaration stub files (.d.rscript)
    if (path.size() >= 10 && toLowerAscii(std::string_view(path).substr(path.size() - 10)) == ".d.rscript") {
        return {};
    }

    const debug::SourceFileInfo fileInfo = buildSourceFileInfo(path, content);
    const SuppressionMap suppressions = parseInlineSuppressions(content);

    text::SourceText source(content, path);
    diagnostics::DiagnosticBag parseBag;
    syntax::Parser parser(source, parseBag);
    const syntax::CompilationUnitSyntax unit = parser.parseCompilationUnit();

    WorkspaceKnowledge kb;
    kb.ingestCompilationUnit(unit);
    if (workspaceSources) {
        for (const auto& ws : *workspaceSources) {
            if (ws.path == path) continue;
            text::SourceText wsSrc(ws.content, ws.path);
            diagnostics::DiagnosticBag wsBag;
            syntax::Parser wsParser(wsSrc, wsBag);
            kb.ingestCompilationUnit(wsParser.parseCompilationUnit());
        }
    }
    if (buildContext) {
        kb.ingestBuildResult(*buildContext);
    }

    std::vector<LintDiagnostic> emitted;
    const auto report = [&](const char* ruleCode, text::TextSpan span, std::string message) {
        const LintSeverity sev = config_.severityFor(ruleCode);
        if (sev == LintSeverity::Off) return;
        const debug::SourceRange rng = debug::makeSourceRange(fileInfo, span);
        if (suppressions.isSuppressed(rng.start.line, ruleCode)) {
            return;
        }
        LintDiagnostic d;
        d.code = ruleCode;
        d.ruleName = lintRuleSlug(ruleCode);
        d.message = std::move(message);
        d.severity = sev;
        d.sourcePath = path;
        d.span = span;
        d.range = rng;
        emitted.push_back(std::move(d));
    };

    // ------------------------------------------------------------------------
    // Scoped Body Analyzer (handles RSG001, RSG002, RSG003 locals/fields, RSG004, RSG005)
    // ------------------------------------------------------------------------
    struct LocalBinding {
        std::string name;
        std::string typeName;
        text::TextSpan span;
        bool used = false;
        bool skipUnusedReport = false;
    };

    struct CallableContext {
        std::string ownerTypeName;
        std::unordered_map<std::string, std::string> ownerMemberTypes;
        std::unordered_set<std::string>* readPrivateFields = nullptr;
        bool isHotPathLifecycle = false;
        bool isSequenceCoroutine = false;
        std::unordered_set<std::string> parameters;
        std::unordered_map<std::string, std::string> parameterTypes;
        std::vector<std::vector<LocalBinding>> scopes;

        void pushScope() { scopes.emplace_back(); }

        void popScopeAndReportUnused(const std::function<void(const LocalBinding&)>& onUnused) {
            if (scopes.empty()) return;
            for (const auto& loc : scopes.back()) {
                if (!loc.skipUnusedReport && !loc.used) {
                    onUnused(loc);
                }
            }
            scopes.pop_back();
        }

        LocalBinding* findLocal(const std::string& name) {
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                for (auto bit = it->rbegin(); bit != it->rend(); ++bit) {
                    if (bit->name == name) return &(*bit);
                }
            }
            return nullptr;
        }

        const LocalBinding* findLocalConst(const std::string& name) const {
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                for (auto bit = it->rbegin(); bit != it->rend(); ++bit) {
                    if (bit->name == name) return &(*bit);
                }
            }
            return nullptr;
        }

        bool isShadowedByParamOrLocal(const std::string& name) const {
            return findLocalConst(name) != nullptr ||
                parameters.find(name) != parameters.end();
        }
    };

    const auto inferExpressionType = [&](
        auto& self,
        const syntax::ExpressionSyntax* expr,
        const CallableContext& ctx) -> std::string {
        if (!expr) return {};
        switch (expr->kind()) {
        case syntax::SyntaxKind::LiteralExpression: {
            const auto& lit = static_cast<const syntax::LiteralExpressionSyntax&>(*expr);
            if (lit.literalToken.kind == syntax::SyntaxKind::FloatLiteralToken) {
                const auto& txt = lit.literalToken.text;
                if (!txt.empty() && (txt.back() == 'f' || txt.back() == 'F')) return "float";
                return "double";
            }
            if (lit.literalToken.kind == syntax::SyntaxKind::IntegerLiteralToken) return "int";
            if (lit.literalToken.kind == syntax::SyntaxKind::StringLiteralToken) return "string";
            if (lit.literalToken.kind == syntax::SyntaxKind::TrueKeyword ||
                lit.literalToken.kind == syntax::SyntaxKind::FalseKeyword) return "bool";
            if (lit.literalToken.kind == syntax::SyntaxKind::NullKeyword) return "null";
            return {};
        }
        case syntax::SyntaxKind::NameExpression: {
            const auto& nm = static_cast<const syntax::NameExpressionSyntax&>(*expr);
            const std::string& id = nm.identifierToken.text;
            if (const auto* loc = ctx.findLocalConst(id)) return loc->typeName;
            if (const auto pIt = ctx.parameterTypes.find(id); pIt != ctx.parameterTypes.end()) {
                return pIt->second;
            }
            if (const auto mIt = ctx.ownerMemberTypes.find(id); mIt != ctx.ownerMemberTypes.end()) {
                return mIt->second;
            }
            return id;
        }
        case syntax::SyntaxKind::ThisExpression:
            return ctx.ownerTypeName;
        case syntax::SyntaxKind::CastExpression: {
            const auto& c = static_cast<const syntax::CastExpressionSyntax&>(*expr);
            return renderTypeSyntaxSimple(c.type);
        }
        case syntax::SyntaxKind::ParenthesizedExpression: {
            const auto& p = static_cast<const syntax::ParenthesizedExpressionSyntax&>(*expr);
            return self(self, p.expression.get(), ctx);
        }
        case syntax::SyntaxKind::UnaryExpression: {
            const auto& u = static_cast<const syntax::UnaryExpressionSyntax&>(*expr);
            if (u.operatorToken.kind == syntax::SyntaxKind::BangToken) return "bool";
            return self(self, u.operand.get(), ctx);
        }
        case syntax::SyntaxKind::BinaryExpression: {
            const auto& b = static_cast<const syntax::BinaryExpressionSyntax&>(*expr);
            const auto op = b.operatorToken.kind;
            if (op == syntax::SyntaxKind::EqualsEqualsToken ||
                op == syntax::SyntaxKind::BangEqualsToken ||
                op == syntax::SyntaxKind::LessToken ||
                op == syntax::SyntaxKind::LessOrEqualsToken ||
                op == syntax::SyntaxKind::GreaterToken ||
                op == syntax::SyntaxKind::GreaterOrEqualsToken ||
                op == syntax::SyntaxKind::AmpersandAmpersandToken ||
                op == syntax::SyntaxKind::PipePipeToken) {
                return "bool";
            }
            const std::string lt = self(self, b.left.get(), ctx);
            const std::string rt = self(self, b.right.get(), ctx);
            if (op == syntax::SyntaxKind::QuestionQuestionToken) {
                return !rt.empty() ? rt : lt;
            }
            if (lt == "double" || rt == "double") return "double";
            if (lt == "float" || rt == "float") return "float";
            return !lt.empty() ? lt : rt;
        }
        case syntax::SyntaxKind::ConditionalExpression: {
            const auto& c = static_cast<const syntax::ConditionalExpressionSyntax&>(*expr);
            const std::string tt = self(self, c.whenTrue.get(), ctx);
            const std::string ft = self(self, c.whenFalse.get(), ctx);
            if (isFloatOrDoubleTypeName(tt)) return tt;
            if (isFloatOrDoubleTypeName(ft)) return ft;
            return !tt.empty() ? tt : ft;
        }
        case syntax::SyntaxKind::AssignmentExpression: {
            const auto& a = static_cast<const syntax::AssignmentExpressionSyntax&>(*expr);
            return self(self, a.expression.get(), ctx);
        }
        case syntax::SyntaxKind::MemberAccessExpression: {
            const auto& m = static_cast<const syntax::MemberAccessExpressionSyntax&>(*expr);
            const std::string recvType = stripModulePrefix(self(self, m.receiver.get(), ctx));
            if (recvType == ctx.ownerTypeName) {
                if (const auto it = ctx.ownerMemberTypes.find(m.nameToken.text);
                    it != ctx.ownerMemberTypes.end()) {
                    return it->second;
                }
            }
            if (const auto tIt = kb.typeMemberTypes.find(recvType); tIt != kb.typeMemberTypes.end()) {
                if (const auto mIt = tIt->second.find(m.nameToken.text); mIt != tIt->second.end()) {
                    return mIt->second;
                }
            }
            return {};
        }
        case syntax::SyntaxKind::CallExpression: {
            const auto& c = static_cast<const syntax::CallExpressionSyntax&>(*expr);
            const std::string fn = stripModulePrefix(c.identifierToken.text);
            if (fn == "checked" || fn == "unchecked") {
                return c.arguments.empty() ? std::string{} : self(self, c.arguments.front().get(), ctx);
            }
            if (const auto mIt = ctx.ownerMemberTypes.find(fn); mIt != ctx.ownerMemberTypes.end()) {
                return mIt->second;
            }
            if (const auto fIt = kb.functionReturnTypes.find(fn); fIt != kb.functionReturnTypes.end()) {
                return fIt->second;
            }
            return {};
        }
        case syntax::SyntaxKind::MemberCallExpression: {
            const auto& c = static_cast<const syntax::MemberCallExpressionSyntax&>(*expr);
            const std::string recvType = stripModulePrefix(self(self, c.receiver.get(), ctx));
            if (recvType == ctx.ownerTypeName) {
                if (const auto it = ctx.ownerMemberTypes.find(c.nameToken.text);
                    it != ctx.ownerMemberTypes.end()) {
                    return it->second;
                }
            }
            if (const auto tIt = kb.typeMemberTypes.find(recvType); tIt != kb.typeMemberTypes.end()) {
                if (const auto mIt = tIt->second.find(c.nameToken.text); mIt != tIt->second.end()) {
                    return mIt->second;
                }
            }
            return {};
        }
        case syntax::SyntaxKind::ElementAccessExpression: {
            const auto& e = static_cast<const syntax::ElementAccessExpressionSyntax&>(*expr);
            const std::string recvType = self(self, e.receiver.get(), ctx);
            if (recvType.size() > 2 && recvType.substr(recvType.size() - 2) == "[]") {
                return recvType.substr(0, recvType.size() - 2);
            }
            return {};
        }
        default:
            return {};
        }
    };

    const auto reportUnusedLocal = [&](const LocalBinding& loc) {
        report(
            "RSG003",
            loc.span,
            "Unused local variable '" + loc.name + "' is never read");
    };

    const auto visitExpr = [&](
        auto& self,
        const syntax::ExpressionSyntax* expr,
        CallableContext& ctx,
        bool valueConsumed) -> void {
        if (!expr) return;
        switch (expr->kind()) {
        case syntax::SyntaxKind::NameExpression: {
            const auto& nm = static_cast<const syntax::NameExpressionSyntax&>(*expr);
            const std::string& id = nm.identifierToken.text;
            if (auto* loc = ctx.findLocal(id)) {
                loc->used = true;
            } else if (!ctx.isShadowedByParamOrLocal(id) && ctx.readPrivateFields) {
                ctx.readPrivateFields->insert(id);
            }
            break;
        }
        case syntax::SyntaxKind::NewArrayExpression: {
            const auto& arr = static_cast<const syntax::NewArrayExpressionSyntax&>(*expr);
            if (ctx.isHotPathLifecycle) {
                report(
                    "RSG001",
                    arr.span(),
                    "Avoid heap array allocation inside frame hot-path method");
            }
            if (arr.explicitLength) self(self, arr.length.get(), ctx, true);
            for (const auto& iv : arr.initialValues) self(self, iv.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::NewObjectExpression: {
            const auto& obj = static_cast<const syntax::NewObjectExpressionSyntax&>(*expr);
            const std::string shortType = stripModulePrefix(obj.type.name.text);
            const bool isValueStruct =
                obj.type.isNullable() ||
                kb.structNames.find(shortType) != kb.structNames.end() ||
                kb.structNames.find(obj.type.name.text) != kb.structNames.end();
            if (ctx.isHotPathLifecycle && !isValueStruct) {
                report(
                    "RSG001",
                    obj.span(),
                    "Avoid heap object allocation 'new " + shortType + "(...)' inside frame hot-path method");
            }
            for (const auto& arg : obj.arguments) self(self, arg.get(), ctx, true);
            for (const auto& init : obj.initializers) {
                for (const auto& ie : init.expressions) self(self, ie.get(), ctx, true);
            }
            break;
        }
        case syntax::SyntaxKind::LambdaExpression: {
            const auto& lam = static_cast<const syntax::LambdaExpressionSyntax&>(*expr);
            if (ctx.isHotPathLifecycle) {
                report(
                    "RSG001",
                    lam.span(),
                    "Avoid lambda closure/delegate heap allocation inside frame hot-path method");
            }
            ctx.pushScope();
            for (const auto& p : lam.parameterTokens) {
                ctx.scopes.back().push_back({p.text, {}, p.span, true, true});
            }
            self(self, lam.body.get(), ctx, true);
            ctx.popScopeAndReportUnused(reportUnusedLocal);
            break;
        }
        case syntax::SyntaxKind::UnaryExpression: {
            const auto& u = static_cast<const syntax::UnaryExpressionSyntax&>(*expr);
            self(self, u.operand.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::BinaryExpression: {
            const auto& b = static_cast<const syntax::BinaryExpressionSyntax&>(*expr);
            if (ctx.isSequenceCoroutine &&
                (b.operatorToken.kind == syntax::SyntaxKind::EqualsEqualsToken ||
                 b.operatorToken.kind == syntax::SyntaxKind::BangEqualsToken)) {
                const std::string lt = inferExpressionType(inferExpressionType, b.left.get(), ctx);
                const std::string rt = inferExpressionType(inferExpressionType, b.right.get(), ctx);
                const bool nullCheck = (lt == "null" || rt == "null");
                if (!nullCheck && (isFloatOrDoubleTypeName(lt) || isFloatOrDoubleTypeName(rt))) {
                    report(
                        "RSG004",
                        b.span(),
                        "Avoid exact floating-point '" + b.operatorToken.text +
                            "' comparison inside deterministic 'sequence' coroutine; use Fixed math or epsilon tolerance");
                }
            }
            self(self, b.left.get(), ctx, true);
            self(self, b.right.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::ConditionalExpression: {
            const auto& c = static_cast<const syntax::ConditionalExpressionSyntax&>(*expr);
            self(self, c.condition.get(), ctx, true);
            self(self, c.whenTrue.get(), ctx, true);
            self(self, c.whenFalse.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::SwitchExpression: {
            const auto& sw = static_cast<const syntax::SwitchExpressionSyntax&>(*expr);
            self(self, sw.expression.get(), ctx, true);
            for (const auto& arm : sw.arms) {
                ctx.pushScope();
                if (arm.patternDesignation) {
                    const std::string pType = arm.patternType
                        ? renderTypeSyntaxSimple(*arm.patternType)
                        : std::string{};
                    ctx.scopes.back().push_back(
                        {arm.patternDesignation->text, pType, arm.patternDesignation->span, true, true});
                }
                self(self, arm.label.get(), ctx, true);
                self(self, arm.guard.get(), ctx, true);
                self(self, arm.value.get(), ctx, true);
                ctx.popScopeAndReportUnused(reportUnusedLocal);
            }
            break;
        }
        case syntax::SyntaxKind::TypeBinaryExpression: {
            const auto& tb = static_cast<const syntax::TypeBinaryExpressionSyntax&>(*expr);
            self(self, tb.expression.get(), ctx, true);
            if (tb.designationToken && !ctx.scopes.empty()) {
                ctx.scopes.back().push_back({
                    tb.designationToken->text,
                    renderTypeSyntaxSimple(tb.type),
                    tb.designationToken->span,
                    false,
                    tb.designationToken->text.empty() || tb.designationToken->text[0] == '_'});
            }
            break;
        }
        case syntax::SyntaxKind::AssignmentExpression: {
            const auto& a = static_cast<const syntax::AssignmentExpressionSyntax&>(*expr);
            self(self, a.expression.get(), ctx, true);
            const std::string& id = a.identifierToken.text;
            if (valueConsumed) {
                if (auto* loc = ctx.findLocal(id)) {
                    loc->used = true;
                } else if (!ctx.isShadowedByParamOrLocal(id) && ctx.readPrivateFields) {
                    ctx.readPrivateFields->insert(id);
                }
            } else if (a.equalsToken.kind != syntax::SyntaxKind::EqualsToken) {
                // Compound assignment `field += x` on a field reads its previous value if consumed?
                // Note: standalone write-only `x = 1` or `_unused += 1` without ever reading `x` remains unread!
            }
            break;
        }
        case syntax::SyntaxKind::MemberAssignmentExpression: {
            const auto& ma = static_cast<const syntax::MemberAssignmentExpressionSyntax&>(*expr);
            self(self, ma.receiver.get(), ctx, true);
            self(self, ma.expression.get(), ctx, true);
            if (valueConsumed && ctx.readPrivateFields) {
                ctx.readPrivateFields->insert(ma.nameToken.text);
            }
            break;
        }
        case syntax::SyntaxKind::ElementAssignmentExpression: {
            const auto& ea = static_cast<const syntax::ElementAssignmentExpressionSyntax&>(*expr);
            self(self, ea.receiver.get(), ctx, true);
            self(self, ea.index.get(), ctx, true);
            self(self, ea.expression.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::ParenthesizedExpression: {
            const auto& p = static_cast<const syntax::ParenthesizedExpressionSyntax&>(*expr);
            self(self, p.expression.get(), ctx, valueConsumed);
            break;
        }
        case syntax::SyntaxKind::CastExpression: {
            const auto& c = static_cast<const syntax::CastExpressionSyntax&>(*expr);
            self(self, c.expression.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::CallExpression: {
            const auto& c = static_cast<const syntax::CallExpressionSyntax&>(*expr);
            const std::string& callee = c.identifierToken.text;
            if (auto* loc = ctx.findLocal(callee)) {
                loc->used = true;
            } else if (!ctx.isShadowedByParamOrLocal(callee) && ctx.readPrivateFields) {
                ctx.readPrivateFields->insert(callee);
            }
            for (std::size_t ai = 0; ai < c.arguments.size(); ++ai) {
                const bool isOutArg =
                    ai < c.argumentModifiers.size() &&
                    c.argumentModifiers[ai].has_value() &&
                    c.argumentModifiers[ai]->kind == syntax::SyntaxKind::OutKeyword;
                if (isOutArg && c.arguments[ai] &&
                    c.arguments[ai]->kind() == syntax::SyntaxKind::NameExpression) {
                    // `out x` writes `x`; only `ref`/`in`/normal argument reads `x`
                    continue;
                }
                self(self, c.arguments[ai].get(), ctx, true);
            }
            break;
        }
        case syntax::SyntaxKind::MemberCallExpression: {
            const auto& c = static_cast<const syntax::MemberCallExpressionSyntax&>(*expr);
            self(self, c.receiver.get(), ctx, true);
            if (ctx.readPrivateFields) {
                ctx.readPrivateFields->insert(c.nameToken.text);
            }
            for (std::size_t ai = 0; ai < c.arguments.size(); ++ai) {
                const bool isOutArg =
                    ai < c.argumentModifiers.size() &&
                    c.argumentModifiers[ai].has_value() &&
                    c.argumentModifiers[ai]->kind == syntax::SyntaxKind::OutKeyword;
                if (isOutArg && c.arguments[ai] &&
                    c.arguments[ai]->kind() == syntax::SyntaxKind::NameExpression) {
                    continue;
                }
                self(self, c.arguments[ai].get(), ctx, true);
            }
            break;
        }
        case syntax::SyntaxKind::MemberAccessExpression: {
            const auto& m = static_cast<const syntax::MemberAccessExpressionSyntax&>(*expr);
            self(self, m.receiver.get(), ctx, true);
            if (ctx.readPrivateFields) {
                ctx.readPrivateFields->insert(m.nameToken.text);
            }
            break;
        }
        case syntax::SyntaxKind::ElementAccessExpression: {
            const auto& e = static_cast<const syntax::ElementAccessExpressionSyntax&>(*expr);
            self(self, e.receiver.get(), ctx, true);
            self(self, e.index.get(), ctx, true);
            break;
        }
        default:
            break;
        }
    };

    const auto visitStmt = [&](
        auto& self,
        const syntax::StatementSyntax* stmt,
        CallableContext& ctx) -> void {
        if (!stmt) return;
        switch (stmt->kind()) {
        case syntax::SyntaxKind::BlockStatement: {
            const auto& b = static_cast<const syntax::BlockStatementSyntax&>(*stmt);
            ctx.pushScope();
            for (const auto& s : b.statements) self(self, s.get(), ctx);
            ctx.popScopeAndReportUnused(reportUnusedLocal);
            break;
        }
        case syntax::SyntaxKind::VariableDeclarationStatement: {
            const auto& vd = static_cast<const syntax::VariableDeclarationStatementSyntax&>(*stmt);
            visitExpr(visitExpr, vd.initializer.get(), ctx, true);
            std::string resolvedType = renderTypeSyntaxSimple(vd.type);
            if (resolvedType == "var" && vd.initializer) {
                resolvedType = inferExpressionType(inferExpressionType, vd.initializer.get(), ctx);
            }
            const std::string& vName = vd.identifierToken.text;
            const bool skip =
                vName.empty() || vName[0] == '_' || vName[0] == '$' ||
                vd.identifierToken.missing ||
                vd.isUsingDeclaration ||
                vd.refKeyword.has_value();
            if (!ctx.scopes.empty()) {
                ctx.scopes.back().push_back(
                    {vName, std::move(resolvedType), vd.identifierToken.span, false, skip});
            }
            break;
        }
        case syntax::SyntaxKind::ExpressionStatement: {
            const auto& es = static_cast<const syntax::ExpressionStatementSyntax&>(*stmt);
            visitExpr(visitExpr, es.expression.get(), ctx, false);
            break;
        }
        case syntax::SyntaxKind::ReturnStatement: {
            const auto& r = static_cast<const syntax::ReturnStatementSyntax&>(*stmt);
            visitExpr(visitExpr, r.expression.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::ThrowStatement: {
            const auto& t = static_cast<const syntax::ThrowStatementSyntax&>(*stmt);
            visitExpr(visitExpr, t.expression.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::YieldWaitStatement: {
            const auto& y = static_cast<const syntax::YieldWaitStatementSyntax&>(*stmt);
            visitExpr(visitExpr, y.delay.get(), ctx, true);
            visitExpr(visitExpr, y.secondArgument.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::EventSubscriptionStatement: {
            const auto& ev = static_cast<const syntax::EventSubscriptionStatementSyntax&>(*stmt);
            if (ctx.isHotPathLifecycle &&
                (!ev.handler || ev.handler->kind() != syntax::SyntaxKind::LambdaExpression)) {
                report(
                    "RSG001",
                    ev.span(),
                    "Avoid delegate combination/allocation inside frame hot-path method");
            }
            visitExpr(visitExpr, ev.receiver.get(), ctx, true);
            visitExpr(visitExpr, ev.handler.get(), ctx, true);
            break;
        }
        case syntax::SyntaxKind::IfStatement: {
            const auto& ifs = static_cast<const syntax::IfStatementSyntax&>(*stmt);
            if (ifs.thenStatement && ifs.elseStatement &&
                statementsAreTokenEquivalent(source, *ifs.thenStatement, *ifs.elseStatement)) {
                report(
                    "RSG005",
                    ifs.ifKeyword.span,
                    "Suspicious 'if'/'else' control flow: both branches execute identical code");
            }
            ctx.pushScope();
            visitExpr(visitExpr, ifs.condition.get(), ctx, true);
            self(self, ifs.thenStatement.get(), ctx);
            if (ifs.elseStatement) self(self, ifs.elseStatement.get(), ctx);
            ctx.popScopeAndReportUnused(reportUnusedLocal);
            break;
        }
        case syntax::SyntaxKind::WhileStatement: {
            const auto& ws = static_cast<const syntax::WhileStatementSyntax&>(*stmt);
            ctx.pushScope();
            visitExpr(visitExpr, ws.condition.get(), ctx, true);
            self(self, ws.body.get(), ctx);
            ctx.popScopeAndReportUnused(reportUnusedLocal);
            break;
        }
        case syntax::SyntaxKind::ForStatement: {
            const auto& fs = static_cast<const syntax::ForStatementSyntax&>(*stmt);
            ctx.pushScope();
            self(self, fs.initializer.get(), ctx);
            visitExpr(visitExpr, fs.condition.get(), ctx, true);
            visitExpr(visitExpr, fs.increment.get(), ctx, false);
            self(self, fs.body.get(), ctx);
            ctx.popScopeAndReportUnused(reportUnusedLocal);
            break;
        }
        case syntax::SyntaxKind::ForeachStatement: {
            const auto& fe = static_cast<const syntax::ForeachStatementSyntax&>(*stmt);
            visitExpr(visitExpr, fe.collection.get(), ctx, true);
            ctx.pushScope();
            {
                const std::string& bName = fe.identifierToken.text;
                const bool skip = bName.empty() || bName[0] == '_' || bName[0] == '$' ||
                    fe.identifierToken.missing || fe.modifierToken.has_value();
                ctx.scopes.back().push_back(
                    {bName, renderTypeSyntaxSimple(fe.type), fe.identifierToken.span, false, skip});
            }
            for (const auto& extra : fe.additionalBindings) {
                const std::string& bName = extra.identifierToken.text;
                const bool skip = bName.empty() || bName[0] == '_' || bName[0] == '$' ||
                    extra.identifierToken.missing || extra.modifierToken.has_value();
                ctx.scopes.back().push_back(
                    {bName, renderTypeSyntaxSimple(extra.type), extra.identifierToken.span, false, skip});
            }
            self(self, fe.body.get(), ctx);
            ctx.popScopeAndReportUnused(reportUnusedLocal);
            break;
        }
        case syntax::SyntaxKind::DoWhileStatement: {
            const auto& dw = static_cast<const syntax::DoWhileStatementSyntax&>(*stmt);
            ctx.pushScope();
            self(self, dw.body.get(), ctx);
            visitExpr(visitExpr, dw.condition.get(), ctx, true);
            ctx.popScopeAndReportUnused(reportUnusedLocal);
            break;
        }
        case syntax::SyntaxKind::SwitchStatement: {
            const auto& sw = static_cast<const syntax::SwitchStatementSyntax&>(*stmt);
            visitExpr(visitExpr, sw.expression.get(), ctx, true);
            for (const auto& sec : sw.sections) {
                ctx.pushScope();
                if (sec.patternDesignation) {
                    const std::string pType = sec.patternType
                        ? renderTypeSyntaxSimple(*sec.patternType)
                        : std::string{};
                    ctx.scopes.back().push_back(
                        {sec.patternDesignation->text, pType, sec.patternDesignation->span, true, true});
                }
                visitExpr(visitExpr, sec.label.get(), ctx, true);
                visitExpr(visitExpr, sec.guard.get(), ctx, true);
                for (const auto& s : sec.statements) self(self, s.get(), ctx);
                ctx.popScopeAndReportUnused(reportUnusedLocal);
            }
            break;
        }
        case syntax::SyntaxKind::TryStatement: {
            const auto& tr = static_cast<const syntax::TryStatementSyntax&>(*stmt);
            const bool isSyntheticUsing =
                tr.finallyKeyword && tr.finallyKeyword->text == "$using_finally";
            self(self, &tr.body, ctx);
            for (const auto& c : tr.catches) {
                if (c.body.statements.empty() && !catchBlockHasComment(content, c.body)) {
                    report(
                        "RSG005",
                        c.catchKeyword.span,
                        "Empty 'catch' block swallows exceptions silently without statement or explanatory comment");
                }
                ctx.pushScope();
                if (c.identifierToken) {
                    const std::string cType = c.type ? renderTypeSyntaxSimple(*c.type) : std::string{};
                    ctx.scopes.back().push_back(
                        {c.identifierToken->text, cType, c.identifierToken->span, true, true});
                }
                for (const auto& cs : c.body.statements) self(self, cs.get(), ctx);
                ctx.popScopeAndReportUnused(reportUnusedLocal);
            }
            if (tr.finallyBody && !isSyntheticUsing) {
                self(self, &(*tr.finallyBody), ctx);
            }
            break;
        }
        default:
            break;
        }
    };

    const auto checkComplexity = [&](std::string_view kindLabel,
                                     const syntax::SyntaxToken& nameTok,
                                     const syntax::BlockStatementSyntax& body) {
        const std::size_t cc = 1u + countStatementComplexity(&body);
        if (cc > config_.maxCyclomaticComplexity) {
            report(
                "RSG002",
                nameTok.span,
                std::string(kindLabel) + " '" + nameTok.text + "' has cyclomatic complexity " +
                    std::to_string(cc) + " (exceeds maximum allowed " +
                    std::to_string(config_.maxCyclomaticComplexity) + "); consider refactoring");
        }
    };

    const auto analyzeTypeLike = [&](
        const std::string& typeName,
        const std::vector<syntax::FieldDeclarationSyntax>& fields,
        const std::vector<syntax::FunctionDeclarationSyntax>& methods,
        const std::vector<syntax::SequenceDeclarationSyntax>& sequences,
        const std::vector<syntax::ConstructorDeclarationSyntax>& constructors,
        const std::vector<syntax::PropertyDeclarationSyntax>& properties) {
        std::unordered_map<std::string, std::string> memberTypes;
        if (const auto it = kb.typeMemberTypes.find(typeName); it != kb.typeMemberTypes.end()) {
            memberTypes = it->second;
        }
        std::unordered_set<std::string> readPrivateFields;

        // Field initializers can read other private fields
        for (const auto& f : fields) {
            if (f.initializer) {
                CallableContext ctx;
                ctx.ownerTypeName = typeName;
                ctx.ownerMemberTypes = memberTypes;
                ctx.readPrivateFields = &readPrivateFields;
                ctx.pushScope();
                visitExpr(visitExpr, f.initializer.get(), ctx, true);
                ctx.popScopeAndReportUnused(reportUnusedLocal);
            }
        }

        for (const auto& ctor : constructors) {
            checkComplexity("Constructor", ctor.identifierToken, ctor.body);
            CallableContext ctx;
            ctx.ownerTypeName = typeName;
            ctx.ownerMemberTypes = memberTypes;
            ctx.readPrivateFields = &readPrivateFields;
            for (const auto& p : ctor.parameters) {
                ctx.parameters.insert(p.identifierToken.text);
                ctx.parameterTypes[p.identifierToken.text] = renderTypeSyntaxSimple(p.type);
            }
            ctx.pushScope();
            for (const auto& ba : ctor.baseArguments) {
                visitExpr(visitExpr, ba.get(), ctx, true);
            }
            for (const auto& s : ctor.body.statements) {
                visitStmt(visitStmt, s.get(), ctx);
            }
            ctx.popScopeAndReportUnused(reportUnusedLocal);
        }

        for (const auto& prop : properties) {
            if (prop.initializer) {
                CallableContext ctx;
                ctx.ownerTypeName = typeName;
                ctx.ownerMemberTypes = memberTypes;
                ctx.readPrivateFields = &readPrivateFields;
                ctx.pushScope();
                visitExpr(visitExpr, prop.initializer.get(), ctx, true);
                ctx.popScopeAndReportUnused(reportUnusedLocal);
            }
            if (prop.getter && prop.getter->body) {
                checkComplexity("Property getter", prop.identifierToken, *prop.getter->body);
                CallableContext ctx;
                ctx.ownerTypeName = typeName;
                ctx.ownerMemberTypes = memberTypes;
                ctx.readPrivateFields = &readPrivateFields;
                visitStmt(visitStmt, prop.getter->body.get(), ctx);
            }
            if (prop.setter && prop.setter->body) {
                checkComplexity("Property setter", prop.identifierToken, *prop.setter->body);
                CallableContext ctx;
                ctx.ownerTypeName = typeName;
                ctx.ownerMemberTypes = memberTypes;
                ctx.readPrivateFields = &readPrivateFields;
                ctx.parameters.insert("value");
                ctx.parameterTypes["value"] = renderTypeSyntaxSimple(prop.type);
                visitStmt(visitStmt, prop.setter->body.get(), ctx);
            }
        }

        for (const auto& m : methods) {
            if (!m.hasBody()) continue;
            checkComplexity("Method", m.identifierToken, m.body);
            CallableContext ctx;
            ctx.ownerTypeName = typeName;
            ctx.ownerMemberTypes = memberTypes;
            ctx.readPrivateFields = &readPrivateFields;
            ctx.isHotPathLifecycle = isHotLifecycleMethodName(m.identifierToken.text);
            for (const auto& p : m.parameters) {
                ctx.parameters.insert(p.identifierToken.text);
                ctx.parameterTypes[p.identifierToken.text] = renderTypeSyntaxSimple(p.type);
                if (p.defaultValue) {
                    ctx.pushScope();
                    visitExpr(visitExpr, p.defaultValue.get(), ctx, true);
                    ctx.popScopeAndReportUnused(reportUnusedLocal);
                }
            }
            visitStmt(visitStmt, &m.body, ctx);
        }

        for (const auto& seq : sequences) {
            checkComplexity("Sequence", seq.identifierToken, seq.body);
            CallableContext ctx;
            ctx.ownerTypeName = typeName;
            ctx.ownerMemberTypes = memberTypes;
            ctx.readPrivateFields = &readPrivateFields;
            ctx.isSequenceCoroutine = true;
            for (const auto& p : seq.parameters) {
                ctx.parameters.insert(p.identifierToken.text);
                ctx.parameterTypes[p.identifierToken.text] = renderTypeSyntaxSimple(p.type);
            }
            visitStmt(visitStmt, &seq.body, ctx);
        }

        // Report unread `private` fields (RSG003)
        for (const auto& f : fields) {
            if (!semantic::hasModifier(f.modifiers, syntax::SyntaxKind::PrivateKeyword)) {
                continue;
            }
            const std::string& fName = f.identifierToken.text;
            if (fName.empty() || fName[0] == '$' || f.identifierToken.missing) continue;
            if (readPrivateFields.find(fName) == readPrivateFields.end()) {
                report(
                    "RSG003",
                    f.identifierToken.span,
                    "Unread private field '" + typeName + "." + fName + "' is never read");
            }
        }
    };

    // 1. Analyze classes & structs
    for (const auto& cls : unit.classes) {
        analyzeTypeLike(
            cls.identifierToken.text,
            cls.fields,
            cls.methods,
            cls.sequences,
            cls.constructors,
            cls.properties);
    }
    for (const auto& st : unit.structs) {
        analyzeTypeLike(
            st.identifierToken.text,
            st.fields,
            st.methods,
            st.sequences,
            st.constructors,
            st.properties);
    }

    // 2. Analyze top-level functions
    for (const auto& fn : unit.functions) {
        if (!fn.hasBody()) continue;
        checkComplexity("Function", fn.identifierToken, fn.body);
        CallableContext ctx;
        ctx.isHotPathLifecycle = isHotLifecycleMethodName(fn.identifierToken.text);
        for (const auto& p : fn.parameters) {
            ctx.parameters.insert(p.identifierToken.text);
            ctx.parameterTypes[p.identifierToken.text] = renderTypeSyntaxSimple(p.type);
            if (p.defaultValue) {
                ctx.pushScope();
                visitExpr(visitExpr, p.defaultValue.get(), ctx, true);
                ctx.popScopeAndReportUnused(reportUnusedLocal);
            }
        }
        visitStmt(visitStmt, &fn.body, ctx);
    }

    // 3. Analyze unreferenced `import` declarations (RSG003)
    if (!unit.imports.empty() && config_.severityFor("RSG003") != LintSeverity::Off) {
        const std::string currentMod = unit.moduleDeclaration
            ? unit.moduleDeclaration->fullName()
            : std::string{"$global"};

        // Compute byte span of header (`module` + `import`s) so we only scan body tokens
        std::size_t bodyStartOffset = 0;
        if (unit.moduleDeclaration) {
            bodyStartOffset = std::max(bodyStartOffset, unit.moduleDeclaration->span().end());
        }
        for (const auto& imp : unit.imports) {
            bodyStartOffset = std::max(bodyStartOffset, imp.span().end());
        }

        std::unordered_set<std::string> usedIdentifiers;
        std::unordered_set<std::string> explicitModuleQualifiers;
        const auto& allTokens = parser.tokens();
        for (std::size_t ti = 0; ti < allTokens.size(); ++ti) {
            const auto& tok = allTokens[ti];
            if (tok.span.start < bodyStartOffset) continue;
            if (tok.kind == syntax::SyntaxKind::IdentifierToken) {
                usedIdentifiers.insert(tok.text);
                // Check `Mod.Sub::Ident` pattern in raw token stream
                if (ti + 2 < allTokens.size() &&
                    allTokens[ti + 1].kind == syntax::SyntaxKind::ColonToken &&
                    allTokens[ti + 2].kind == syntax::SyntaxKind::ColonToken) {
                    std::string modQual = tok.text;
                    std::size_t back = ti;
                    while (back >= 2 &&
                           allTokens[back - 1].kind == syntax::SyntaxKind::DotToken &&
                           allTokens[back - 2].kind == syntax::SyntaxKind::IdentifierToken) {
                        modQual = allTokens[back - 2].text + "." + modQual;
                        back -= 2;
                    }
                    explicitModuleQualifiers.insert(modQual);
                }
            }
        }

        std::unordered_set<std::string> semanticUsedModules;
        if (buildContext) {
            for (const auto& occ : buildContext->symbols) {
                if (occ.definition || occ.sourceName != path || occ.id == 0) continue;
                if (const auto defIt = kb.symbolDefModule.find(occ.id);
                    defIt != kb.symbolDefModule.end()) {
                    semanticUsedModules.insert(defIt->second);
                }
            }
        }

        // Collect all locally declared identifiers in this file (including local variables, params, members)
        std::unordered_set<std::string> localDeclaredWords;
        if (const auto selfIt = kb.moduleSymbols.find(currentMod); selfIt != kb.moduleSymbols.end()) {
            localDeclaredWords.insert(selfIt->second.begin(), selfIt->second.end());
        }
        for (const auto& c : unit.classes) {
            for (const auto& t : c.typeParameters) localDeclaredWords.insert(t.text);
            for (const auto& f : c.fields) localDeclaredWords.insert(f.identifierToken.text);
            for (const auto& p : c.properties) localDeclaredWords.insert(p.identifierToken.text);
            for (const auto& e : c.events) localDeclaredWords.insert(e.identifierToken.text);
            for (const auto& m : c.methods) {
                localDeclaredWords.insert(m.identifierToken.text);
                for (const auto& prm : m.parameters) localDeclaredWords.insert(prm.identifierToken.text);
            }
            for (const auto& s : c.sequences) {
                localDeclaredWords.insert(s.identifierToken.text);
                for (const auto& prm : s.parameters) localDeclaredWords.insert(prm.identifierToken.text);
            }
            for (const auto& ctor : c.constructors) {
                for (const auto& prm : ctor.parameters) localDeclaredWords.insert(prm.identifierToken.text);
            }
        }
        for (const auto& s : unit.structs) {
            for (const auto& t : s.typeParameters) localDeclaredWords.insert(t.text);
            for (const auto& f : s.fields) localDeclaredWords.insert(f.identifierToken.text);
            for (const auto& p : s.properties) localDeclaredWords.insert(p.identifierToken.text);
            for (const auto& m : s.methods) {
                localDeclaredWords.insert(m.identifierToken.text);
                for (const auto& prm : m.parameters) localDeclaredWords.insert(prm.identifierToken.text);
            }
        }
        for (const auto& e : unit.enums) {
            for (const auto& m : e.members) localDeclaredWords.insert(m.identifierToken.text);
        }
        for (const auto& fn : unit.functions) {
            localDeclaredWords.insert(fn.identifierToken.text);
            for (const auto& prm : fn.parameters) localDeclaredWords.insert(prm.identifierToken.text);
        }
        // Standard generic collections & contextual identifiers built into RealScript
        for (const char* stdWord : {
                 "var", "where", "using", "checked", "unchecked",
                 "wait_ticks", "wait_seconds", "wait_event", "wait_signal", "wait_until", "wait_async",
                 "List", "Dictionary", "Queue", "Stack", "HashSet", "Nullable",
                 "Add", "Remove", "Contains", "ContainsKey", "ContainsValue", "Count", "Capacity",
                 "IsEmpty", "Enqueue", "Dequeue", "Push", "Pop", "Peek", "Clear", "Get", "Set",
                 "Length", "length", "HasValue", "Value", "GetValueOrDefault",
                 "Substring", "Trim", "Replace", "StartsWith", "EndsWith", "IndexOf", "ToUpper", "ToLower",
                 "Export", "Serializable", "Guid", "Signal", "Deterministic", "NonDeterministic", "Range"}) {
            localDeclaredWords.insert(stdWord);
        }

        // Also record VariableDeclaration identifiers so local variable names aren't mistaken for external module symbols
        for (std::size_t ti = 0; ti + 1 < allTokens.size(); ++ti) {
            if (allTokens[ti + 1].kind == syntax::SyntaxKind::IdentifierToken &&
                (syntax::isPrimitiveTypeKeyword(allTokens[ti].kind) ||
                 allTokens[ti].text == "var" ||
                 allTokens[ti].kind == syntax::SyntaxKind::CloseBracketToken ||
                 allTokens[ti].kind == syntax::SyntaxKind::GreaterToken)) {
                if (ti + 2 < allTokens.size() &&
                    (allTokens[ti + 2].kind == syntax::SyntaxKind::EqualsToken ||
                     allTokens[ti + 2].kind == syntax::SyntaxKind::SemicolonToken ||
                     allTokens[ti + 2].kind == syntax::SyntaxKind::InKeyword)) {
                    localDeclaredWords.insert(allTokens[ti + 1].text);
                }
            }
        }

        for (const auto& imp : unit.imports) {
            const std::string impMod = imp.fullName();
            if (impMod.empty()) continue;

            bool referenced =
                explicitModuleQualifiers.find(impMod) != explicitModuleQualifiers.end() ||
                semanticUsedModules.find(impMod) != semanticUsedModules.end();

            if (!referenced) {
                if (const auto expIt = kb.moduleSymbols.find(impMod); expIt != kb.moduleSymbols.end()) {
                    for (const auto& sym : expIt->second) {
                        if (usedIdentifiers.find(sym) != usedIdentifiers.end()) {
                            referenced = true;
                            break;
                        }
                    }
                } else {
                    // Imported module is not in known workspace/build context:
                    // check if there is any unresolved external identifier in the file that could come from it
                    for (const auto& id : usedIdentifiers) {
                        if (localDeclaredWords.find(id) != localDeclaredWords.end()) continue;
                        bool explainedByOtherKnownMod = false;
                        for (const auto& otherImp : unit.imports) {
                            const std::string otherName = otherImp.fullName();
                            if (otherName == impMod) continue;
                            if (const auto oIt = kb.moduleSymbols.find(otherName);
                                oIt != kb.moduleSymbols.end() &&
                                oIt->second.find(id) != oIt->second.end()) {
                                explainedByOtherKnownMod = true;
                                break;
                            }
                        }
                        if (!explainedByOtherKnownMod) {
                            referenced = true;
                            break;
                        }
                    }
                }
            }

            if (!referenced) {
                report(
                    "RSG003",
                    imp.span(),
                    "Unreferenced import '" + impMod + "' is never used in this file");
            }
        }
    }

    std::stable_sort(
        emitted.begin(),
        emitted.end(),
        [](const LintDiagnostic& lhs, const LintDiagnostic& rhs) {
            if (lhs.span.start != rhs.span.start) return lhs.span.start < rhs.span.start;
            return lhs.code < rhs.code;
        });
    return emitted;
}

std::vector<LintDiagnostic> GovernanceLinter::lintWorkspace(
    const std::vector<compiler::SourceFile>& sources,
    const compiler::BuildResult* buildContext) const {
    compiler::BuildResult localBuild;
    const compiler::BuildResult* effectiveBuild = buildContext;
    if (!effectiveBuild && !sources.empty()) {
        compiler::Compilation comp(sources);
        localBuild = comp.build();
        effectiveBuild = &localBuild;
    }

    std::vector<LintDiagnostic> all;
    for (const auto& src : sources) {
        auto fileDiags = lintSource(src.path, src.content, effectiveBuild, &sources);
        all.insert(
            all.end(),
            std::make_move_iterator(fileDiags.begin()),
            std::make_move_iterator(fileDiags.end()));
    }
    return all;
}

} // namespace realscript::tooling
