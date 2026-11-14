#include "realscript/plugin/ModHost.h"

#include "realscript/tooling/Json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

namespace realscript::plugin {

namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool parseUint32Component(std::string_view part, std::uint32_t& out) noexcept {
    if (part.empty()) return false;
    if (part.size() > 1 && part.front() == '0') {
        return false;
    }
    std::uint64_t acc = 0;
    for (const char ch : part) {
        if (ch < '0' || ch > '9') return false;
        acc = acc * 10u + static_cast<std::uint64_t>(ch - '0');
        if (acc > std::numeric_limits<std::uint32_t>::max()) return false;
    }
    out = static_cast<std::uint32_t>(acc);
    return true;
}

runtime::Value defaultElementForPrimitive(
    semantic::PrimitiveType elemType,
    semantic::SymbolId elemTypeId) {
    switch (elemType) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return runtime::ByteValue{};
    case semantic::PrimitiveType::SByte: return runtime::SByteValue{};
    case semantic::PrimitiveType::Short: return runtime::ShortValue{};
    case semantic::PrimitiveType::UShort: return runtime::UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return runtime::UIntValue{};
    case semantic::PrimitiveType::Long: return runtime::LongValue{};
    case semantic::PrimitiveType::ULong: return runtime::ULongValue{};
    case semantic::PrimitiveType::Float: return runtime::FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return runtime::CharValue{};
    case semantic::PrimitiveType::String: return runtime::NullString{};
    case semantic::PrimitiveType::Object: return runtime::NullObject{};
    case semantic::PrimitiveType::Array: return runtime::NullArray{};
    case semantic::PrimitiveType::Enum: return runtime::EnumValue{elemTypeId, 0};
    case semantic::PrimitiveType::Struct:
        if (const auto def = runtime::makeDefaultInlineStructValue(elemTypeId)) {
            return *def;
        }
        return runtime::StructValue{};
    default:
        return runtime::Value{};
    }
}

void walkExpressionSyntax(
    const syntax::ExpressionSyntax* expr,
    const std::function<void(const syntax::SyntaxNode&)>& visitor);

void walkPatternSyntax(
    const syntax::PatternSyntax* pat,
    const std::function<void(const syntax::SyntaxNode&)>& visitor) {
    if (!pat) return;
    visitor(*pat);
    switch (pat->kind()) {
    case syntax::SyntaxKind::ConstantPattern: {
        const auto& cp = static_cast<const syntax::ConstantPatternSyntax&>(*pat);
        walkExpressionSyntax(cp.expression.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::RelationalPattern: {
        const auto& rp = static_cast<const syntax::RelationalPatternSyntax&>(*pat);
        walkExpressionSyntax(rp.operand.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::UnaryPattern: {
        const auto& up = static_cast<const syntax::UnaryPatternSyntax&>(*pat);
        walkPatternSyntax(up.pattern.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::BinaryPattern: {
        const auto& bp = static_cast<const syntax::BinaryPatternSyntax&>(*pat);
        walkPatternSyntax(bp.left.get(), visitor);
        walkPatternSyntax(bp.right.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::PropertyPattern: {
        const auto& pp = static_cast<const syntax::PropertyPatternSyntax&>(*pat);
        for (const auto& sub : pp.properties) {
            walkPatternSyntax(sub.pattern.get(), visitor);
        }
        break;
    }
    case syntax::SyntaxKind::ParenthesizedPattern: {
        const auto& par = static_cast<const syntax::ParenthesizedPatternSyntax&>(*pat);
        walkPatternSyntax(par.pattern.get(), visitor);
        break;
    }
    default:
        break;
    }
}

void walkExpressionSyntax(
    const syntax::ExpressionSyntax* expr,
    const std::function<void(const syntax::SyntaxNode&)>& visitor) {
    if (!expr) return;
    visitor(*expr);
    switch (expr->kind()) {
    case syntax::SyntaxKind::UnaryExpression: {
        const auto& u = static_cast<const syntax::UnaryExpressionSyntax&>(*expr);
        walkExpressionSyntax(u.operand.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::BinaryExpression: {
        const auto& b = static_cast<const syntax::BinaryExpressionSyntax&>(*expr);
        walkExpressionSyntax(b.left.get(), visitor);
        walkExpressionSyntax(b.right.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::ConditionalExpression: {
        const auto& c = static_cast<const syntax::ConditionalExpressionSyntax&>(*expr);
        walkExpressionSyntax(c.condition.get(), visitor);
        walkExpressionSyntax(c.whenTrue.get(), visitor);
        walkExpressionSyntax(c.whenFalse.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::AssignmentExpression: {
        const auto& a = static_cast<const syntax::AssignmentExpressionSyntax&>(*expr);
        walkExpressionSyntax(a.expression.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::ParenthesizedExpression: {
        const auto& p = static_cast<const syntax::ParenthesizedExpressionSyntax&>(*expr);
        walkExpressionSyntax(p.expression.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::CastExpression: {
        const auto& c = static_cast<const syntax::CastExpressionSyntax&>(*expr);
        walkExpressionSyntax(c.expression.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::CallExpression: {
        const auto& c = static_cast<const syntax::CallExpressionSyntax&>(*expr);
        for (const auto& arg : c.arguments) walkExpressionSyntax(arg.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::MemberCallExpression: {
        const auto& mc = static_cast<const syntax::MemberCallExpressionSyntax&>(*expr);
        walkExpressionSyntax(mc.receiver.get(), visitor);
        for (const auto& arg : mc.arguments) walkExpressionSyntax(arg.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::MemberAccessExpression: {
        const auto& ma = static_cast<const syntax::MemberAccessExpressionSyntax&>(*expr);
        walkExpressionSyntax(ma.receiver.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::MemberAssignmentExpression: {
        const auto& ma = static_cast<const syntax::MemberAssignmentExpressionSyntax&>(*expr);
        walkExpressionSyntax(ma.receiver.get(), visitor);
        walkExpressionSyntax(ma.expression.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::ElementAccessExpression: {
        const auto& ea = static_cast<const syntax::ElementAccessExpressionSyntax&>(*expr);
        walkExpressionSyntax(ea.receiver.get(), visitor);
        walkExpressionSyntax(ea.index.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::ElementAssignmentExpression: {
        const auto& ea = static_cast<const syntax::ElementAssignmentExpressionSyntax&>(*expr);
        walkExpressionSyntax(ea.receiver.get(), visitor);
        walkExpressionSyntax(ea.index.get(), visitor);
        walkExpressionSyntax(ea.expression.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::NewObjectExpression: {
        const auto& no = static_cast<const syntax::NewObjectExpressionSyntax&>(*expr);
        for (const auto& arg : no.arguments) walkExpressionSyntax(arg.get(), visitor);
        for (const auto& init : no.initializers) {
            for (const auto& sub : init.expressions) walkExpressionSyntax(sub.get(), visitor);
        }
        break;
    }
    case syntax::SyntaxKind::NewArrayExpression: {
        const auto& na = static_cast<const syntax::NewArrayExpressionSyntax&>(*expr);
        walkExpressionSyntax(na.length.get(), visitor);
        for (const auto& iv : na.initialValues) walkExpressionSyntax(iv.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::LambdaExpression: {
        const auto& lam = static_cast<const syntax::LambdaExpressionSyntax&>(*expr);
        walkExpressionSyntax(lam.body.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::TypeBinaryExpression: {
        const auto& tb = static_cast<const syntax::TypeBinaryExpressionSyntax&>(*expr);
        walkExpressionSyntax(tb.expression.get(), visitor);
        walkPatternSyntax(tb.pattern.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::SwitchExpression: {
        const auto& sw = static_cast<const syntax::SwitchExpressionSyntax&>(*expr);
        walkExpressionSyntax(sw.expression.get(), visitor);
        for (const auto& arm : sw.arms) {
            walkPatternSyntax(arm.pattern.get(), visitor);
            walkExpressionSyntax(arm.label.get(), visitor);
            walkExpressionSyntax(arm.guard.get(), visitor);
            walkExpressionSyntax(arm.value.get(), visitor);
        }
        break;
    }
    default:
        break;
    }
}

void walkStatementSyntax(
    const syntax::StatementSyntax* stmt,
    const std::function<void(const syntax::SyntaxNode&)>& visitor) {
    if (!stmt) return;
    visitor(*stmt);
    switch (stmt->kind()) {
    case syntax::SyntaxKind::BlockStatement: {
        const auto& blk = static_cast<const syntax::BlockStatementSyntax&>(*stmt);
        for (const auto& child : blk.statements) walkStatementSyntax(child.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::ReturnStatement: {
        const auto& ret = static_cast<const syntax::ReturnStatementSyntax&>(*stmt);
        walkExpressionSyntax(ret.expression.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::IfStatement: {
        const auto& ifs = static_cast<const syntax::IfStatementSyntax&>(*stmt);
        walkExpressionSyntax(ifs.condition.get(), visitor);
        walkStatementSyntax(ifs.thenStatement.get(), visitor);
        walkStatementSyntax(ifs.elseStatement.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::WhileStatement: {
        const auto& ws = static_cast<const syntax::WhileStatementSyntax&>(*stmt);
        walkExpressionSyntax(ws.condition.get(), visitor);
        walkStatementSyntax(ws.body.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::ForStatement: {
        const auto& fs = static_cast<const syntax::ForStatementSyntax&>(*stmt);
        walkStatementSyntax(fs.initializer.get(), visitor);
        walkExpressionSyntax(fs.condition.get(), visitor);
        walkExpressionSyntax(fs.increment.get(), visitor);
        walkStatementSyntax(fs.body.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::ForeachStatement: {
        const auto& fes = static_cast<const syntax::ForeachStatementSyntax&>(*stmt);
        walkExpressionSyntax(fes.collection.get(), visitor);
        walkStatementSyntax(fes.body.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::DoWhileStatement: {
        const auto& dws = static_cast<const syntax::DoWhileStatementSyntax&>(*stmt);
        walkStatementSyntax(dws.body.get(), visitor);
        walkExpressionSyntax(dws.condition.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::SwitchStatement: {
        const auto& sw = static_cast<const syntax::SwitchStatementSyntax&>(*stmt);
        walkExpressionSyntax(sw.expression.get(), visitor);
        for (const auto& sec : sw.sections) {
            walkPatternSyntax(sec.pattern.get(), visitor);
            walkExpressionSyntax(sec.label.get(), visitor);
            walkExpressionSyntax(sec.guard.get(), visitor);
            for (const auto& s : sec.statements) walkStatementSyntax(s.get(), visitor);
        }
        break;
    }
    case syntax::SyntaxKind::ThrowStatement: {
        const auto& ts = static_cast<const syntax::ThrowStatementSyntax&>(*stmt);
        walkExpressionSyntax(ts.expression.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::TryStatement: {
        const auto& tr = static_cast<const syntax::TryStatementSyntax&>(*stmt);
        walkStatementSyntax(&tr.body, visitor);
        for (const auto& c : tr.catches) walkStatementSyntax(&c.body, visitor);
        if (tr.finallyBody) walkStatementSyntax(&*tr.finallyBody, visitor);
        break;
    }
    case syntax::SyntaxKind::YieldWaitStatement: {
        const auto& yw = static_cast<const syntax::YieldWaitStatementSyntax&>(*stmt);
        walkExpressionSyntax(yw.delay.get(), visitor);
        walkExpressionSyntax(yw.secondArgument.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::EventSubscriptionStatement: {
        const auto& es = static_cast<const syntax::EventSubscriptionStatementSyntax&>(*stmt);
        walkExpressionSyntax(es.receiver.get(), visitor);
        walkExpressionSyntax(es.handler.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::VariableDeclarationStatement: {
        const auto& vd = static_cast<const syntax::VariableDeclarationStatementSyntax&>(*stmt);
        walkExpressionSyntax(vd.initializer.get(), visitor);
        break;
    }
    case syntax::SyntaxKind::ExpressionStatement: {
        const auto& ex = static_cast<const syntax::ExpressionStatementSyntax&>(*stmt);
        walkExpressionSyntax(ex.expression.get(), visitor);
        break;
    }
    default:
        break;
    }
}

void walkCompilationUnitSyntax(
    const syntax::CompilationUnitSyntax& unit,
    const std::function<void(const syntax::SyntaxNode&)>& visitor) {
    visitor(unit);
    if (unit.moduleDeclaration) visitor(*unit.moduleDeclaration);
    for (const auto& imp : unit.imports) visitor(imp);
    const auto walkParameters = [&](const std::vector<syntax::ParameterSyntax>& params) {
        for (const auto& param : params) {
            visitor(param);
            walkExpressionSyntax(param.defaultValue.get(), visitor);
        }
    };
    for (const auto& fn : unit.functions) {
        visitor(fn);
        walkParameters(fn.parameters);
        if (fn.hasBody()) walkStatementSyntax(&fn.body, visitor);
    }
    const auto walkTypeLike = [&](const auto& decl) {
        visitor(decl);
        for (const auto& f : decl.fields) {
            visitor(f);
            walkExpressionSyntax(f.initializer.get(), visitor);
        }
        for (const auto& c : decl.constructors) {
            visitor(c);
            walkParameters(c.parameters);
            for (const auto& arg : c.baseArguments) {
                walkExpressionSyntax(arg.get(), visitor);
            }
            walkStatementSyntax(&c.body, visitor);
        }
        for (const auto& m : decl.methods) {
            visitor(m);
            walkParameters(m.parameters);
            if (m.hasBody()) walkStatementSyntax(&m.body, visitor);
        }
        for (const auto& seq : decl.sequences) {
            visitor(seq);
            walkParameters(seq.parameters);
            walkStatementSyntax(&seq.body, visitor);
        }
        for (const auto& p : decl.properties) {
            visitor(p);
            if (p.getter && p.getter->body) walkStatementSyntax(p.getter->body.get(), visitor);
            if (p.setter && p.setter->body) walkStatementSyntax(p.setter->body.get(), visitor);
            walkExpressionSyntax(p.initializer.get(), visitor);
        }
    };
    for (const auto& cl : unit.classes) walkTypeLike(cl);
    for (const auto& st : unit.structs) walkTypeLike(st);
    for (const auto& en : unit.enums) visitor(en);
    for (const auto& in : unit.interfaces) {
        visitor(in);
        for (const auto& m : in.methods) {
            visitor(m);
            walkParameters(m.parameters);
            if (m.hasBody()) walkStatementSyntax(&*m.body, visitor);
        }
    }
    for (const auto& del : unit.delegates) {
        visitor(del);
        walkParameters(del.parameters);
    }
}

} // namespace

// ============================================================================
// SemVer & VersionConstraint Implementation
// ============================================================================

std::optional<SemVer> SemVer::parse(std::string_view text) noexcept {
    const std::string s = trim(text);
    if (s.empty()) return std::nullopt;
    std::string_view view(s);

    const auto dot1 = view.find('.');
    if (dot1 == std::string_view::npos) return std::nullopt;
    const auto dot2 = view.find('.', dot1 + 1u);
    if (dot2 == std::string_view::npos) return std::nullopt;
    if (view.find('.', dot2 + 1u) != std::string_view::npos) return std::nullopt;

    SemVer v;
    if (!parseUint32Component(view.substr(0, dot1), v.major) ||
        !parseUint32Component(view.substr(dot1 + 1u, dot2 - dot1 - 1u), v.minor) ||
        !parseUint32Component(view.substr(dot2 + 1u), v.patch)) {
        return std::nullopt;
    }
    return v;
}

std::string SemVer::toString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

bool VersionComparator::matches(SemVer version) const noexcept {
    switch (op) {
    case VersionComparatorOp::Any: return true;
    case VersionComparatorOp::Equal: return version == target;
    case VersionComparatorOp::NotEqual: return version != target;
    case VersionComparatorOp::Greater: return version > target;
    case VersionComparatorOp::GreaterOrEqual: return version >= target;
    case VersionComparatorOp::Less: return version < target;
    case VersionComparatorOp::LessOrEqual: return version <= target;
    }
    return false;
}

std::optional<VersionConstraint> VersionConstraint::parse(std::string_view expr) {
    const std::string cleaned = trim(expr);
    if (cleaned.empty()) return std::nullopt;

    VersionConstraint out;
    out.rawExpression = cleaned;
    out.valid = true;

    if (cleaned == "*") {
        out.comparators.push_back(VersionComparator{VersionComparatorOp::Any, {}});
        return out;
    }

    std::size_t pos = 0;
    const std::size_t n = cleaned.size();
    while (pos < n) {
        while (pos < n && (std::isspace(static_cast<unsigned char>(cleaned[pos])) ||
                           cleaned[pos] == ',')) {
            ++pos;
        }
        if (pos >= n) break;

        if (cleaned[pos] == '*') {
            ++pos;
            if (pos < n && !std::isspace(static_cast<unsigned char>(cleaned[pos])) &&
                cleaned[pos] != ',') {
                return std::nullopt;
            }
            out.comparators.push_back(VersionComparator{VersionComparatorOp::Any, {}});
            continue;
        }

        std::string opStr;
        if (cleaned.compare(pos, 2, ">=") == 0 ||
            cleaned.compare(pos, 2, "<=") == 0 ||
            cleaned.compare(pos, 2, "!=") == 0) {
            opStr = cleaned.substr(pos, 2);
            pos += 2;
        } else if (cleaned[pos] == '^' || cleaned[pos] == '~' ||
                   cleaned[pos] == '=' || cleaned[pos] == '>' || cleaned[pos] == '<') {
            opStr.push_back(cleaned[pos]);
            ++pos;
        }

        while (pos < n && std::isspace(static_cast<unsigned char>(cleaned[pos]))) {
            ++pos;
        }
        const std::size_t verStart = pos;
        while (pos < n && !std::isspace(static_cast<unsigned char>(cleaned[pos])) &&
               cleaned[pos] != ',' &&
               cleaned[pos] != '<' && cleaned[pos] != '>' &&
               cleaned[pos] != '=' && cleaned[pos] != '^' && cleaned[pos] != '~') {
            ++pos;
        }
        if (pos == verStart) return std::nullopt;
        const auto ver = SemVer::parse(std::string_view(cleaned).substr(verStart, pos - verStart));
        if (!ver) return std::nullopt;

        if (opStr.empty() || opStr == "=") {
            out.comparators.push_back(VersionComparator{VersionComparatorOp::Equal, *ver});
        } else if (opStr == "!=") {
            out.comparators.push_back(VersionComparator{VersionComparatorOp::NotEqual, *ver});
        } else if (opStr == ">") {
            out.comparators.push_back(VersionComparator{VersionComparatorOp::Greater, *ver});
        } else if (opStr == ">=") {
            out.comparators.push_back(VersionComparator{VersionComparatorOp::GreaterOrEqual, *ver});
        } else if (opStr == "<") {
            out.comparators.push_back(VersionComparator{VersionComparatorOp::Less, *ver});
        } else if (opStr == "<=") {
            out.comparators.push_back(VersionComparator{VersionComparatorOp::LessOrEqual, *ver});
        } else if (opStr == "^") {
            out.comparators.push_back(VersionComparator{VersionComparatorOp::GreaterOrEqual, *ver});
            SemVer upper{};
            if (ver->major > 0) {
                upper = SemVer{ver->major + 1u, 0u, 0u};
            } else if (ver->minor > 0) {
                upper = SemVer{0u, ver->minor + 1u, 0u};
            } else {
                upper = SemVer{0u, 0u, ver->patch + 1u};
            }
            out.comparators.push_back(VersionComparator{VersionComparatorOp::Less, upper});
        } else if (opStr == "~") {
            out.comparators.push_back(VersionComparator{VersionComparatorOp::GreaterOrEqual, *ver});
            const SemVer upper{ver->major, ver->minor + 1u, 0u};
            out.comparators.push_back(VersionComparator{VersionComparatorOp::Less, upper});
        } else {
            return std::nullopt;
        }
    }

    if (out.comparators.empty()) return std::nullopt;
    return out;
}

bool VersionConstraint::satisfies(SemVer version) const noexcept {
    if (!valid) return false;
    for (const auto& cmp : comparators) {
        if (!cmp.matches(version)) return false;
    }
    return true;
}

ModDependency::ModDependency(std::string id, std::string_view constraintExpr)
    : modId(std::move(id)) {
    if (auto parsed = VersionConstraint::parse(constraintExpr)) {
        constraint = std::move(*parsed);
    } else {
        constraint.rawExpression = std::string(constraintExpr);
        constraint.valid = false;
    }
}

// ============================================================================
// JSON Manifest Parsing & Topological Dependency Solver
// ============================================================================

bool parseModManifestJson(
    const std::string& jsonText,
    ModManifest& outManifest,
    std::string& outError) {
    std::string parseErr;
    const auto rootOpt = tooling::Json::parse(jsonText, parseErr);
    if (!rootOpt || !rootOpt->isObject()) {
        outError = parseErr.empty() ? "manifest root must be a JSON object" : parseErr;
        return false;
    }
    const auto& root = *rootOpt;

    ModManifest manifest;
    const auto* idField = root.find("id");
    if (!idField || !idField->isString() || idField->stringValue().empty()) {
        outError = "manifest requires a non-empty string 'id'";
        return false;
    }
    manifest.id = idField->stringValue();

    const auto* verField = root.find("version");
    if (!verField || !verField->isString()) {
        outError = "manifest '" + manifest.id + "' requires a string 'version'";
        return false;
    }
    const auto parsedVer = SemVer::parse(verField->stringValue());
    if (!parsedVer) {
        outError = "manifest '" + manifest.id + "' has invalid SemVer '" + verField->stringValue() + "'";
        return false;
    }
    manifest.version = *parsedVer;

    if (const auto* depsField = root.find("dependencies")) {
        if (depsField->isObject()) {
            for (const auto& [depId, depVal] : depsField->objectValue()) {
                if (depId.empty() || !depVal.isString()) {
                    outError = "dependency entry in '" + manifest.id + "' must map modId to constraint string";
                    return false;
                }
                const auto constraint = VersionConstraint::parse(depVal.stringValue());
                if (!constraint) {
                    outError = "invalid version constraint '" + depVal.stringValue() +
                        "' for dependency '" + depId + "' in '" + manifest.id + "'";
                    return false;
                }
                manifest.dependencies.push_back(ModDependency{depId, *constraint});
            }
        } else if (depsField->isArray()) {
            for (const auto& item : depsField->arrayValue()) {
                if (!item.isObject()) {
                    outError = "dependency array element must be an object";
                    return false;
                }
                const auto* depId = item.find("id");
                const auto* depVer = item.find("version");
                if (!depVer) depVer = item.find("constraint");
                if (!depId || !depId->isString() || depId->stringValue().empty() ||
                    !depVer || !depVer->isString()) {
                    outError = "dependency object requires string 'id' and 'version'";
                    return false;
                }
                const auto constraint = VersionConstraint::parse(depVer->stringValue());
                if (!constraint) {
                    outError = "invalid version constraint '" + depVer->stringValue() + "'";
                    return false;
                }
                manifest.dependencies.push_back(ModDependency{depId->stringValue(), *constraint});
            }
        } else {
            outError = "'dependencies' must be a JSON object or array";
            return false;
        }
    }

    if (const auto* capsField = root.find("grantedCapabilities")) {
        if (!capsField->isArray()) {
            outError = "'grantedCapabilities' must be a JSON array of strings";
            return false;
        }
        for (const auto& cap : capsField->arrayValue()) {
            if (!cap.isString()) {
                outError = "capability entry in 'grantedCapabilities' must be a string";
                return false;
            }
            if (!cap.stringValue().empty()) {
                manifest.grantedCapabilities.insert(cap.stringValue());
            }
        }
    }

    if (const auto* heapField = root.find("maxHeapBytes")) {
        if (!heapField->isNumber() || heapField->integerValue(-1) <= 0) {
            outError = "'maxHeapBytes' must be a positive integer";
            return false;
        }
        manifest.maxHeapBytes = static_cast<std::size_t>(heapField->integerValue());
    }

    if (const auto* budgetField = root.find("tickInstructionBudget")) {
        if (!budgetField->isNumber() || budgetField->integerValue(-1) <= 0) {
            outError = "'tickInstructionBudget' must be a positive integer";
            return false;
        }
        manifest.tickInstructionBudget = static_cast<std::uint64_t>(budgetField->integerValue());
    }

    if (const auto* entryField = root.find("entryModule")) {
        if (!entryField->isString()) {
            outError = "'entryModule' must be a string";
            return false;
        }
        manifest.entryModule = entryField->stringValue();
    }

    outManifest = std::move(manifest);
    outError.clear();
    return true;
}

std::optional<ModManifest> parseModManifestJson(
    const std::string& jsonText,
    std::string* outError) {
    ModManifest manifest;
    std::string err;
    if (!parseModManifestJson(jsonText, manifest, err)) {
        if (outError) *outError = std::move(err);
        return std::nullopt;
    }
    if (outError) outError->clear();
    return manifest;
}

ModLoadPlan resolveModLoadPlan(const std::vector<ModManifest>& manifests) {
    ModLoadPlan plan;

    std::map<std::string, const ModManifest*> byId;
    for (const auto& m : manifests) {
        if (m.id.empty()) {
            plan.diagnostics.push_back(ModPlanDiagnostic{
                ModPlanErrorKind::InvalidManifest,
                {},
                {},
                {},
                "mod manifest has an empty id"});
            continue;
        }
        const auto [it, inserted] = byId.emplace(m.id, &m);
        if (!inserted) {
            plan.diagnostics.push_back(ModPlanDiagnostic{
                ModPlanErrorKind::DuplicateModId,
                m.id,
                {},
                {},
                "duplicate mod id '" + m.id + "'"});
        }
    }

    if (!plan.diagnostics.empty()) {
        plan.succeeded = false;
        return plan;
    }

    std::map<std::string, std::set<std::string>> dependentsOf;
    std::map<std::string, std::set<std::string>> dependenciesOf;
    std::map<std::string, std::size_t> inDegree;

    for (const auto& [id, mPtr] : byId) {
        inDegree[id] = 0;
        dependentsOf[id] = {};
        dependenciesOf[id] = {};
        (void)mPtr;
    }

    for (const auto& [id, mPtr] : byId) {
        for (const auto& dep : mPtr->dependencies) {
            if (!dep.constraint.valid) {
                plan.diagnostics.push_back(ModPlanDiagnostic{
                    ModPlanErrorKind::InvalidManifest,
                    id,
                    dep.modId,
                    {},
                    "mod '" + id + "' has invalid version constraint '" +
                        dep.constraint.rawExpression + "' for '" + dep.modId + "'"});
                continue;
            }
            const auto targetIt = byId.find(dep.modId);
            if (targetIt == byId.end()) {
                plan.diagnostics.push_back(ModPlanDiagnostic{
                    ModPlanErrorKind::MissingDependency,
                    id,
                    dep.modId,
                    {},
                    "mod '" + id + "' requires missing dependency '" + dep.modId + "'"});
                continue;
            }
            const SemVer actualVer = targetIt->second->version;
            if (!dep.constraint.satisfies(actualVer)) {
                plan.diagnostics.push_back(ModPlanDiagnostic{
                    ModPlanErrorKind::VersionIncompatible,
                    id,
                    dep.modId,
                    {},
                    "mod '" + id + "' requires '" + dep.modId + "' (" +
                        dep.constraint.rawExpression + "), but found version " +
                        actualVer.toString()});
                continue;
            }
            if (dependenciesOf[id].insert(dep.modId).second) {
                dependentsOf[dep.modId].insert(id);
                ++inDegree[id];
            }
        }
    }

    if (!plan.diagnostics.empty()) {
        plan.succeeded = false;
        return plan;
    }

    // Deterministic lexicographical Kahn's topological sort
    std::set<std::string> readyQueue;
    for (const auto& [id, deg] : inDegree) {
        if (deg == 0) {
            readyQueue.insert(id);
        }
    }

    plan.loadOrder.reserve(byId.size());
    while (!readyQueue.empty()) {
        const auto firstIt = readyQueue.begin();
        std::string current = *firstIt;
        readyQueue.erase(firstIt);
        plan.loadOrder.push_back(current);

        for (const auto& dependent : dependentsOf[current]) {
            auto& deg = inDegree[dependent];
            if (deg > 0 && --deg == 0) {
                readyQueue.insert(dependent);
            }
        }
    }

    if (plan.loadOrder.size() < byId.size()) {
        std::set<std::string> remaining;
        for (const auto& [id, deg] : inDegree) {
            if (deg > 0) remaining.insert(id);
        }

        std::map<std::string, int> color;
        std::vector<std::string> stack;
        std::vector<std::string> foundCycle;

        const std::function<bool(const std::string&)> dfs = [&](const std::string& u) -> bool {
            color[u] = 1;
            stack.push_back(u);
            for (const auto& dep : dependenciesOf[u]) {
                if (remaining.find(dep) == remaining.end()) continue;
                const int c = color[dep];
                if (c == 1) {
                    const auto startIt = std::find(stack.begin(), stack.end(), dep);
                    foundCycle.assign(startIt, stack.end());
                    foundCycle.push_back(dep);
                    return true;
                }
                if (c == 0 && dfs(dep)) {
                    return true;
                }
            }
            stack.pop_back();
            color[u] = 2;
            return false;
        };

        for (const auto& id : remaining) {
            if (color[id] == 0 && dfs(id)) {
                break;
            }
        }

        std::ostringstream msg;
        msg << "cyclic mod dependency detected: ";
        for (std::size_t i = 0; i < foundCycle.size(); ++i) {
            if (i != 0) msg << " -> ";
            msg << foundCycle[i];
        }
        plan.diagnostics.push_back(ModPlanDiagnostic{
            ModPlanErrorKind::DependencyCycle,
            foundCycle.empty() ? std::string{} : foundCycle.front(),
            foundCycle.size() > 1 ? foundCycle[1] : std::string{},
            foundCycle,
            msg.str()});
        plan.loadOrder.clear();
        plan.succeeded = false;
        return plan;
    }

    plan.succeeded = true;
    return plan;
}

// ============================================================================
// Cross-Heap Value Deep-Copy Marshaler
// ============================================================================

const char* marshalErrorCodeName(MarshalErrorCode code) noexcept {
    switch (code) {
    case MarshalErrorCode::None: return "none";
    case MarshalErrorCode::CrossHeapReferenceRejected: return "cross-heap-reference-rejected";
    case MarshalErrorCode::InvalidSourceReference: return "invalid-source-reference";
    case MarshalErrorCode::TargetOutOfMemory: return "target-out-of-memory";
    case MarshalErrorCode::TypeMismatch: return "type-mismatch";
    case MarshalErrorCode::DepthLimitExceeded: return "depth-limit-exceeded";
    }
    return "unknown";
}

namespace {

MarshalResult marshalCrossHeapImpl(
    const runtime::Value& value,
    const runtime::ManagedHeap* sourceHeap,
    runtime::ManagedHeap* targetHeap,
    std::uint32_t depth,
    std::uint32_t maxDepth) {
    if (depth > maxDepth) {
        return MarshalResult{
            false,
            MarshalErrorCode::DepthLimitExceeded,
            "cross-heap value nesting exceeded maximum depth limit",
            {}};
    }

    const auto tag = value.rawTag();
    switch (tag) {
    case runtime::ValueTag::Monostate:
    case runtime::ValueTag::NullString:
    case runtime::ValueTag::NullObject:
    case runtime::ValueTag::NullArray:
    case runtime::ValueTag::Bool:
    case runtime::ValueTag::Byte:
    case runtime::ValueTag::SByte:
    case runtime::ValueTag::Short:
    case runtime::ValueTag::UShort:
    case runtime::ValueTag::Int64:
    case runtime::ValueTag::UInt:
    case runtime::ValueTag::Long:
    case runtime::ValueTag::ULong:
    case runtime::ValueTag::Float:
    case runtime::ValueTag::Double:
    case runtime::ValueTag::Char:
    case runtime::ValueTag::Fixed:
    case runtime::ValueTag::Fixed16:
    case runtime::ValueTag::Fixed64:
    case runtime::ValueTag::Vec2:
    case runtime::ValueTag::Vec3:
    case runtime::ValueTag::Vec2f:
    case runtime::ValueTag::Vec3f:
        return MarshalResult{true, MarshalErrorCode::None, {}, value};

    case runtime::ValueTag::EnumInline:
    case runtime::ValueTag::EnumArena: {
        runtime::EnumValue ev{};
        if (!value.tryGetEnum(ev)) {
            return MarshalResult{
                false, MarshalErrorCode::TypeMismatch, "failed to decode EnumValue", {}};
        }
        return MarshalResult{true, MarshalErrorCode::None, {}, runtime::Value{ev}};
    }

    case runtime::ValueTag::QuatInline:
    case runtime::ValueTag::QuatArena: {
        runtime::QuatValue qv{};
        if (!value.tryGetQuat(qv)) {
            return MarshalResult{
                false, MarshalErrorCode::TypeMismatch, "failed to decode QuatValue", {}};
        }
        return MarshalResult{true, MarshalErrorCode::None, {}, runtime::Value{qv}};
    }

    case runtime::ValueTag::QuatfInline:
    case runtime::ValueTag::QuatfArena: {
        runtime::QuatfValue qfv{};
        if (!value.tryGetQuatf(qfv)) {
            return MarshalResult{
                false, MarshalErrorCode::TypeMismatch, "failed to decode QuatfValue", {}};
        }
        return MarshalResult{true, MarshalErrorCode::None, {}, runtime::Value{qfv}};
    }

    case runtime::ValueTag::NativeHandle:
        return MarshalResult{
            false,
            MarshalErrorCode::CrossHeapReferenceRejected,
            "NativeHandle cannot be marshaled across isolated Mod heaps",
            {}};

    case runtime::ValueTag::String: {
        const std::string copy = value.asStringUnchecked().str();
        if (targetHeap) {
            runtime::RuntimeError allocErr;
            const auto strRef = targetHeap->allocateString(copy, &allocErr);
            if (!strRef) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::TargetOutOfMemory,
                    allocErr.message.empty() ? "target heap OOM allocating string" : allocErr.message,
                    {}};
            }
            return MarshalResult{true, MarshalErrorCode::None, {}, runtime::Value{*strRef}};
        }
        return MarshalResult{true, MarshalErrorCode::None, {}, runtime::Value{copy}};
    }

    case runtime::ValueTag::Struct: {
        const auto& sv = value.asStructUnchecked();
        if (!sv.valid()) {
            return MarshalResult{
                false, MarshalErrorCode::TypeMismatch, "cannot marshal invalid StructValue", {}};
        }
        const std::size_t count = sv.fieldCount();
        std::vector<runtime::Value> marshaledFields;
        std::vector<runtime::PersistentRoot> fieldRoots;
        marshaledFields.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto fieldVal = runtime::structValueFieldAt(sv, i);
            if (!fieldVal) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::TypeMismatch,
                    "failed to read StructValue field during cross-heap marshal",
                    {}};
            }
            auto sub = marshalCrossHeapImpl(*fieldVal, sourceHeap, targetHeap, depth + 1u, maxDepth);
            if (!sub.succeeded) return sub;
            if (targetHeap && sub.value.isTraceable()) {
                fieldRoots.push_back(targetHeap->retain(sub.value));
            }
            marshaledFields.push_back(std::move(sub.value));
        }
        return MarshalResult{
            true,
            MarshalErrorCode::None,
            {},
            runtime::Value{runtime::makeStructValueFromFields(sv.typeId, std::move(marshaledFields))}};
    }

    case runtime::ValueTag::ObjectRef: {
        const auto& ref = value.asObjectRefUnchecked();
        if (ref.kind == runtime::ObjectKind::Record) {
            return MarshalResult{
                false,
                MarshalErrorCode::CrossHeapReferenceRejected,
                "Record ObjectRef cannot cross isolated Mod heaps (prevents cross-heap GC dangling pointers)",
                {}};
        }

        if (ref.kind == runtime::ObjectKind::String) {
            std::optional<std::string> copied;
            if (sourceHeap && ref.heapId == static_cast<std::uint32_t>(sourceHeap->heapId())) {
                if (const auto sv = sourceHeap->stringView(ref)) {
                    copied = std::string(*sv);
                }
            }
            if (!copied) {
                copied = runtime::ManagedHeap::tryCopyStringGlobal(ref);
            }
            if (!copied) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::InvalidSourceReference,
                    "source heap string ObjectRef is dead or invalid",
                    {}};
            }
            if (targetHeap) {
                runtime::RuntimeError allocErr;
                const auto dstStr = targetHeap->allocateString(std::move(*copied), &allocErr);
                if (!dstStr) {
                    return MarshalResult{
                        false,
                        MarshalErrorCode::TargetOutOfMemory,
                        allocErr.message.empty() ? "target heap OOM copying string" : allocErr.message,
                        {}};
                }
                return MarshalResult{true, MarshalErrorCode::None, {}, runtime::Value{*dstStr}};
            }
            return MarshalResult{true, MarshalErrorCode::None, {}, runtime::Value{std::move(*copied)}};
        }

        if (ref.kind == runtime::ObjectKind::Array) {
            if (!sourceHeap ||
                ref.heapId != static_cast<std::uint32_t>(sourceHeap->heapId()) ||
                !sourceHeap->isAlive(ref)) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::InvalidSourceReference,
                    "source array ObjectRef is dead or does not belong to sourceHeap",
                    {}};
            }
            if (sourceHeap->isRawColumnArray(ref)) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::CrossHeapReferenceRejected,
                    "raw native ECS column array view cannot cross isolated Mod heaps",
                    {}};
            }

            const auto elemType = sourceHeap->arrayElementType(ref).value_or(semantic::PrimitiveType::Error);
            const auto elemTypeId = sourceHeap->arrayElementTypeId(ref).value_or(0);
            const auto arrTypeId = sourceHeap->arrayTypeId(ref).value_or(0);
            if (elemType == semantic::PrimitiveType::Object ||
                elemType == semantic::PrimitiveType::Handle) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::CrossHeapReferenceRejected,
                    "object/handle reference array cannot cross isolated Mod heaps",
                    {}};
            }

            if (!targetHeap) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::InvalidSourceReference,
                    "marshaling an array requires a destination ManagedHeap",
                    {}};
            }

            const auto lenOpt = sourceHeap->arrayLength(ref);
            if (!lenOpt) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::InvalidSourceReference,
                    "failed to query source array length",
                    {}};
            }
            const std::size_t len = *lenOpt;

            runtime::RuntimeError allocErr;
            std::optional<runtime::ObjectRef> dstArr;
            if (elemType != semantic::PrimitiveType::Error || arrTypeId != 0) {
                dstArr = targetHeap->allocateTypedArray(
                    arrTypeId,
                    elemType,
                    elemTypeId,
                    len,
                    defaultElementForPrimitive(elemType, elemTypeId),
                    &allocErr);
            } else {
                dstArr = targetHeap->allocateArray(len, {}, &allocErr);
            }
            if (!dstArr) {
                return MarshalResult{
                    false,
                    MarshalErrorCode::TargetOutOfMemory,
                    allocErr.message.empty() ? "target heap OOM allocating array" : allocErr.message,
                    {}};
            }

            auto dstRoot = targetHeap->retain(*dstArr);
            for (std::size_t i = 0; i < len; ++i) {
                const auto srcElem = sourceHeap->arrayGet(ref, i);
                if (!srcElem) {
                    return MarshalResult{
                        false,
                        MarshalErrorCode::InvalidSourceReference,
                        "failed to read source array element",
                        {}};
                }
                auto marshaledElem = marshalCrossHeapImpl(
                    *srcElem, sourceHeap, targetHeap, depth + 1u, maxDepth);
                if (!marshaledElem.succeeded) return marshaledElem;

                runtime::RuntimeError setErr;
                if (!targetHeap->arraySet(*dstArr, i, std::move(marshaledElem.value), &setErr)) {
                    return MarshalResult{
                        false,
                        MarshalErrorCode::TypeMismatch,
                        setErr.message.empty() ? "failed to store marshaled array element" : setErr.message,
                        {}};
                }
            }
            return MarshalResult{true, MarshalErrorCode::None, {}, runtime::Value{*dstArr}};
        }
        break;
    }
    }

    return MarshalResult{
        false, MarshalErrorCode::TypeMismatch, "unsupported runtime ValueTag", {}};
}

} // namespace

MarshalResult marshalCrossHeapValue(
    const runtime::Value& value,
    const runtime::ManagedHeap* sourceHeap,
    runtime::ManagedHeap* targetHeap,
    std::uint32_t maxDepth) {
    std::optional<runtime::ScopedValuePoolDomain> targetDomainScope;
    if (targetHeap && targetHeap->valuePoolDomain()) {
        targetDomainScope.emplace(targetHeap->valuePoolDomain());
    }
    return marshalCrossHeapImpl(value, sourceHeap, targetHeap, 0u, maxDepth);
}

// ============================================================================
// CompilerExtensionPass Built-in Security Guards
// ============================================================================

CompilerExtensionPass makeForbiddenImportPass(
    std::unordered_set<std::string> forbiddenModules,
    std::string passName) {
    CompilerExtensionPass pass;
    pass.name = std::move(passName);
    pass.onAstParsed = [forbidden = std::move(forbiddenModules)](
                           const ModManifest& manifest,
                           const std::vector<ModAstUnit>& units,
                           diagnostics::DiagnosticBag& diagnostics) {
        for (const auto& u : units) {
            if (!u.syntaxTree) continue;
            for (const auto& imp : u.syntaxTree->imports) {
                const std::string importedName = imp.fullName();
                if (forbidden.find(importedName) != forbidden.end()) {
                    diagnostics.report(
                        "MOD1001",
                        "mod '" + manifest.id + "' is forbidden from importing restricted module '" +
                            importedName + "'",
                        imp.span(),
                        diagnostics::DiagnosticSeverity::Error,
                        u.sourcePath);
                }
            }
        }
    };
    return pass;
}

CompilerExtensionPass makeForbiddenSyntaxPass(
    std::unordered_set<syntax::SyntaxKind> forbiddenSyntaxKinds,
    std::string passName) {
    CompilerExtensionPass pass;
    pass.name = std::move(passName);
    pass.onAstParsed = [forbidden = std::move(forbiddenSyntaxKinds)](
                           const ModManifest& manifest,
                           const std::vector<ModAstUnit>& units,
                           diagnostics::DiagnosticBag& diagnostics) {
        for (const auto& u : units) {
            if (!u.syntaxTree) continue;
            walkCompilationUnitSyntax(
                *u.syntaxTree,
                [&](const syntax::SyntaxNode& node) {
                    if (forbidden.find(node.kind()) != forbidden.end()) {
                        diagnostics.report(
                            "MOD1002",
                            "mod '" + manifest.id + "' uses forbidden syntax construct '" +
                                std::string(syntax::syntaxKindName(node.kind())) + "'",
                            node.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            u.sourcePath);
                    }
                });
        }
    };
    return pass;
}

// ============================================================================
// Capability-Sandboxed ModHost Implementation
// ============================================================================

ModHost::ModHost(ModHostConfig config)
    : config_(std::move(config)),
      hostHeap_(std::make_shared<runtime::ManagedHeap>(runtime::HeapConfig{
          std::min<std::size_t>(config_.hostBusHeapMaxBytes / 2u, 64u * 1024u),
          config_.hostBusHeapMaxBytes})) {
    if (config_.faultThreshold == 0) {
        config_.faultThreshold = 1;
    }
}

ModHost::~ModHost() = default;

void ModHost::registerHostModule(
    std::string moduleName,
    compiler::SourceFile declarationStub,
    std::shared_ptr<const runtime::BindingRegistry> bindings) {
    for (auto& existing : hostModules_) {
        if (existing.moduleName == moduleName) {
            existing.stub = std::move(declarationStub);
            existing.bindings = std::move(bindings);
            return;
        }
    }
    hostModules_.push_back(HostAmbientModule{
        std::move(moduleName),
        std::move(declarationStub),
        std::move(bindings)});
}

void ModHost::addCompilerExtensionPass(CompilerExtensionPass pass) {
    compilerPasses_.push_back(std::move(pass));
}

ModLoadReport ModHost::loadMods(const std::vector<ModPackage>& packages) {
    unloadAll();

    ModLoadReport report;
    std::vector<ModManifest> manifests;
    manifests.reserve(packages.size());
    std::unordered_map<std::string, const ModPackage*> packageById;
    for (const auto& pkg : packages) {
        manifests.push_back(pkg.manifest);
        packageById.emplace(pkg.manifest.id, &pkg);
    }

    report.plan = resolveModLoadPlan(manifests);
    if (!report.plan.succeeded) {
        report.succeeded = false;
        return report;
    }

    std::unordered_map<std::string, std::vector<compiler::SourceFile>> modSourcesById;

    for (const auto& modId : report.plan.loadOrder) {
        const auto* pkg = packageById.at(modId);
        const auto& manifest = pkg->manifest;

        diagnostics::DiagnosticBag modDiagnostics;

        std::vector<ModAstUnit> astUnits;
        astUnits.reserve(pkg->sources.size());
        std::string inferredEntryModule = manifest.entryModule;
        for (const auto& src : pkg->sources) {
            auto srcText = std::make_shared<text::SourceText>(src.content, src.path);
            syntax::Parser parser(*srcText, modDiagnostics);
            auto unit = std::make_shared<syntax::CompilationUnitSyntax>(
                parser.parseCompilationUnit());
            ModAstUnit astUnit;
            astUnit.sourcePath = src.path;
            astUnit.sourceText = std::move(srcText);
            astUnit.moduleName = unit->moduleDeclaration
                ? unit->moduleDeclaration->fullName()
                : std::string{};
            if (inferredEntryModule.empty() && !astUnit.moduleName.empty()) {
                inferredEntryModule = astUnit.moduleName;
            }
            for (const auto& imp : unit->imports) {
                astUnit.importedModules.push_back(imp.fullName());
            }
            astUnit.syntaxTree = std::move(unit);
            astUnits.push_back(std::move(astUnit));
        }

        for (const auto& pass : compilerPasses_) {
            if (pass.onAstParsed) {
                pass.onAstParsed(manifest, astUnits, modDiagnostics);
            }
        }
        if (modDiagnostics.hasErrors()) {
            report.compileDiagnostics[modId] = std::move(modDiagnostics);
            report.succeeded = false;
            return report;
        }

        compiler::Compilation compilation;
        std::unordered_map<std::string, std::shared_ptr<const runtime::BindingRegistry>> hostBindingsByMod;
        for (const auto& hm : hostModules_) {
            compiler::SourceFile anchoredStub = hm.stub;
            anchoredStub.content += "\nvoid __rs_modhost_stub_anchor__() {}\n";
            compilation.addSource(std::move(anchoredStub));
            if (hm.bindings && !hm.moduleName.empty()) {
                hostBindingsByMod[hm.moduleName] = hm.bindings;
            }
        }
        bool hasReflectHostMod = false;
        for (const auto& hm : hostModules_) {
            if (hm.moduleName == "RealScript.Reflect") {
                hasReflectHostMod = true;
                break;
            }
        }
        if (config_.mountReflectModuleByDefault && !hasReflectHostMod) {
            compilation.addSource(reflection::exportReflectDeclarationStub());
        }

        std::set<std::string> visitedDeps;
        std::function<void(const ModManifest&)> collectDepSources = [&](const ModManifest& cur) {
            for (const auto& d : cur.dependencies) {
                if (!visitedDeps.insert(d.modId).second) continue;
                const auto* depPkg = packageById.at(d.modId);
                collectDepSources(depPkg->manifest);
                if (const auto srcIt = modSourcesById.find(d.modId); srcIt != modSourcesById.end()) {
                    for (const auto& depSrc : srcIt->second) {
                        compilation.addSource(depSrc);
                    }
                }
            }
        };
        collectDepSources(manifest);

        for (const auto& src : pkg->sources) {
            compilation.addSource(src);
        }

        auto build = compilation.build();
        for (const auto& d : build.diagnostics.items()) {
            modDiagnostics.report(d.code, d.message, d.span, d.severity, d.sourceName);
        }

        if (!modDiagnostics.hasErrors()) {
            for (const auto& pass : compilerPasses_) {
                if (pass.onSemanticBuilt) {
                    pass.onSemanticBuilt(manifest, build, modDiagnostics);
                }
            }
        }
        if (modDiagnostics.hasErrors()) {
            report.compileDiagnostics[modId] = std::move(modDiagnostics);
            report.succeeded = false;
            return report;
        }

        std::vector<mir::Module> mirModules;
        for (auto& m : build.modules) {
            if (const auto hbIt = hostBindingsByMod.find(m.name);
                hbIt != hostBindingsByMod.end() && hbIt->second) {
                const auto& hb = *hbIt->second;
                m.functions.erase(
                    std::remove_if(
                        m.functions.begin(),
                        m.functions.end(),
                        [&](const mir::Function& fn) {
                            bytecode::FunctionReference probe;
                            probe.symbolId = fn.symbolId;
                            probe.name = (fn.name.find("::") != std::string::npos)
                                ? fn.name
                                : (m.name + "::" + fn.name);
                            return hb.resolve(probe).has_value();
                        }),
                    m.functions.end());
            }
            mirModules.push_back(std::move(m));
        }
        if (config_.optimizationLevel != optimization::Level::None) {
            optimization::Options optOpts;
            optOpts.level = config_.optimizationLevel;
            auto optRes = optimization::Optimizer{}.optimize(
                std::move(mirModules), modDiagnostics, optOpts);
            if (modDiagnostics.hasErrors()) {
                report.compileDiagnostics[modId] = std::move(modDiagnostics);
                report.succeeded = false;
                return report;
            }
            mirModules = std::move(optRes.modules);
        }

        bytecode::Lowerer lowerer;
        std::vector<bytecode::Module> bcModules;
        bcModules.reserve(mirModules.size());
        for (const auto& mm : mirModules) {
            bcModules.push_back(lowerer.lower(mm));
        }

        runtime::RuntimeError linkErr;
        auto linkedImage = runtime::ProgramImage::link(std::move(bcModules), linkErr);
        if (!linkedImage) {
            report.linkErrors[modId] = linkErr.message;
            report.succeeded = false;
            return report;
        }

        auto sandbox = std::make_unique<SandboxInstance>();
        sandbox->manifest = manifest;
        if (sandbox->manifest.entryModule.empty()) {
            sandbox->manifest.entryModule = inferredEntryModule;
        }
        for (const auto& au : astUnits) {
            if (!au.moduleName.empty()) {
                sandbox->ownedModules.insert(au.moduleName);
            }
        }
        if (!sandbox->manifest.entryModule.empty()) {
            sandbox->ownedModules.insert(sandbox->manifest.entryModule);
        }
        sandbox->program = std::make_shared<const runtime::ProgramImage>(std::move(*linkedImage));

        auto modDomain = runtime::ValuePoolDomain::create();
        runtime::ScopedValuePoolDomain modDomainScope(modDomain);

        const std::size_t maxBytes = std::max<std::size_t>(manifest.maxHeapBytes, 1024u);
        const std::size_t initThreshold = std::max<std::size_t>(
            512u, std::min<std::size_t>(maxBytes / 2u, 64u * 1024u));
        sandbox->heap = std::make_shared<runtime::ManagedHeap>(
            runtime::HeapConfig{initThreshold, maxBytes, std::move(modDomain)});

        sandbox->reflection = std::make_shared<reflection::ReflectionRegistry>(
            sandbox->program, sandbox->heap);

        auto mergedBindings = std::make_shared<runtime::BindingRegistry>();
        if (config_.mountReflectModuleByDefault) {
            reflection::registerReflectBindings(
                *mergedBindings,
                sandbox->reflection,
                config_.reflectRequiredCapability,
                sandbox->heap);
        }

        for (const auto& mod : sandbox->program->modules()) {
            for (const auto& fref : mod.functionReferences) {
                for (const auto& hm : hostModules_) {
                    if (!hm.bindings) continue;
                    if (const auto resolved = hm.bindings->resolve(fref)) {
                        if (resolved->function && *resolved->function) {
                            (void)mergedBindings->bind(
                                fref.symbolId,
                                *resolved->function,
                                resolved->determinism,
                                resolved->requiredCapability);
                            (void)mergedBindings->bind(
                                fref.name,
                                *resolved->function,
                                resolved->determinism,
                                resolved->requiredCapability);
                            break;
                        }
                    }
                }
            }
        }

        sandbox->bindings = std::move(mergedBindings);
        sandbox->runtime = std::make_unique<runtime::EngineRuntime>(sandbox->program);
        sandbox->runtime->setHeap(sandbox->heap);
        sandbox->runtime->setBindings(sandbox->bindings);
        sandbox->state = ModState::Active;

        discoverAttributesAndEndpoints(*sandbox);

        modSourcesById[modId] = pkg->sources;
        loadOrder_.push_back(modId);
        report.loadedMods.push_back(modId);
        sandboxes_.emplace(modId, std::move(sandbox));
    }

    report.succeeded = true;
    return report;
}

void ModHost::discoverAttributesAndEndpoints(SandboxInstance& instance) {
    if (!instance.reflection || !instance.program) return;

    for (const auto& mf : instance.reflection->moduleFunctions()) {
        if (!instance.ownedModules.empty() &&
            instance.ownedModules.find(mf.moduleName) == instance.ownedModules.end()) {
            continue;
        }
        if (!instance.manifest.entryModule.empty() &&
            mf.moduleName == instance.manifest.entryModule) {
            instance.endpoints.try_emplace(mf.name, mf.symbolId);
        }
        instance.endpoints.try_emplace(mf.qualifiedName, mf.symbolId);

        for (const char* attrName : {"ModEndpoint", "Export", "Rpc"}) {
            if (const auto* attr = mf.findAttribute(attrName)) {
                const std::string alias = attr->getString("name")
                    .value_or(attr->getString("arg0").value_or(mf.name));
                instance.endpoints[alias] = mf.symbolId;
            }
        }
        for (const char* evAttr : {"EventHandler", "Subscribe", "OnEvent"}) {
            if (const auto* attr = mf.findAttribute(evAttr)) {
                const std::string topic = attr->getString("topic")
                    .value_or(attr->getString("arg0").value_or(mf.name));
                eventBus_[topic].push_back(EventSubscription{
                    instance.manifest.id,
                    mf.symbolId,
                    mf.qualifiedName,
                    mf.parameters.size()});
            }
        }
    }

    for (const auto& tr : instance.reflection->types()) {
        if (tr.isSynthetic) continue;
        if (!instance.ownedModules.empty() &&
            instance.ownedModules.find(tr.moduleName) == instance.ownedModules.end()) {
            continue;
        }
        for (const auto& m : tr.methods) {
            if (!m.isStatic) continue;
            if (!instance.manifest.entryModule.empty() &&
                tr.moduleName == instance.manifest.entryModule) {
                instance.endpoints.try_emplace(tr.name + "." + m.name, m.symbolId);
            }
            instance.endpoints.try_emplace(m.qualifiedName, m.symbolId);

            for (const char* attrName : {"ModEndpoint", "Export", "Rpc"}) {
                if (const auto* attr = m.findAttribute(attrName)) {
                    const std::string alias = attr->getString("name")
                        .value_or(attr->getString("arg0").value_or(m.name));
                    instance.endpoints[alias] = m.symbolId;
                }
            }
            for (const char* evAttr : {"EventHandler", "Subscribe", "OnEvent"}) {
                if (const auto* attr = m.findAttribute(evAttr)) {
                    const std::string topic = attr->getString("topic")
                        .value_or(attr->getString("arg0").value_or(m.name));
                    eventBus_[topic].push_back(EventSubscription{
                        instance.manifest.id,
                        m.symbolId,
                        m.qualifiedName,
                        m.parameters.size()});
                }
            }
        }
    }
}

bool ModHost::unloadMod(std::string_view modId) {
    const std::string key(modId);
    const auto it = sandboxes_.find(key);
    if (it == sandboxes_.end()) return false;

    sandboxes_.erase(it);
    loadOrder_.erase(
        std::remove(loadOrder_.begin(), loadOrder_.end(), key),
        loadOrder_.end());

    for (auto& [topic, subs] : eventBus_) {
        (void)topic;
        subs.erase(
            std::remove_if(
                subs.begin(),
                subs.end(),
                [&](const EventSubscription& s) { return s.modId == key; }),
            subs.end());
    }
    return true;
}

void ModHost::unloadAll() {
    sandboxes_.clear();
    loadOrder_.clear();
    eventBus_.clear();
}

ModHost::SandboxInstance* ModHost::findSandbox(std::string_view modId) noexcept {
    const auto it = sandboxes_.find(std::string(modId));
    return it == sandboxes_.end() ? nullptr : it->second.get();
}

const ModHost::SandboxInstance* ModHost::findSandbox(std::string_view modId) const noexcept {
    const auto it = sandboxes_.find(std::string(modId));
    return it == sandboxes_.end() ? nullptr : it->second.get();
}

bool ModHost::hasMod(std::string_view modId) const noexcept {
    return findSandbox(modId) != nullptr;
}

bool ModHost::isModActive(std::string_view modId) const noexcept {
    const auto* s = findSandbox(modId);
    return s && s->state == ModState::Active;
}

bool ModHost::isModTripped(std::string_view modId) const noexcept {
    const auto* s = findSandbox(modId);
    return s && s->state == ModState::Tripped;
}

std::optional<ModSandboxStats> ModHost::modStats(std::string_view modId) const {
    const auto* s = findSandbox(modId);
    if (!s) return std::nullopt;
    ModSandboxStats st;
    st.state = s->state;
    st.invocations = s->invocations;
    st.totalInstructionsExecuted = s->totalInstructions;
    st.faultCount = s->faultCount;
    st.consecutiveFaults = s->consecutiveFaults;
    st.liveHeapBytes = s->heap ? s->heap->liveBytes() : 0;
    st.peakHeapBytes = s->heap ? s->heap->statistics().peakLiveBytes : 0;
    st.maxHeapBytes = s->manifest.maxHeapBytes;
    st.lastError = s->lastError;
    return st;
}

const ModManifest* ModHost::modManifest(std::string_view modId) const noexcept {
    const auto* s = findSandbox(modId);
    return s ? &s->manifest : nullptr;
}

std::shared_ptr<runtime::ManagedHeap> ModHost::modHeap(std::string_view modId) const noexcept {
    const auto* s = findSandbox(modId);
    return s ? s->heap : nullptr;
}

std::shared_ptr<const reflection::ReflectionRegistry> ModHost::modReflection(
    std::string_view modId) const noexcept {
    const auto* s = findSandbox(modId);
    return s ? s->reflection : nullptr;
}

runtime::EngineRuntime* ModHost::modRuntime(std::string_view modId) noexcept {
    auto* s = findSandbox(modId);
    return s ? s->runtime.get() : nullptr;
}

const runtime::EngineRuntime* ModHost::modRuntime(std::string_view modId) const noexcept {
    const auto* s = findSandbox(modId);
    return s ? s->runtime.get() : nullptr;
}

bool ModHost::resetCircuitBreaker(std::string_view modId) {
    auto* s = findSandbox(modId);
    if (!s) return false;
    s->state = ModState::Active;
    s->consecutiveFaults = 0;
    s->lastError = runtime::RuntimeError{};
    if (s->heap) {
        s->heap->collect();
    }
    return true;
}

runtime::ExecutionOptions ModHost::makeSandboxExecutionOptions(
    const ModManifest& manifest) const {
    runtime::ExecutionOptions opts;
    opts.limits.instructionBudget = manifest.tickInstructionBudget;
    opts.enforceCapabilities = true;
    opts.grantedCapabilities.insert(
        manifest.grantedCapabilities.begin(),
        manifest.grantedCapabilities.end());
    opts.determinism.enforceCapabilities = true;
    opts.determinism.grantedCapabilities = opts.grantedCapabilities;
    return opts;
}

void ModHost::recordSandboxFailure(
    SandboxInstance& instance,
    const runtime::RuntimeError& err) noexcept {
    ++instance.faultCount;
    ++instance.consecutiveFaults;
    instance.lastError = err;
    if (instance.consecutiveFaults >= config_.faultThreshold) {
        instance.state = ModState::Tripped;
    }
}

bool ModHost::subscribeEvent(
    std::string_view modId,
    std::string eventTopic,
    std::string handlerQualifiedOrShortName) {
    auto* s = findSandbox(modId);
    if (!s || eventTopic.empty() || handlerQualifiedOrShortName.empty()) {
        return false;
    }

    semantic::SymbolId symId = 0;
    std::string resolvedName = handlerQualifiedOrShortName;
    if (const auto epIt = s->endpoints.find(handlerQualifiedOrShortName);
        epIt != s->endpoints.end()) {
        symId = epIt->second;
    } else if (!s->manifest.entryModule.empty()) {
        const std::string cand = s->manifest.entryModule + "::" + handlerQualifiedOrShortName;
        if (const auto found = s->program->findFunction(cand)) {
            symId = *found;
            resolvedName = cand;
        }
    }
    if (symId == 0) {
        if (const auto found = s->program->findFunction(handlerQualifiedOrShortName)) {
            symId = *found;
        }
    }
    if (symId == 0) return false;

    std::size_t arity = 0;
    if (const auto* loc = s->program->resolveFunction(symId); loc && loc->function) {
        arity = loc->function->parameterTypes.size();
    }

    auto& list = eventBus_[eventTopic];
    for (const auto& existing : list) {
        if (existing.modId == s->manifest.id && existing.symbolId == symId) {
            return true;
        }
    }
    list.push_back(EventSubscription{
        s->manifest.id, symId, std::move(resolvedName), arity});
    return true;
}

EventPublishReport ModHost::publishEvent(
    std::string_view eventTopic,
    const std::vector<runtime::Value>& payload,
    const runtime::ManagedHeap* sourceHeap) {
    EventPublishReport report;

    for (const auto& arg : payload) {
        const auto check = marshalCrossHeapValue(
            arg, sourceHeap ? sourceHeap : hostHeap_.get(), hostHeap_.get());
        if (!check.succeeded &&
            check.errorCode == MarshalErrorCode::CrossHeapReferenceRejected) {
            report.succeeded = false;
            report.marshalError = MarshalErrorCode::CrossHeapReferenceRejected;
            report.errorMessage = check.errorMessage;
            return report;
        }
    }

    const auto busIt = eventBus_.find(std::string(eventTopic));
    if (busIt == eventBus_.end() || busIt->second.empty()) {
        return report;
    }

    std::unordered_map<std::string, std::size_t> orderRank;
    for (std::size_t i = 0; i < loadOrder_.size(); ++i) {
        orderRank.emplace(loadOrder_[i], i);
    }
    std::vector<EventSubscription> orderedSubs = busIt->second;
    std::stable_sort(
        orderedSubs.begin(),
        orderedSubs.end(),
        [&](const EventSubscription& a, const EventSubscription& b) {
            const auto ra = orderRank.find(a.modId);
            const auto rb = orderRank.find(b.modId);
            const std::size_t ia = ra == orderRank.end() ? loadOrder_.size() : ra->second;
            const std::size_t ib = rb == orderRank.end() ? loadOrder_.size() : rb->second;
            return ia < ib;
        });

    const runtime::ManagedHeap* effectiveSourceHeap = sourceHeap ? sourceHeap : hostHeap_.get();

    for (const auto& sub : orderedSubs) {
        auto* sandbox = findSandbox(sub.modId);
        if (!sandbox) continue;

        EventSubscriberOutcome outcome;
        outcome.modId = sub.modId;
        outcome.handlerName = sub.qualifiedName;

        if (sandbox->state == ModState::Tripped) {
            outcome.succeeded = false;
            outcome.skippedBecauseTripped = true;
            outcome.status = BusCallStatus::ModTripped;
            outcome.error = sandbox->lastError;
            ++report.skippedTrippedCount;
            report.outcomes.push_back(std::move(outcome));
            continue;
        }

        if (sub.arity != payload.size()) {
            outcome.succeeded = false;
            outcome.status = BusCallStatus::MarshalFailed;
            outcome.error.code = runtime::ErrorCode::InvalidArguments;
            outcome.error.message = "event handler '" + sub.qualifiedName + "' expects " +
                std::to_string(sub.arity) + " argument(s), got " + std::to_string(payload.size());
            ++report.failedCount;
            report.outcomes.push_back(std::move(outcome));
            continue;
        }

        std::optional<runtime::ScopedValuePoolDomain> modDomainScope;
        if (sandbox->heap && sandbox->heap->valuePoolDomain()) {
            modDomainScope.emplace(sandbox->heap->valuePoolDomain());
        }

        std::vector<runtime::Value> marshaledArgs;
        std::vector<runtime::PersistentRoot> argRoots;
        marshaledArgs.reserve(payload.size());
        bool marshalOk = true;

        for (const auto& item : payload) {
            auto m = marshalCrossHeapValue(item, effectiveSourceHeap, sandbox->heap.get());
            if (!m.succeeded) {
                marshalOk = false;
                outcome.succeeded = false;
                outcome.status = m.errorCode == MarshalErrorCode::CrossHeapReferenceRejected
                    ? BusCallStatus::CrossHeapReferenceRejected
                    : BusCallStatus::MarshalFailed;
                outcome.error.code = m.errorCode == MarshalErrorCode::TargetOutOfMemory
                    ? runtime::ErrorCode::OutOfMemory
                    : runtime::ErrorCode::TypeMismatch;
                outcome.error.message = m.errorMessage;
                if (m.errorCode == MarshalErrorCode::TargetOutOfMemory) {
                    recordSandboxFailure(*sandbox, outcome.error);
                } else {
                    report.marshalError = m.errorCode;
                }
                ++report.failedCount;
                break;
            }
            if (m.value.isTraceable()) {
                argRoots.push_back(sandbox->heap->retain(m.value));
            }
            marshaledArgs.push_back(std::move(m.value));
        }

        if (!marshalOk) {
            report.outcomes.push_back(std::move(outcome));
            continue;
        }

        ++sandbox->invocations;
        auto execRes = sandbox->runtime->invoke(
            sub.symbolId,
            marshaledArgs,
            makeSandboxExecutionOptions(sandbox->manifest));
        sandbox->totalInstructions += execRes.instructionsExecuted;
        outcome.instructionsExecuted = execRes.instructionsExecuted;

        if (!execRes.succeeded) {
            recordSandboxFailure(*sandbox, execRes.error);
            outcome.succeeded = false;
            outcome.status = BusCallStatus::RuntimeFault;
            outcome.error = std::move(execRes.error);
            ++report.failedCount;
        } else {
            sandbox->consecutiveFaults = 0;
            outcome.succeeded = true;
            outcome.status = BusCallStatus::Succeeded;
            ++report.deliveredCount;
        }
        report.outcomes.push_back(std::move(outcome));
    }

    return report;
}

ModEndpointResult ModHost::invokeModEndpoint(
    std::string_view targetModId,
    std::string_view endpointName,
    const std::vector<runtime::Value>& arguments,
    const runtime::ManagedHeap* callerHeap,
    runtime::ManagedHeap* resultTargetHeap) {
    ModEndpointResult out;
    auto* sandbox = findSandbox(targetModId);
    if (!sandbox) {
        out.succeeded = false;
        out.status = BusCallStatus::ModNotFound;
        out.message = "target mod '" + std::string(targetModId) + "' is not loaded";
        return out;
    }

    if (sandbox->state == ModState::Tripped) {
        out.succeeded = false;
        out.status = BusCallStatus::ModTripped;
        out.runtimeError = sandbox->lastError;
        out.modTrippedAfterCall = true;
        out.message = "target mod '" + sandbox->manifest.id + "' is isolated by circuit breaker";
        return out;
    }

    semantic::SymbolId targetSym = 0;
    const std::string epKey(endpointName);
    if (const auto it = sandbox->endpoints.find(epKey); it != sandbox->endpoints.end()) {
        targetSym = it->second;
    } else if (!sandbox->manifest.entryModule.empty()) {
        if (const auto found = sandbox->program->findFunction(
                sandbox->manifest.entryModule + "::" + epKey)) {
            targetSym = *found;
        }
    }
    if (targetSym == 0) {
        if (const auto found = sandbox->program->findFunction(epKey)) {
            targetSym = *found;
        }
    }
    if (targetSym == 0) {
        out.succeeded = false;
        out.status = BusCallStatus::EndpointNotFound;
        out.message = "endpoint '" + epKey + "' was not found in mod '" + sandbox->manifest.id + "'";
        return out;
    }

    const runtime::ManagedHeap* effectiveCallerHeap = callerHeap ? callerHeap : hostHeap_.get();
    runtime::ManagedHeap* effectiveReturnHeap = resultTargetHeap ? resultTargetHeap : hostHeap_.get();

    std::optional<runtime::ScopedValuePoolDomain> modDomainScope;
    if (sandbox->heap && sandbox->heap->valuePoolDomain()) {
        modDomainScope.emplace(sandbox->heap->valuePoolDomain());
    }

    std::vector<runtime::Value> marshaledArgs;
    std::vector<runtime::PersistentRoot> argRoots;
    marshaledArgs.reserve(arguments.size());
    for (const auto& arg : arguments) {
        auto m = marshalCrossHeapValue(arg, effectiveCallerHeap, sandbox->heap.get());
        if (!m.succeeded) {
            out.succeeded = false;
            out.marshalError = m.errorCode;
            out.status = m.errorCode == MarshalErrorCode::CrossHeapReferenceRejected
                ? BusCallStatus::CrossHeapReferenceRejected
                : BusCallStatus::MarshalFailed;
            out.message = m.errorMessage;
            if (m.errorCode == MarshalErrorCode::TargetOutOfMemory) {
                runtime::RuntimeError oomErr;
                oomErr.code = runtime::ErrorCode::OutOfMemory;
                oomErr.message = m.errorMessage;
                recordSandboxFailure(*sandbox, oomErr);
                out.runtimeError = std::move(oomErr);
                out.modTrippedAfterCall = (sandbox->state == ModState::Tripped);
            }
            return out;
        }
        if (m.value.isTraceable()) {
            argRoots.push_back(sandbox->heap->retain(m.value));
        }
        marshaledArgs.push_back(std::move(m.value));
    }

    ++sandbox->invocations;
    auto execRes = sandbox->runtime->invoke(
        targetSym,
        marshaledArgs,
        makeSandboxExecutionOptions(sandbox->manifest));
    sandbox->totalInstructions += execRes.instructionsExecuted;
    out.instructionsExecuted = execRes.instructionsExecuted;

    if (!execRes.succeeded) {
        recordSandboxFailure(*sandbox, execRes.error);
        out.succeeded = false;
        out.status = BusCallStatus::RuntimeFault;
        out.runtimeError = std::move(execRes.error);
        out.modTrippedAfterCall = (sandbox->state == ModState::Tripped);
        out.message = out.runtimeError.message;
        return out;
    }

    sandbox->consecutiveFaults = 0;

    runtime::PersistentRoot retTempRoot;
    if (execRes.value.isTraceable()) {
        retTempRoot = sandbox->heap->retain(execRes.value);
    }
    auto retMarshal = marshalCrossHeapValue(
        execRes.value,
        sandbox->heap.get(),
        effectiveReturnHeap);
    if (!retMarshal.succeeded) {
        out.succeeded = false;
        out.marshalError = retMarshal.errorCode;
        out.status = retMarshal.errorCode == MarshalErrorCode::CrossHeapReferenceRejected
            ? BusCallStatus::CrossHeapReferenceRejected
            : BusCallStatus::MarshalFailed;
        out.message = retMarshal.errorMessage;
        return out;
    }

    out.succeeded = true;
    out.status = BusCallStatus::Succeeded;
    out.value = std::move(retMarshal.value);
    return out;
}

} // namespace realscript::plugin
