#include "CompilationInternal.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::compiler {

using namespace detail;

std::uint64_t stableFingerprint(const std::string& value) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}


struct CompilerSession::Impl {
    mutable std::mutex parsedUnitCacheMutex;
    std::unordered_map<std::uint64_t, ParsedUnit> parsedUnitCache;
    mutable std::mutex moduleFrontendCacheMutex;
    std::unordered_map<std::string, CachedModuleFrontend> moduleFrontendCache;
};

CompilerSession::CompilerSession() : impl_(std::make_unique<Impl>()) {}
CompilerSession::~CompilerSession() = default;

void CompilerSession::clearCaches() {
    {
        std::lock_guard<std::mutex> lock(impl_->parsedUnitCacheMutex);
        impl_->parsedUnitCache.clear();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->moduleFrontendCacheMutex);
        impl_->moduleFrontendCache.clear();
    }
}

std::size_t CompilerSession::parsedUnitCacheSize() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->parsedUnitCacheMutex);
    return impl_->parsedUnitCache.size();
}

std::size_t CompilerSession::moduleFrontendCacheSize() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->moduleFrontendCacheMutex);
    return impl_->moduleFrontendCache.size();
}

void ModulePackRegistry::registerPack(ModulePack pack) {
    for (auto& existing : packs_) {
        if (existing.moduleName == pack.moduleName) {
            existing = std::move(pack);
            return;
        }
    }
    packs_.push_back(std::move(pack));
}

bool ModulePackRegistry::unregisterPack(std::string_view moduleName) {
    const auto before = packs_.size();
    packs_.erase(
        std::remove_if(
            packs_.begin(),
            packs_.end(),
            [&](const ModulePack& p) { return p.moduleName == moduleName; }),
        packs_.end());
    return packs_.size() != before;
}

const ModulePack* ModulePackRegistry::findPack(std::string_view moduleName) const noexcept {
    for (const auto& p : packs_) {
        if (p.moduleName == moduleName) return &p;
    }
    return nullptr;
}

std::uint64_t ModulePackRegistry::fingerprint() const noexcept {
    std::uint64_t fp = 14695981039346656037ull;
    for (const auto& p : packs_) {
        fp = combineFingerprint(fp, stableFingerprint(p.moduleName));
    }
    return fp;
}

const ModulePackRegistry& ModulePackRegistry::builtinDefaults() {
    static const ModulePackRegistry kDefaults = []() {
        ModulePackRegistry reg;
        reg.registerPack({
            "RealScript.Math",
            "$builtin/RealScript.Math.rscript",
            &builtinDeterministicMathModuleSource,
            nullptr,
            false});
        reg.registerPack({
            "RealScript.Fiber",
            "$builtin/RealScript.Fiber.rscript",
            &builtinFiberModuleSource,
            nullptr,
            false});
        reg.registerPack({
            "RealScript.Async",
            "$builtin/RealScript.Async.rscript",
            &builtinAsyncModuleSource,
            &sourceMentionsAsyncPrimitives,
            true});
        reg.registerPack({
            "RealScript.Resource",
            "$builtin/RealScript.Resource.rscript",
            &builtinResourceModuleSource,
            &sourceMentionsResourcePrimitives,
            true});
        reg.registerPack({
            "RealScript.Weak",
            "$builtin/RealScript.Weak.rscript",
            &builtinWeakModuleSource,
            nullptr,
            false});
        reg.registerPack({
            "RealScript.Memory",
            "$builtin/RealScript.Memory.rscript",
            &builtinMemoryModuleSource,
            nullptr,
            false});
        return reg;
    }();
    return kDefaults;
}

namespace {

std::string formatTypeSyntaxKey(const syntax::TypeSyntax& type) {
    std::string out = type.name.text;
    if (!type.typeArguments.empty() || type.isGeneric()) {
        out.push_back('<');
        for (std::size_t i = 0; i < type.typeArguments.size(); ++i) {
            if (i > 0) out.push_back(',');
            out += formatTypeSyntaxKey(type.typeArguments[i]);
        }
        out.push_back('>');
    }
    if (type.isNullable()) out.push_back('?');
    if (type.isArray()) out += "[]";
    return out;
}

std::string partialMethodSignatureKey(const syntax::FunctionDeclarationSyntax& fn) {
    std::ostringstream oss;
    const bool isStat =
        fn.staticKeyword.has_value() ||
        semantic::hasModifier(fn.modifiers, syntax::SyntaxKind::StaticKeyword);
    oss << (isStat ? "static:" : "inst:") << fn.identifierToken.text << "`"
        << fn.typeParameters.size() << "/" << fn.parameters.size() << ":";
    for (const auto& p : fn.parameters) {
        if (p.modifierToken) oss << p.modifierToken->text << "&";
        oss << formatTypeSyntaxKey(p.type) << ",";
    }
    return oss.str();
}

std::string normalizedPartialMethodAccess(const syntax::FunctionDeclarationSyntax& fn) {
    if (semantic::hasModifier(fn.modifiers, syntax::SyntaxKind::PublicKeyword)) return "public";
    if (semantic::hasModifier(fn.modifiers, syntax::SyntaxKind::ProtectedKeyword)) return "protected";
    if (semantic::hasModifier(fn.modifiers, syntax::SyntaxKind::InternalKeyword)) return "internal";
    return "private";
}

bool partialMethodModifiersMatch(
    const syntax::FunctionDeclarationSyntax& lhs,
    const syntax::FunctionDeclarationSyntax& rhs) {
    if (normalizedPartialMethodAccess(lhs) != normalizedPartialMethodAccess(rhs)) {
        return false;
    }
    static constexpr syntax::SyntaxKind kCheckedModifiers[] = {
        syntax::SyntaxKind::StaticKeyword,
        syntax::SyntaxKind::AsyncKeyword,
        syntax::SyntaxKind::AbstractKeyword,
        syntax::SyntaxKind::VirtualKeyword,
        syntax::SyntaxKind::OverrideKeyword,
        syntax::SyntaxKind::SealedKeyword,
    };
    for (const auto kind : kCheckedModifiers) {
        if (semantic::hasModifier(lhs.modifiers, kind) !=
            semantic::hasModifier(rhs.modifiers, kind)) {
            return false;
        }
    }
    return true;
}

struct ConstraintClauseItem {
    std::string key;
    std::vector<syntax::SyntaxToken> tokens;
};

std::vector<ConstraintClauseItem> splitConstraintClauseItems(
    const std::vector<syntax::SyntaxToken>& tokens) {
    std::vector<ConstraintClauseItem> items;
    std::vector<syntax::SyntaxToken> currentTokens;
    std::string currentKey;
    int angleDepth = 0;
    auto flush = [&]() {
        if (!currentTokens.empty()) {
            items.push_back(ConstraintClauseItem{std::move(currentKey), std::move(currentTokens)});
            currentKey.clear();
            currentTokens.clear();
        }
    };
    for (const auto& token : tokens) {
        if (token.kind == syntax::SyntaxKind::LessToken) ++angleDepth;
        if (token.kind == syntax::SyntaxKind::GreaterToken && angleDepth > 0) --angleDepth;
        if (token.kind == syntax::SyntaxKind::CommaToken && angleDepth == 0) {
            flush();
        } else {
            currentKey += token.text;
            currentTokens.push_back(token);
        }
    }
    flush();
    return items;
}

void reconcilePartialMethods(
    std::vector<syntax::FunctionDeclarationSyntax>& methods,
    const std::string& fallbackSourceName,
    diagnostics::DiagnosticBag& diagnostics,
    bool& moduleInvalid) {
    auto sourceOf = [&](const syntax::FunctionDeclarationSyntax& fn) -> const std::string& {
        return fn.originSourceName.empty() ? fallbackSourceName : fn.originSourceName;
    };

    std::unordered_map<std::string, std::size_t> declIndexByKey;
    std::unordered_map<std::string, std::size_t> implIndexByKey;
    std::unordered_set<std::size_t> skipIndices;
    for (std::size_t idx = 0; idx < methods.size(); ++idx) {
        const auto& m = methods[idx];
        if (!semantic::hasModifier(m.modifiers, syntax::SyntaxKind::PartialKeyword)) continue;
        const auto key = partialMethodSignatureKey(m);
        if (m.semicolonToken.has_value()) {
            if (!declIndexByKey.emplace(key, idx).second) {
                diagnostics.report(
                    "RS4023",
                    "duplicate defining declaration of partial method '" +
                        m.identifierToken.text + "'",
                    m.identifierToken.span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceOf(m));
                moduleInvalid = true;
                skipIndices.insert(idx);
            }
        } else {
            if (!implIndexByKey.emplace(key, idx).second) {
                diagnostics.report(
                    "RS4024",
                    "duplicate implementation declaration of partial method '" +
                        m.identifierToken.text + "'",
                    m.identifierToken.span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceOf(m));
                moduleInvalid = true;
                skipIndices.insert(idx);
            }
        }
    }

    for (const auto& [key, dIdx] : declIndexByKey) {
        const auto implIt = implIndexByKey.find(key);
        if (implIt != implIndexByKey.end()) {
            auto& implM = methods[implIt->second];
            const auto& declM = methods[dIdx];
            if (formatTypeSyntaxKey(declM.returnType) != formatTypeSyntaxKey(implM.returnType) ||
                declM.refReturnKeyword.has_value() != implM.refReturnKeyword.has_value()) {
                diagnostics.report(
                    "RS4022",
                    "partial method '" + declM.identifierToken.text +
                        "' declarations have mismatched return types",
                    implM.identifierToken.span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceOf(implM));
                moduleInvalid = true;
            }
            if (!partialMethodModifiersMatch(declM, implM)) {
                diagnostics.report(
                    "RS4022",
                    "partial method '" + declM.identifierToken.text +
                        "' declarations have mismatched modifiers",
                    implM.identifierToken.span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceOf(implM));
                moduleInvalid = true;
            }
            const std::string& declSrc = sourceOf(declM);
            for (auto a : declM.attributes) {
                if (a.originSourceName.empty()) a.originSourceName = declSrc;
                implM.attributes.push_back(std::move(a));
            }
            skipIndices.insert(dIdx);
        } else {
            // Unimplemented partial method: allowed if void and has no 'out' parameters -> synthesize empty body!
            auto& declM = methods[dIdx];
            bool hasOut = false;
            for (const auto& p : declM.parameters) {
                if (p.modifierToken && p.modifierToken->kind == syntax::SyntaxKind::OutKeyword) {
                    hasOut = true;
                    break;
                }
            }
            if (declM.returnType.name.kind == syntax::SyntaxKind::VoidKeyword &&
                !declM.refReturnKeyword.has_value() && !hasOut) {
                declM.semicolonToken.reset();
                declM.body.openBraceToken = syntax::SyntaxToken{
                    syntax::SyntaxKind::OpenBraceToken, declM.closeParenToken.span, "{", {}, true};
                declM.body.closeBraceToken = syntax::SyntaxToken{
                    syntax::SyntaxKind::CloseBraceToken, declM.closeParenToken.span, "}", {}, true};
            } else {
                diagnostics.report(
                    "RS4020",
                    "partial method '" + declM.identifierToken.text +
                        "' must have an implementation part because it returns a value or has 'out' parameters",
                    declM.identifierToken.span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceOf(declM));
                moduleInvalid = true;
            }
        }
    }

    if (!skipIndices.empty()) {
        std::vector<syntax::FunctionDeclarationSyntax> reconciled;
        reconciled.reserve(methods.size() - skipIndices.size());
        for (std::size_t idx = 0; idx < methods.size(); ++idx) {
            if (skipIndices.find(idx) == skipIndices.end()) {
                reconciled.push_back(std::move(methods[idx]));
            }
        }
        methods = std::move(reconciled);
    }
}

template <typename DeclSyntax>
void mergePartialDeclGroup(
    std::vector<DeclSyntax*> parts,
    diagnostics::DiagnosticBag& diagnostics,
    const std::vector<std::string>& partSourceNames,
    bool& moduleInvalid) {
    if (parts.size() <= 1) return;
    bool allPartial = true;
    bool anyPartial = false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (semantic::hasModifier(parts[i]->modifiers, syntax::SyntaxKind::PartialKeyword)) {
            anyPartial = true;
        } else {
            allPartial = false;
        }
    }
    if (!allPartial) {
        if (anyPartial) {
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (!semantic::hasModifier(parts[i]->modifiers, syntax::SyntaxKind::PartialKeyword)) {
                    diagnostics.report(
                        "RS4004",
                        "missing 'partial' modifier on declaration of type '" +
                            parts[i]->identifierToken.text +
                            "' (all declarations must specify 'partial')",
                        parts[i]->identifierToken.span,
                        diagnostics::DiagnosticSeverity::Error,
                        partSourceNames[i]);
                }
            }
        } else {
            for (std::size_t i = 1; i < parts.size(); ++i) {
                diagnostics.report(
                    "RS4004",
                    "duplicate type '" + parts[i]->identifierToken.text +
                        "' (add 'partial' modifier to all declarations to merge partial parts)",
                    parts[i]->identifierToken.span,
                    diagnostics::DiagnosticSeverity::Error,
                    partSourceNames[i]);
            }
        }
        moduleInvalid = true;
        return;
    }

    auto stampAttrList = [](std::vector<syntax::AttributeListSyntax>& lists, const std::string& src) {
        for (auto& list : lists) {
            if (list.originSourceName.empty()) list.originSourceName = src;
        }
    };
    auto stampMembers = [&](DeclSyntax& decl, const std::string& src) {
        stampAttrList(decl.attributes, src);
        for (auto& f : decl.fields) {
            if (f.originSourceName.empty()) f.originSourceName = src;
            stampAttrList(f.attributes, src);
        }
        for (auto& p : decl.properties) {
            if (p.originSourceName.empty()) p.originSourceName = src;
            stampAttrList(p.attributes, src);
        }
        for (auto& e : decl.events) {
            if (e.originSourceName.empty()) e.originSourceName = src;
            stampAttrList(e.attributes, src);
        }
        for (auto& c : decl.constructors) {
            if (c.originSourceName.empty()) c.originSourceName = src;
            stampAttrList(c.attributes, src);
        }
        for (auto& s : decl.sequences) {
            if (s.originSourceName.empty()) s.originSourceName = src;
            stampAttrList(s.attributes, src);
        }
        for (auto& m : decl.methods) {
            if (m.originSourceName.empty()) m.originSourceName = src;
            stampAttrList(m.attributes, src);
        }
    };
    for (std::size_t i = 0; i < parts.size(); ++i) {
        stampMembers(*parts[i], partSourceNames[i]);
    }

    DeclSyntax& primary = *parts[0];
    for (std::size_t i = 1; i < parts.size(); ++i) {
        DeclSyntax& extra = *parts[i];
        for (std::size_t tp = 0; tp < primary.typeParameters.size() && tp < extra.typeParameters.size(); ++tp) {
            if (primary.typeParameters[tp].text != extra.typeParameters[tp].text) {
                diagnostics.report(
                    "RS4021",
                    "partial declarations of '" + primary.identifierToken.text +
                        "' must use the same generic type parameter names (expected '" +
                        primary.typeParameters[tp].text + "', got '" +
                        extra.typeParameters[tp].text + "')",
                    extra.typeParameters[tp].span,
                    diagnostics::DiagnosticSeverity::Error,
                    partSourceNames[i]);
                moduleInvalid = true;
            }
        }
        for (auto& attrList : extra.attributes) {
            primary.attributes.push_back(std::move(attrList));
        }
        for (const auto& mod : extra.modifiers) {
            if (!semantic::hasModifier(primary.modifiers, mod.kind)) {
                primary.modifiers.push_back(mod);
            }
        }
        for (const auto& iface : extra.interfaces) {
            bool exists = false;
            for (const auto& exIface : primary.interfaces) {
                if (formatTypeSyntaxKey(exIface) == formatTypeSyntaxKey(iface)) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                primary.interfaces.push_back(iface);
            }
        }
        for (const auto& clause : extra.constraints) {
            syntax::GenericConstraintClauseSyntax* existingClause = nullptr;
            for (auto& candidate : primary.constraints) {
                if (candidate.typeParameter.text == clause.typeParameter.text) {
                    existingClause = &candidate;
                    break;
                }
            }
            if (!existingClause) {
                primary.constraints.push_back(clause);
                continue;
            }
            const auto existingItems = splitConstraintClauseItems(existingClause->constraints);
            const auto incomingItems = splitConstraintClauseItems(clause.constraints);
            auto containsKey = [](const std::vector<ConstraintClauseItem>& items, const std::string& key) {
                for (const auto& item : items) {
                    if (item.key == key) return true;
                }
                return false;
            };
            const bool existingHasClass = containsKey(existingItems, "class");
            const bool existingHasStruct = containsKey(existingItems, "struct");
            const bool incomingHasClass = containsKey(incomingItems, "class");
            const bool incomingHasStruct = containsKey(incomingItems, "struct");
            if ((existingHasClass && incomingHasStruct) || (existingHasStruct && incomingHasClass)) {
                diagnostics.report(
                    "RS4022",
                    "conflicting generic primary constraints for type parameter '" +
                        clause.typeParameter.text + "' across partial declarations of '" +
                        primary.identifierToken.text + "'",
                    clause.typeParameter.span,
                    diagnostics::DiagnosticSeverity::Error,
                    partSourceNames[i]);
                moduleInvalid = true;
                continue;
            }
            for (const auto& item : incomingItems) {
                if (containsKey(existingItems, item.key)) continue;
                if (!existingClause->constraints.empty()) {
                    existingClause->constraints.push_back(syntax::SyntaxToken{
                        syntax::SyntaxKind::CommaToken, clause.colonToken.span, ",", {}, true});
                }
                for (const auto& token : item.tokens) {
                    existingClause->constraints.push_back(token);
                }
            }
        }
        for (auto& f : extra.fields) primary.fields.push_back(std::move(f));
        for (auto& p : extra.properties) primary.properties.push_back(std::move(p));
        for (auto& e : extra.events) primary.events.push_back(std::move(e));
        for (auto& c : extra.constructors) primary.constructors.push_back(std::move(c));
        for (auto& s : extra.sequences) primary.sequences.push_back(std::move(s));
        for (auto& m : extra.methods) primary.methods.push_back(std::move(m));
    }

    reconcilePartialMethods(primary.methods, partSourceNames[0], diagnostics, moduleInvalid);
}

template <typename DeclSyntax>
void mergeRecordGroupsForUnits(
    const std::vector<detail::PartialUnitEntry>& units,
    std::vector<DeclSyntax> syntax::CompilationUnitSyntax::* vecMember,
    std::string_view kindPrefix,
    diagnostics::DiagnosticBag& diagnostics,
    bool& moduleInvalid,
    std::unordered_map<std::string, detail::MergedPartialTypeOriginInfo>* outMultiPartRecords) {
    struct RecordGroup {
        std::vector<DeclSyntax*> parts;
        std::vector<std::pair<syntax::CompilationUnitSyntax*, std::size_t>> locations;
        std::vector<std::string> sourceNames;
    };

    std::map<std::string, RecordGroup> groups;
    std::vector<std::string> order;
    for (const auto& u : units) {
        if (!u.syntaxTree) continue;
        auto& vec = u.syntaxTree->*vecMember;
        for (std::size_t idx = 0; idx < vec.size(); ++idx) {
            auto& decl = vec[idx];
            const std::string key =
                decl.identifierToken.text + "#" + std::to_string(decl.typeParameters.size());
            if (groups.find(key) == groups.end()) order.push_back(key);
            auto& g = groups[key];
            g.parts.push_back(&decl);
            g.locations.emplace_back(u.syntaxTree, idx);
            g.sourceNames.push_back(u.sourceName);
        }
    }

    std::unordered_map<syntax::CompilationUnitSyntax*, std::unordered_set<std::size_t>> eraseIndices;
    for (const auto& key : order) {
        auto& g = groups[key];
        if (g.parts.size() > 1) {
            if (outMultiPartRecords) {
                auto& info = (*outMultiPartRecords)[std::string(kindPrefix) + key];
                for (std::size_t i = 0; i < g.parts.size(); ++i) {
                    info.parts.push_back(detail::MergedGenericPartialPart{
                        g.sourceNames[i], g.parts[i]->span()});
                }
            }
            mergePartialDeclGroup(g.parts, diagnostics, g.sourceNames, moduleInvalid);
            for (std::size_t i = 1; i < g.locations.size(); ++i) {
                eraseIndices[g.locations[i].first].insert(g.locations[i].second);
            }
        } else if (g.parts.size() == 1) {
            reconcilePartialMethods(
                g.parts[0]->methods, g.sourceNames[0], diagnostics, moduleInvalid);
        }
    }

    for (auto& [treePtr, indices] : eraseIndices) {
        auto& vec = treePtr->*vecMember;
        std::vector<DeclSyntax> kept;
        kept.reserve(vec.size());
        for (std::size_t i = 0; i < vec.size(); ++i) {
            if (indices.find(i) == indices.end()) kept.push_back(std::move(vec[i]));
        }
        vec = std::move(kept);
    }
}

} // namespace

namespace detail {

void mergePartialRecordDeclarations(
    const std::vector<PartialUnitEntry>& units,
    diagnostics::DiagnosticBag& diagnostics,
    bool& moduleInvalid,
    std::unordered_map<std::string, MergedPartialTypeOriginInfo>* outMultiPartRecords) {
    mergeRecordGroupsForUnits(
        units,
        &syntax::CompilationUnitSyntax::classes,
        "class:",
        diagnostics,
        moduleInvalid,
        outMultiPartRecords);
    mergeRecordGroupsForUnits(
        units,
        &syntax::CompilationUnitSyntax::structs,
        "struct:",
        diagnostics,
        moduleInvalid,
        outMultiPartRecords);
}

} // namespace detail

namespace {

void mergePartialTypesInModule(ModuleWork& module, BuildResult& result) {
    // Clone shared syntaxTree before mutating so parsedUnitCache stays immutable!
    bool needsTreeMutation = false;
    std::unordered_map<std::string, std::size_t> classCounts;
    std::unordered_map<std::string, std::size_t> structCounts;
    for (const auto* u : module.units) {
        for (const auto& c : u->syntaxTree->classes) {
            ++classCounts[c.identifierToken.text + "#" + std::to_string(c.typeParameters.size())];
            for (const auto& m : c.methods) {
                if (semantic::hasModifier(m.modifiers, syntax::SyntaxKind::PartialKeyword)) {
                    needsTreeMutation = true;
                }
            }
        }
        for (const auto& s : u->syntaxTree->structs) {
            ++structCounts[s.identifierToken.text + "#" + std::to_string(s.typeParameters.size())];
            for (const auto& m : s.methods) {
                if (semantic::hasModifier(m.modifiers, syntax::SyntaxKind::PartialKeyword)) {
                    needsTreeMutation = true;
                }
            }
        }
    }
    for (const auto& [k, cnt] : classCounts) if (cnt > 1) needsTreeMutation = true;
    for (const auto& [k, cnt] : structCounts) if (cnt > 1) needsTreeMutation = true;
    if (!needsTreeMutation) return;

    std::vector<detail::PartialUnitEntry> partialUnits;
    partialUnits.reserve(module.units.size());
    for (auto* u : module.units) {
        auto cloned = std::make_shared<syntax::CompilationUnitSyntax>();
        if (u->syntaxTree->moduleDeclaration) {
            cloned->moduleDeclaration = std::make_unique<syntax::ModuleDeclarationSyntax>(*u->syntaxTree->moduleDeclaration);
        }
        cloned->imports = u->syntaxTree->imports;
        for (const auto& c : u->syntaxTree->classes) {
            cloned->classes.push_back(syntax::cloneClassDeclarationSyntax(c));
        }
        for (const auto& s : u->syntaxTree->structs) {
            cloned->structs.push_back(syntax::cloneStructDeclarationSyntax(s));
        }
        cloned->enums = u->syntaxTree->enums;
        for (const auto& iface : u->syntaxTree->interfaces) {
            cloned->interfaces.push_back(syntax::cloneInterfaceDeclarationSyntax(iface));
        }
        for (const auto& del : u->syntaxTree->delegates) {
            cloned->delegates.push_back(syntax::cloneDelegateDeclarationSyntax(del));
        }
        for (const auto& fn : u->syntaxTree->functions) {
            cloned->functions.push_back(syntax::cloneFunctionDeclarationSyntax(fn));
        }
        cloned->endOfFileToken = u->syntaxTree->endOfFileToken;
        u->syntaxTree = std::move(cloned);
        partialUnits.push_back(detail::PartialUnitEntry{u->syntaxTree.get(), u->source->name()});
    }

    detail::mergePartialRecordDeclarations(
        partialUnits, result.diagnostics, module.invalid);
}

} // namespace

BuildResult Compilation::build(const BuildSnapshot* previous) const {
    BuildResult result;
    std::vector<std::unique_ptr<ParsedUnit>> units;
    std::map<std::string, ModuleWork> modules;

    static auto sDefaultSession = std::make_shared<CompilerSession>();
    CompilerSession& activeSession = options_.session ? *options_.session : *sDefaultSession;
    auto& sessionImpl = activeSession.impl();

    // Compute active ModulePack + MacroPlugin configuration fingerprint for cache isolation
    std::vector<ModulePack> effectivePacks;
    if (options_.packRegistry) {
        for (const auto& p : options_.packRegistry->packs()) {
            if (options_.disabledPacks.find(p.moduleName) == options_.disabledPacks.end()) {
                effectivePacks.push_back(p);
            }
        }
    } else if (options_.allowDefaultBuiltinPacks) {
        for (const auto& p : ModulePackRegistry::builtinDefaults().packs()) {
            if (options_.disabledPacks.find(p.moduleName) == options_.disabledPacks.end()) {
                effectivePacks.push_back(p);
            }
        }
    }
    for (const auto& extra : options_.extraPacks) {
        if (options_.disabledPacks.find(extra.moduleName) == options_.disabledPacks.end()) {
            effectivePacks.push_back(extra);
        }
    }
    std::uint64_t configFp = 14695981039346656037ull;
    configFp = combineFingerprint(configFp, options_.allowDefaultBuiltinPacks ? 1ull : 0ull);
    configFp = combineFingerprint(configFp, options_.projectConfigFingerprint);
    {
        std::vector<std::string> sortedDisabled(
            options_.disabledPacks.begin(), options_.disabledPacks.end());
        std::sort(sortedDisabled.begin(), sortedDisabled.end());
        for (const auto& d : sortedDisabled) {
            configFp = combineFingerprint(configFp, stableFingerprint("disabled:" + d));
        }
    }
    for (const auto& p : effectivePacks) {
        configFp = combineFingerprint(configFp, stableFingerprint(p.moduleName));
        configFp = combineFingerprint(configFp, stableFingerprint(p.virtualSourcePath));
    }
    for (const auto& ep : options_.extraPacks) {
        configFp = combineFingerprint(configFp, stableFingerprint("extra:" + ep.moduleName));
        configFp = combineFingerprint(configFp, stableFingerprint(ep.virtualSourcePath));
        if (ep.sourceProvider) {
            configFp = combineFingerprint(configFp, stableFingerprint(ep.sourceProvider()));
        }
    }
    for (const auto& mp : options_.macroPlugins) {
        if (!mp) continue;
        configFp = combineFingerprint(configFp, stableFingerprint("macro:" + mp->name()));
        configFp = combineFingerprint(configFp, mp->fingerprint());
        auto mNames = mp->macroNames();
        std::sort(mNames.begin(), mNames.end());
        for (const auto& mn : mNames) {
            configFp = combineFingerprint(configFp, stableFingerprint("mname:" + mn));
        }
    }

    const auto addSourceUnit = [&](const SourceFile& sourceFile) {
        const auto fp = stableFingerprint(sourceFile.path + "\n" + sourceFile.content);
        auto unit = std::make_unique<ParsedUnit>();
        bool cacheHit = false;
        {
            std::lock_guard<std::mutex> lock(sessionImpl.parsedUnitCacheMutex);
            const auto found = sessionImpl.parsedUnitCache.find(fp);
            if (found != sessionImpl.parsedUnitCache.end() &&
                found->second.source &&
                found->second.source->name() == sourceFile.path) {
                *unit = found->second;
                cacheHit = true;
            }
        }
        if (!cacheHit) {
            unit->source = std::make_shared<text::SourceText>(sourceFile.content, sourceFile.path);
            diagnostics::DiagnosticBag parseDiagnostics;
            syntax::Parser parser(*unit->source, parseDiagnostics);
            unit->syntaxTree = std::make_shared<syntax::CompilationUnitSyntax>(
                parser.parseCompilationUnit());
            unit->moduleName = unit->syntaxTree->moduleDeclaration
                ? unit->syntaxTree->moduleDeclaration->fullName()
                : "$global";
            unit->sourceFingerprint = fp;
            for (const auto& import : unit->syntaxTree->imports) {
                unit->imports.push_back(import.fullName());
            }
            unit->parseDiagnostics = parseDiagnostics.items();
            unit->invalid = parseDiagnostics.hasErrors();
            if (unit->syntaxTree->moduleDeclaration) {
                for (const auto& list : unit->syntaxTree->moduleDeclaration->attributes) {
                    for (const auto& attr : list.attributes) {
                        if (attr.nameToken.text == "Deterministic") {
                            unit->deterministic = true;
                        }
                    }
                }
            }
            unit->hasGenericSyntax = (sourceFile.content.find('<') != std::string::npos);
            {
                std::lock_guard<std::mutex> lock(sessionImpl.parsedUnitCacheMutex);
                if (sessionImpl.parsedUnitCache.size() > 8192) sessionImpl.parsedUnitCache.clear();
                sessionImpl.parsedUnitCache[fp] = *unit;
            }
        }
        for (const auto& diagnostic : unit->parseDiagnostics) {
            result.diagnostics.report(
                diagnostic.code, diagnostic.message, diagnostic.span,
                diagnostic.severity, sourceFile.path);
        }
        auto& module = modules[unit->moduleName];
        module.name = unit->moduleName;
        if (unit->deterministic) module.deterministic = true;
        module.units.push_back(unit.get());
        module.invalid = module.invalid || unit->invalid;
        module.imports.insert(unit->imports.begin(), unit->imports.end());
        units.push_back(std::move(unit));
    };

    for (const auto& sourceFile : sources_) {
        addSourceUnit(sourceFile);
    }

    // Execute registered ICompilerMacroPlugin source generators prior to pack mounting & semantic binding
    if (!options_.macroPlugins.empty()) {
        std::vector<MacroAstUnitView> views;
        views.reserve(units.size());
        for (const auto& u : units) {
            if (!u || !u->source || !u->syntaxTree) continue;
            views.push_back(MacroAstUnitView{
                u->source->name(),
                u->moduleName,
                u->imports,
                u->source.get(),
                u->syntaxTree.get()});
        }
        MacroExpansionContext ctx{views};
        std::vector<SourceFile> generatedFiles;
        for (const auto& plugin : options_.macroPlugins) {
            if (!plugin) continue;
            plugin->expand(ctx, generatedFiles, result.diagnostics);
        }
        for (const auto& genFile : generatedFiles) {
            addSourceUnit(genFile);
        }
    }

    // Mount needed ModulePacks via unified ModulePack descriptors (zero hardcoded module branches)
    std::unordered_set<std::string> packNamesSet;
    for (const auto& p : effectivePacks) packNamesSet.insert(p.moduleName);
    for (const auto& pack : effectivePacks) {
        if (!pack.sourceProvider) continue;
        bool needed = false;
        std::vector<std::string> implicitConsumers;
        for (const auto& [name, module] : modules) {
            if (name == pack.moduleName) continue;
            if (module.imports.find(pack.moduleName) != module.imports.end()) {
                needed = true;
                continue;
            }
            if (pack.implicitUsagePredicate && packNamesSet.find(name) == packNamesSet.end()) {
                for (const auto* u : module.units) {
                    if (u && u->source && pack.implicitUsagePredicate(u->source->content())) {
                        needed = true;
                        if (pack.autoInjectImportOnImplicitUsage) {
                            implicitConsumers.push_back(name);
                        }
                        break;
                    }
                }
            }
        }
        if (needed && modules.find(pack.moduleName) == modules.end()) {
            addSourceUnit(SourceFile{
                pack.virtualSourcePath.empty()
                    ? ("$builtin/" + pack.moduleName + ".rscript")
                    : pack.virtualSourcePath,
                pack.sourceProvider()});
        }
        for (const auto& consumer : implicitConsumers) {
            modules[consumer].imports.insert(pack.moduleName);
        }
    }

    // Merge partial class / partial struct declarations & reconcile partial methods
    for (auto& [modName, mod] : modules) {
        (void)modName;
        mergePartialTypesInModule(mod, result);
    }

    specializeNativeGenerics(units, modules, result);

    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& [modName, mod] : modules) {
                auto propagateFrom = [&](const std::string& depName) {
                    if (depName == modName) return;
                    const auto depIt = modules.find(depName);
                    if (depIt == modules.end()) return;
                    for (const auto& sub : depIt->second.imports) {
                        if (sub != modName &&
                            mod.imports.find(sub) == mod.imports.end() &&
                            mod.hygienicModules.insert(sub).second) {
                            changed = true;
                        }
                    }
                    for (const auto& sub : depIt->second.hygienicModules) {
                        if (sub != modName &&
                            mod.imports.find(sub) == mod.imports.end() &&
                            mod.hygienicModules.insert(sub).second) {
                            changed = true;
                        }
                    }
                };
                const auto currentImports = mod.imports;
                const auto currentHygienic = mod.hygienicModules;
                for (const auto& dep : currentImports) propagateFrom(dep);
                for (const auto& dep : currentHygienic) propagateFrom(dep);
            }
        }
    }

    // Stable source order, source fingerprints, and incremental module frontend cache check.
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        std::stable_sort(module.units.begin(), module.units.end(),
            [](const ParsedUnit* left, const ParsedUnit* right) {
                if (left->source->name() != right->source->name()) {
                    return left->source->name() < right->source->name();
                }
                return left->sourceFingerprint < right->sourceFingerprint;
            });
        std::uint64_t sourceFingerprint = 14695981039346656037ull;
        for (const auto* unit : module.units) {
            sourceFingerprint = combineFingerprint(sourceFingerprint, unit->sourceFingerprint);
        }
        module.sourceFingerprint = sourceFingerprint;
    }

    if (result.nativeGenericInstantiations.empty()) {
        std::unordered_map<std::string, int> fpState;
        std::function<std::uint64_t(const std::string&)> computeTransitiveFp =
            [&](const std::string& modName) -> std::uint64_t {
            const auto it = modules.find(modName);
            if (it == modules.end() || it->second.invalid) return 0;
            const int st = fpState[modName];
            if (st == 1) return 0;
            if (st == 2) return it->second.transitiveSourceFingerprint;
            fpState[modName] = 1;
            std::uint64_t tfp = combineFingerprint(
                it->second.sourceFingerprint,
                stableFingerprint(modName + (it->second.deterministic ? ":det" : ":std")));
            for (const auto& imp : it->second.imports) {
                const auto impFp = computeTransitiveFp(imp);
                if (impFp == 0) {
                    fpState[modName] = 2;
                    it->second.transitiveSourceFingerprint = 0;
                    return 0;
                }
                tfp = combineFingerprint(tfp, impFp);
            }
            fpState[modName] = 2;
            it->second.transitiveSourceFingerprint = tfp;
            return tfp;
        };
        for (auto& [moduleName, module] : modules) {
            (void)module;
            (void)computeTransitiveFp(moduleName);
        }
        if (previous) {
            std::lock_guard<std::mutex> lock(sessionImpl.moduleFrontendCacheMutex);
            for (auto& [moduleName, module] : modules) {
                if (moduleName == "$global" || module.invalid || module.transitiveSourceFingerprint == 0) continue;
                const auto prevIt = previous->modules.find(moduleName);
                if (prevIt == previous->modules.end() ||
                    prevIt->second.sourceFingerprint != module.sourceFingerprint) {
                    continue;
                }
                const std::string cacheKey =
                    moduleName + "#" + std::to_string(module.transitiveSourceFingerprint) +
                    "#" + std::to_string(configFp);
                const auto fcIt = sessionImpl.moduleFrontendCache.find(cacheKey);
                if (fcIt == sessionImpl.moduleFrontendCache.end() ||
                    fcIt->second.transitiveSourceFingerprint != module.transitiveSourceFingerprint ||
                    fcIt->second.sourceFingerprint != module.sourceFingerprint ||
                    fcIt->second.publicFingerprint != prevIt->second.publicFingerprint ||
                    fcIt->second.dependencyFingerprint != prevIt->second.dependencyFingerprint) {
                    continue;
                }
                module.frontendCached = true;
                module.publicFingerprint = fcIt->second.publicFingerprint;
                module.dependencyFingerprint = fcIt->second.dependencyFingerprint;
                result.nativeAttributes.insert(
                    result.nativeAttributes.end(),
                    fcIt->second.nativeAttributes.begin(),
                    fcIt->second.nativeAttributes.end());
                result.nativeSequences.insert(
                    result.nativeSequences.end(),
                    fcIt->second.nativeSequences.begin(),
                    fcIt->second.nativeSequences.end());
                result.nativeInterfaces.insert(
                    result.nativeInterfaces.end(),
                    fcIt->second.nativeInterfaces.begin(),
                    fcIt->second.nativeInterfaces.end());
            }
            // Mark frontendCached modules that are transitively imported by any non-frontendCached module
            std::vector<std::string> dirtyQueue;
            for (const auto& [moduleName, module] : modules) {
                if (!module.frontendCached) {
                    for (const auto& imp : module.imports) dirtyQueue.push_back(imp);
                }
            }
            while (!dirtyQueue.empty()) {
                auto curr = std::move(dirtyQueue.back());
                dirtyQueue.pop_back();
                auto it = modules.find(curr);
                if (it == modules.end() || it->second.neededByDirtyModule) continue;
                it->second.neededByDirtyModule = true;
                for (const auto& imp : it->second.imports) dirtyQueue.push_back(imp);
            }
            for (auto& [moduleName, module] : modules) {
                if (module.frontendCached && module.neededByDirtyModule) {
                    const std::string cacheKey =
                        moduleName + "#" + std::to_string(module.transitiveSourceFingerprint) +
                        "#" + std::to_string(configFp);
                    const auto& fc = sessionImpl.moduleFrontendCache.at(cacheKey);
                    module.types = fc.types;
                    module.interfaces = fc.interfaces;
                    module.delegates = fc.delegates;
                    module.declarations = fc.declarations;
                }
            }
        }
    }

    discoverTypesContractsAndLayouts(modules, result);
    synthesizeDeclarationsSequencesAndMembers(modules, result);

    std::unordered_map<semantic::SymbolId, semantic::ObsoleteInfo> globalObsoleteSymbols;
    closeGlobalInterfacesValidateAttributesAndLayouts(
        modules, result, globalObsoleteSymbols);

    if (result.nativeGenericInstantiations.empty()) {
        std::lock_guard<std::mutex> lock(sessionImpl.moduleFrontendCacheMutex);
        if (sessionImpl.moduleFrontendCache.size() > 4096) sessionImpl.moduleFrontendCache.clear();
        for (const auto& [moduleName, module] : modules) {
            if (moduleName == "$global" || module.frontendCached || module.invalid || module.transitiveSourceFingerprint == 0) {
                continue;
            }
            CachedModuleFrontend fc;
            fc.sourceFingerprint = module.sourceFingerprint;
            fc.transitiveSourceFingerprint = module.transitiveSourceFingerprint;
            fc.publicFingerprint = module.publicFingerprint;
            fc.dependencyFingerprint = module.dependencyFingerprint;
            fc.types = module.types;
            fc.interfaces = module.interfaces;
            fc.delegates = module.delegates;
            fc.declarations = module.declarations;
            for (const auto* u : module.units) {
                if (u && u->syntaxTree) {
                    fc.keptSyntaxTrees.push_back(u->syntaxTree);
                }
            }
            const std::string prefix = moduleName + "::";
            for (const auto& attr : result.nativeAttributes) {
                if (attr.target == moduleName || attr.target.rfind(prefix, 0) == 0) {
                    fc.nativeAttributes.push_back(attr);
                }
            }
            for (const auto& seq : result.nativeSequences) {
                if (seq.typeName.rfind(prefix, 0) == 0) {
                    fc.nativeSequences.push_back(seq);
                }
            }
            for (const auto& impl : result.nativeInterfaces) {
                if (impl.typeName.rfind(prefix, 0) == 0) {
                    fc.nativeInterfaces.push_back(impl);
                }
            }
            const std::string cacheKey =
                moduleName + "#" + std::to_string(module.transitiveSourceFingerprint) +
                "#" + std::to_string(configFp);
            sessionImpl.moduleFrontendCache[cacheKey] = std::move(fc);
        }
    }

    for (auto& [moduleName, module] : modules) {
        ModuleBuildInfo buildInfo{
            moduleName,
            module.sourceFingerprint,
            module.publicFingerprint,
            module.dependencyFingerprint,
            false,
        };
        if (previous) {
            const auto cached = previous->modules.find(moduleName);
            bool inlinedUpstreamUnchanged = true;
            if (cached != previous->modules.end()) {
                for (const auto& inlDep : cached->second.module.languageMetadata.inlinedDependencies) {
                    if (inlDep.calleeModule.empty() || inlDep.calleeModule == moduleName) continue;
                    const auto curUp = modules.find(inlDep.calleeModule);
                    const auto prevUp = previous->modules.find(inlDep.calleeModule);
                    if (curUp == modules.end() || prevUp == previous->modules.end() ||
                        curUp->second.sourceFingerprint != prevUp->second.sourceFingerprint) {
                        inlinedUpstreamUnchanged = false;
                        break;
                    }
                }
            }
            if (cached != previous->modules.end() && !module.invalid &&
                inlinedUpstreamUnchanged &&
                cached->second.sourceFingerprint == module.sourceFingerprint &&
                cached->second.publicFingerprint == module.publicFingerprint &&
                cached->second.dependencyFingerprint == module.dependencyFingerprint) {
                buildInfo.reused = true;
                result.modules.push_back(cached->second.module);
                result.symbols.insert(result.symbols.end(),
                    cached->second.symbols.begin(), cached->second.symbols.end());
                result.snapshot.modules[moduleName] = cached->second;
                result.buildInfo.push_back(buildInfo);
                continue;
            }
        }
        // Even if a syntax error occurred (such as an incomplete trailing '.' during editing),
        // still run the semantic binder to collect symbol occurrences for IDE/LSP services,
        // and only skip MIR lowering if errors remain.
        const bool hadSyntaxError = module.invalid;

        auto freeFunctionParamKey = [](const semantic::FunctionSymbol& fn) {
            std::string key = fn.name + "(";
            for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
                if (i != 0) key += ",";
                key += std::to_string(static_cast<int>(fn.parameters[i].modifier)) + ":" +
                    std::to_string(static_cast<int>(fn.parameters[i].type)) + ":" +
                    fn.parameters[i].typeName;
            }
            key += ")";
            return key;
        };
        std::unordered_set<std::string> ownFreeFnSignatures;
        semantic::FunctionOverloadMap visibleFunctions;
        for (const auto& function : module.declarations) {
            visibleFunctions[function.name].push_back(function);
            if (!function.method) {
                ownFreeFnSignatures.insert(freeFunctionParamKey(function));
            }
        }
        for (const auto& importedModule : module.imports) {
            const auto imported = modules.find(importedModule);
            if (imported == modules.end()) continue;
            for (const auto& function : imported->second.declarations) {
                if (!function.method &&
                    ownFreeFnSignatures.find(freeFunctionParamKey(function)) !=
                        ownFreeFnSignatures.end()) {
                    continue;
                }
                visibleFunctions[function.name].push_back(function);
            }
        }
        for (const auto& hygModName : module.hygienicModules) {
            if (hygModName == moduleName || module.imports.count(hygModName) != 0) continue;
            const auto hygIt = modules.find(hygModName);
            if (hygIt == modules.end()) continue;
            for (const auto& function : hygIt->second.declarations) {
                if (!function.method &&
                    ownFreeFnSignatures.find(freeFunctionParamKey(function)) ==
                        ownFreeFnSignatures.end()) {
                    visibleFunctions[function.name].push_back(function);
                }
            }
        }

        semantic::ModuleBindingInput bindingInput;
        // Hygiene is scoped strictly per instantiated template unit/symbol,
        // never opening another module's `internal` symbols to consumer user code.
        bindingInput.moduleName = moduleName;
        bindingInput.declarations = module.declarations;
        bindingInput.functionBindings = module.functionBindings;
        {
            std::unordered_map<semantic::SymbolId, semantic::TypeSymbol> descriptorsById;
            std::vector<semantic::TypeSymbol> descriptorQueue;
            for (const auto& [visibleName, type] : module.visibleTypes) {
                (void)visibleName;
                if (descriptorsById.emplace(type.id, type).second) {
                    descriptorQueue.push_back(type);
                }
            }
            const auto enqueueGlobalTypeByName = [&](const std::string& canonical) {
                if (canonical.empty()) return;
                if (const auto* target = findGlobalType(modules, canonical)) {
                    if (descriptorsById.emplace(target->id, *target).second) {
                        descriptorQueue.push_back(*target);
                    }
                }
            };
            for (std::size_t qIdx = 0; qIdx < descriptorQueue.size(); ++qIdx) {
                const auto curType = descriptorQueue[qIdx];
                if (!curType.baseTypeName.empty()) {
                    enqueueGlobalTypeByName(curType.baseTypeName);
                }
                for (const auto& fld : curType.fields) {
                    if (fld.type == semantic::PrimitiveType::Object ||
                        fld.type == semantic::PrimitiveType::Struct ||
                        fld.type == semantic::PrimitiveType::Enum) {
                        enqueueGlobalTypeByName(fld.typeName);
                    } else if (fld.type == semantic::PrimitiveType::Array) {
                        semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Error;
                        std::string elemTypeName;
                        if (semantic::decodeArrayTypeName(fld.typeName, elemPrim, elemTypeName) &&
                            !elemTypeName.empty()) {
                            enqueueGlobalTypeByName(elemTypeName);
                        }
                    }
                }
            }
            bindingInput.types.reserve(descriptorsById.size());
            for (auto& [typeId, type] : descriptorsById) {
                (void)typeId;
                bindingInput.types.push_back(std::move(type));
            }
            std::sort(bindingInput.types.begin(), bindingInput.types.end(),
                [](const semantic::TypeSymbol& left, const semantic::TypeSymbol& right) {
                    return semantic::canonicalTypeName(left) < semantic::canonicalTypeName(right);
                });
        }
        bindingInput.visibleFunctions = std::move(visibleFunctions);
        bindingInput.visibleTypes = module.visibleTypes;
        bindingInput.obsoleteSymbols = globalObsoleteSymbols;
        for (const auto* unit : module.units) bindingInput.units.push_back(unit->syntaxTree.get());

        diagnostics::DiagnosticBag moduleDiagnostics;
        semantic::Binder binder(moduleDiagnostics);
        auto semanticModel = binder.bindModule(bindingInput);
        result.symbols.insert(result.symbols.end(),
            semanticModel.occurrences.begin(), semanticModel.occurrences.end());
        if (hadSyntaxError || moduleDiagnostics.hasErrors()) {
            result.diagnostics.append(moduleDiagnostics);
            for (const auto& t : bindingInput.types) {
                if (t.moduleName == moduleName) {
                    mir::Module partialMod;
                    partialMod.name = moduleName;
                    for (const auto& [visName, visType] : module.visibleTypes) {
                        (void)visName;
                        if (visType.moduleName == moduleName) {
                            const bool dup = std::any_of(
                                partialMod.types.begin(),
                                partialMod.types.end(),
                                [&](const semantic::TypeSymbol& ex) { return ex.id == visType.id; });
                            if (!dup) partialMod.types.push_back(visType);
                        }
                    }
                    {
                        std::unordered_set<std::string> seenPaths;
                        for (const auto* u : module.units) {
                            if (u && u->source && seenPaths.insert(u->source->name()).second) {
                                partialMod.sourceFiles.push_back(makeSourceFileInfo(
                                    *u->source,
                                    static_cast<debug::SourceFileId>(partialMod.sourceFiles.size())));
                            }
                        }
                    }
                    result.modules.push_back(std::move(partialMod));
                    break;
                }
            }
            result.buildInfo.push_back(buildInfo);
            continue;
        }

        mir::Lowerer lowerer;
        auto mirModule = lowerer.lower(semanticModel);
        mirModule.sourceFiles.reserve(module.units.size());
        {
            std::unordered_set<std::string> seenPaths;
            for (const auto* u : module.units) {
                if (u && u->source && seenPaths.insert(u->source->name()).second) {
                    mirModule.sourceFiles.push_back(makeSourceFileInfo(
                        *u->source,
                        static_cast<debug::SourceFileId>(mirModule.sourceFiles.size())));
                }
            }
        }
        for (auto& function : mirModule.functions) {
            debug::finalizeFunctionDebugInfo(
                function.debugInfo, mirModule.sourceFiles);
        }
        mirModule.languageMetadata = languageMetadataForModule(
            moduleName, mirModule, result);
        (void)mir::verifyModule(mirModule, moduleDiagnostics);
        result.diagnostics.append(moduleDiagnostics);
        if (!moduleDiagnostics.hasErrors()) {
            result.modules.push_back(mirModule);
            result.snapshot.modules[moduleName] = {
                module.sourceFingerprint,
                module.publicFingerprint,
                module.dependencyFingerprint,
                mirModule,
                semanticModel.occurrences,
            };
        }
        result.buildInfo.push_back(buildInfo);
    }

    std::stable_sort(
        result.nativeAttributes.begin(),
        result.nativeAttributes.end(),
        [](const auto& left, const auto& right) {
            if (left.target != right.target) {
                return left.target < right.target;
            }
            if (left.name != right.name) {
                return left.name < right.name;
            }
            if (left.sourceName != right.sourceName) {
                return left.sourceName < right.sourceName;
            }
            return left.offset < right.offset;
        });
    std::stable_sort(
        result.nativeSequences.begin(),
        result.nativeSequences.end(),
        [](const auto& left, const auto& right) {
            if (left.typeName != right.typeName) {
                return left.typeName < right.typeName;
            }
            return left.name < right.name;
        });
    std::stable_sort(
        result.nativeInterfaces.begin(),
        result.nativeInterfaces.end(),
        [](const auto& left, const auto& right) {
            return left.typeName < right.typeName;
        });
    return result;
}

} // namespace realscript::compiler
