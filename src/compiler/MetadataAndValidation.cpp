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

void closeGlobalInterfacesValidateAttributesAndLayouts(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result,
    std::unordered_map<semantic::SymbolId, semantic::ObsoleteInfo>& globalObsoleteSymbols) {
    // Interface implementation maps are initially validated in module-local
    // declaration order. Close inherited maps globally so a derived module does
    // not depend on whether its imported base module sorts before it.
    {
        struct InterfaceOwnerWork {
            ModuleWork* module = nullptr;
            semantic::TypeSymbol* type = nullptr;
            std::size_t depth = 0;
        };
        std::vector<InterfaceOwnerWork> owners;
        for (auto& [moduleName, module] : modules) {
            (void)moduleName;
            if (module.frontendCached) continue;
            for (auto& type : module.types) {
                if (type.kind != semantic::TypeKind::Class ||
                    type.interfaceType) {
                    continue;
                }
                std::size_t depth = 0;
                std::unordered_set<semantic::SymbolId> visited;
                auto current = type.baseTypeName;
                while (!current.empty()) {
                    const auto* base = findGlobalType(modules, current);
                    if (!base || !visited.insert(base->id).second) break;
                    ++depth;
                    current = base->baseTypeName;
                }
                owners.push_back(InterfaceOwnerWork{&module, &type, depth});
            }
        }
        std::sort(
            owners.begin(), owners.end(),
            [](const InterfaceOwnerWork& left,
               const InterfaceOwnerWork& right) {
                if (left.depth != right.depth) {
                    return left.depth < right.depth;
                }
                return semantic::canonicalTypeName(*left.type) <
                    semantic::canonicalTypeName(*right.type);
            });

        const auto interfaceNameById =
            [&](semantic::SymbolId interfaceTypeId) {
                for (const auto& [moduleName, module] : modules) {
                    (void)moduleName;
                    for (const auto& type : module.types) {
                        if (type.id == interfaceTypeId &&
                            type.interfaceType) {
                            return semantic::canonicalTypeName(type);
                        }
                    }
                }
                return std::string{};
            };

        for (auto& work : owners) {
            auto& owner = *work.type;
            if (!owner.baseTypeName.empty()) {
                const auto* base = findGlobalType(
                    modules, owner.baseTypeName);
                if (base) {
                    for (const auto& inherited :
                         base->interfaceDispatchMaps) {
                        const auto found = std::find_if(
                            owner.interfaceDispatchMaps.begin(),
                            owner.interfaceDispatchMaps.end(),
                            [&](const auto& current) {
                                return current.interfaceTypeId ==
                                    inherited.interfaceTypeId;
                            });
                        if (found == owner.interfaceDispatchMaps.end()) {
                            auto closed = inherited;
                            for (auto& targetId : closed.slots) {
                                if (targetId == 0) continue;
                                const auto baseMethod = std::find_if(
                                    base->methods.begin(),
                                    base->methods.end(),
                                    [&](const auto& method) {
                                        return method.id == targetId;
                                    });
                                if (baseMethod == base->methods.end() ||
                                    baseMethod->virtualSlot ==
                                        std::numeric_limits<std::uint32_t>::max()) {
                                    continue;
                                }
                                const auto overrideMethod = std::find_if(
                                    owner.methods.begin(),
                                    owner.methods.end(),
                                    [&](const auto& method) {
                                        return !method.interfaceMethod &&
                                            method.virtualSlot ==
                                                baseMethod->virtualSlot;
                                    });
                                if (overrideMethod != owner.methods.end()) {
                                    targetId = overrideMethod->abstractMethod
                                        ? 0
                                        : overrideMethod->id;
                                }
                            }
                            owner.interfaceDispatchMaps.push_back(
                                std::move(closed));
                        }
                    }
                }
            }
            std::sort(
                owner.interfaceDispatchMaps.begin(),
                owner.interfaceDispatchMaps.end(),
                [](const auto& left, const auto& right) {
                    return left.interfaceTypeId < right.interfaceTypeId;
                });

            if (!owner.interfaceDispatchMaps.empty()) {
                const auto typeName = semantic::canonicalTypeName(owner);
                auto metadata = std::find_if(
                    result.nativeInterfaces.begin(),
                    result.nativeInterfaces.end(),
                    [&](const auto& implementation) {
                        return implementation.typeName == typeName;
                    });
                if (metadata == result.nativeInterfaces.end()) {
                    LanguageInterfaceImplementation implementation;
                    implementation.typeName = typeName;
                    result.nativeInterfaces.push_back(
                        std::move(implementation));
                    metadata = std::prev(result.nativeInterfaces.end());
                }
                for (const auto& map : owner.interfaceDispatchMaps) {
                    const auto name = interfaceNameById(
                        map.interfaceTypeId);
                    if (!name.empty()) {
                        metadata->interfaces.push_back(name);
                    }
                }
                std::sort(
                    metadata->interfaces.begin(),
                    metadata->interfaces.end());
                metadata->interfaces.erase(
                    std::unique(
                        metadata->interfaces.begin(),
                        metadata->interfaces.end()),
                    metadata->interfaces.end());
            }
        }
        for (auto& [moduleName, module] : modules) {
            (void)moduleName;
            if (module.frontendCached) continue;
            refreshVisibleTypes(modules, module);

            // The module fingerprint was calculated before global inherited
            // interface closure. Fold the final object model into it so cache
            // reuse and hot reload observe the completed maps.
            std::vector<std::string> objectModelSignatures;
            for (const auto& type : module.types) {
                if (!type.synthetic) {
                    objectModelSignatures.push_back(typeSignature(type));
                }
            }
            std::sort(
                objectModelSignatures.begin(),
                objectModelSignatures.end());
            std::ostringstream objectModelSurface;
            for (const auto& signature : objectModelSignatures) {
                objectModelSurface << signature << '\n';
            }
            module.publicFingerprint = combineFingerprint(
                module.publicFingerprint,
                stableFingerprint(objectModelSurface.str()));
        }
    }

    // Strongly-Typed Attribute Contracts ([AttributeUsage] & [Obsolete])
    {
        enum AttrTargetFlags : std::uint32_t {
            AttrTarget_None      = 0u,
            AttrTarget_Class     = 1u << 0,
            AttrTarget_Struct    = 1u << 1,
            AttrTarget_Interface = 1u << 2,
            AttrTarget_Enum      = 1u << 3,
            AttrTarget_Delegate  = 1u << 4,
            AttrTarget_Ctor      = 1u << 5,
            AttrTarget_Method    = 1u << 6,
            AttrTarget_Function  = 1u << 7,
            AttrTarget_Field     = 1u << 8,
            AttrTarget_Property  = 1u << 9,
            AttrTarget_Parameter = 1u << 10,
            AttrTarget_Event     = 1u << 11,
            AttrTarget_Module    = 1u << 12,
            AttrTarget_All       = 0xFFFFFFFFu
        };
        struct AttributeUsageRule {
            std::uint32_t validTargets = 0xFFFFFFFFu;
            bool allowMultiple = false;
        };

        auto toLowerAscii = [](std::string s) {
            for (char& c : s) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return s;
        };
        auto stripAttributeSuffix = [](const std::string& name) -> std::string {
            constexpr std::string_view sfx = "Attribute";
            if (name.size() > sfx.size() &&
                name.compare(name.size() - sfx.size(), sfx.size(), sfx) == 0) {
                return name.substr(0, name.size() - sfx.size());
            }
            return name;
        };
        auto parseTargetString = [&](const std::string& spec) -> std::uint32_t {
            std::uint32_t flags = AttrTarget_None;
            std::string cur;
            auto flushToken = [&]() {
                if (cur.empty()) return;
                const auto low = toLowerAscii(cur);
                cur.clear();
                if (low == "all") flags |= AttrTarget_All;
                else if (low == "class" || low == "classes") flags |= AttrTarget_Class;
                else if (low == "struct" || low == "structs") flags |= AttrTarget_Struct;
                else if (low == "interface" || low == "interfaces") flags |= AttrTarget_Interface;
                else if (low == "enum" || low == "enums" || low == "enummember") flags |= AttrTarget_Enum;
                else if (low == "delegate" || low == "delegates") flags |= AttrTarget_Delegate;
                else if (low == "constructor" || low == "constructors" || low == "ctor") flags |= AttrTarget_Ctor;
                else if (low == "method" || low == "methods") flags |= (AttrTarget_Method | AttrTarget_Function);
                else if (low == "function" || low == "functions") flags |= (AttrTarget_Function | AttrTarget_Method);
                else if (low == "field" || low == "fields") flags |= AttrTarget_Field;
                else if (low == "property" || low == "properties") flags |= AttrTarget_Property;
                else if (low == "parameter" || low == "parameters" || low == "param") flags |= AttrTarget_Parameter;
                else if (low == "event" || low == "events") flags |= AttrTarget_Event;
                else if (low == "module" || low == "modules") flags |= AttrTarget_Module;
            };
            for (const char ch : spec) {
                const bool isAlpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
                if (isAlpha) cur.push_back(ch);
                else flushToken();
            }
            flushToken();
            return flags == AttrTarget_None ? AttrTarget_All : flags;
        };
        auto evalAttrConstValue = [&](const syntax::AttributeArgumentSyntax& arg,
                                      const text::SourceText& src,
                                      const semantic::TypeSymbolMap& visTypes,
                                      syntax::TokenValue& outVal) -> bool {
            const auto sp = arg.valueSpan();
            if (sp.empty()) return false;
            text::SourceText wrapSrc(
                "class __RsAttrEval { const int __V = " + std::string(src.view(sp)) + "; }",
                "$attr_eval");
            diagnostics::DiagnosticBag ignored;
            syntax::Parser p(wrapSrc, ignored);
            auto cu = p.parseCompilationUnit();
            if (ignored.hasErrors() || cu.classes.empty() ||
                cu.classes[0].fields.empty() || !cu.classes[0].fields[0].initializer) {
                return false;
            }
            semantic::PrimitiveType pt = semantic::PrimitiveType::Error;
            return semantic::evaluateConstantSyntaxExpression(
                *cu.classes[0].fields[0].initializer, nullptr, visTypes, outVal, pt);
        };
        auto evalAttrArgString = [&](const syntax::AttributeArgumentSyntax& arg,
                                     const text::SourceText& src,
                                     const semantic::TypeSymbolMap& visTypes) -> std::optional<std::string> {
            if (arg.valueTokens.size() == 1 &&
                arg.valueTokens[0].kind == syntax::SyntaxKind::StringLiteralToken &&
                std::holds_alternative<std::string>(arg.valueTokens[0].value)) {
                return std::get<std::string>(arg.valueTokens[0].value);
            }
            syntax::TokenValue tv;
            if (evalAttrConstValue(arg, src, visTypes, tv)) {
                if (const auto* s = std::get_if<std::string>(&tv)) return *s;
            }
            for (const auto& tok : arg.valueTokens) {
                if (tok.kind == syntax::SyntaxKind::StringLiteralToken &&
                    std::holds_alternative<std::string>(tok.value)) {
                    return std::get<std::string>(tok.value);
                }
            }
            return std::nullopt;
        };
        auto evalAttrArgBool = [&](const syntax::AttributeArgumentSyntax& arg,
                                   const text::SourceText& src,
                                   const semantic::TypeSymbolMap& visTypes) -> std::optional<bool> {
            if (arg.valueTokens.size() == 1) {
                if (arg.valueTokens[0].kind == syntax::SyntaxKind::TrueKeyword) return true;
                if (arg.valueTokens[0].kind == syntax::SyntaxKind::FalseKeyword) return false;
            }
            syntax::TokenValue tv;
            if (evalAttrConstValue(arg, src, visTypes, tv)) {
                if (const auto* b = std::get_if<bool>(&tv)) return *b;
            }
            return std::nullopt;
        };

        // 1. Collect [AttributeUsage] contracts across all custom attribute types
        std::unordered_map<semantic::SymbolId, AttributeUsageRule> usageByTypeId;
        usageByTypeId.reserve(32);
        const auto scanAttrTypeDecls = [&](ModuleWork& mod, const ParsedUnit& unit, const auto& decls) {
            for (const auto& decl : decls) {
                if (!decl.typeParameters.empty()) continue;
                const auto* owner = findOwnType(mod, decl.identifierToken.text);
                if (!owner) continue;
                for (const auto& list : decl.attributes) {
                    for (const auto& attr : list.attributes) {
                        const auto baseName = stripAttributeSuffix(attr.nameToken.text);
                        if (baseName != "AttributeUsage") continue;
                        AttributeUsageRule rule;
                        rule.validTargets = AttrTarget_All;
                        rule.allowMultiple = false;
                        std::size_t posIdx = 0;
                        for (const auto& arg : attr.arguments) {
                            if (arg.nameToken) {
                                const auto argName = arg.nameToken->text;
                                if (argName == "validOn" || argName == "targets") {
                                    if (const auto s = evalAttrArgString(arg, *unit.source, mod.visibleTypes)) {
                                        rule.validTargets = parseTargetString(*s);
                                    }
                                } else if (argName == "allowMultiple" || argName == "AllowMultiple") {
                                    if (const auto b = evalAttrArgBool(arg, *unit.source, mod.visibleTypes)) {
                                        rule.allowMultiple = *b;
                                    }
                                }
                            } else {
                                if (posIdx == 0) {
                                    if (const auto s = evalAttrArgString(arg, *unit.source, mod.visibleTypes)) {
                                        rule.validTargets = parseTargetString(*s);
                                    }
                                } else if (posIdx == 1) {
                                    if (const auto b = evalAttrArgBool(arg, *unit.source, mod.visibleTypes)) {
                                        rule.allowMultiple = *b;
                                    }
                                }
                                ++posIdx;
                            }
                        }
                        usageByTypeId[owner->id] = rule;
                    }
                }
            }
        };
        for (auto& [modName, mod] : modules) {
            (void)modName;
            for (const auto* unit : mod.units) {
                if (!unit || !unit->syntaxTree) continue;
                scanAttrTypeDecls(mod, *unit, unit->syntaxTree->classes);
                scanAttrTypeDecls(mod, *unit, unit->syntaxTree->structs);
            }
        }

        // Helper to extract [Obsolete] info from attribute lists
        const auto extractObsolete = [&](const std::vector<syntax::AttributeListSyntax>& lists,
                                         const text::SourceText& src,
                                         const semantic::TypeSymbolMap& visTypes)
            -> std::optional<semantic::ObsoleteInfo> {
            for (const auto& list : lists) {
                for (const auto& attr : list.attributes) {
                    if (stripAttributeSuffix(attr.nameToken.text) != "Obsolete") continue;
                    semantic::ObsoleteInfo info;
                    std::size_t posIdx = 0;
                    for (const auto& arg : attr.arguments) {
                        if (arg.nameToken) {
                            const auto& an = arg.nameToken->text;
                            if (an == "message" || an == "reason") {
                                if (const auto s = evalAttrArgString(arg, src, visTypes)) {
                                    info.message = *s;
                                }
                            } else if (an == "error" || an == "isError") {
                                if (const auto b = evalAttrArgBool(arg, src, visTypes)) {
                                    info.isError = *b;
                                }
                            }
                        } else {
                            if (posIdx == 0) {
                                if (const auto s = evalAttrArgString(arg, src, visTypes)) {
                                    info.message = *s;
                                } else if (const auto b = evalAttrArgBool(arg, src, visTypes)) {
                                    info.isError = *b;
                                }
                            } else if (posIdx == 1) {
                                if (const auto b = evalAttrArgBool(arg, src, visTypes)) {
                                    info.isError = *b;
                                }
                            }
                            ++posIdx;
                        }
                    }
                    return info;
                }
            }
            return std::nullopt;
        };

        // 2. Validate attribute contracts ([AttributeUsage]) & populate globalObsoleteSymbols
        const auto targetKindLabel = [](std::uint32_t flag) -> const char* {
            switch (flag) {
            case AttrTarget_Class: return "class";
            case AttrTarget_Struct: return "struct";
            case AttrTarget_Interface: return "interface";
            case AttrTarget_Enum: return "enum";
            case AttrTarget_Delegate: return "delegate";
            case AttrTarget_Ctor: return "constructor";
            case AttrTarget_Method: return "method";
            case AttrTarget_Function: return "function";
            case AttrTarget_Field: return "field";
            case AttrTarget_Property: return "property";
            case AttrTarget_Parameter: return "parameter";
            case AttrTarget_Event: return "event";
            case AttrTarget_Module: return "module";
            default: return "declaration";
            }
        };

        const auto validateAttrLists = [&](ModuleWork& mod,
                                           const std::string& srcName,
                                           const std::vector<syntax::AttributeListSyntax>& lists,
                                           std::uint32_t targetFlag) {
            if (lists.empty()) return;
            std::unordered_set<std::string> seenSingleUseKeys;
            for (const auto& list : lists) {
                for (const auto& attr : list.attributes) {
                    const auto rawName = attr.nameToken.text;
                    const auto shortName = stripAttributeSuffix(rawName);
                    if (shortName == "AttributeUsage") {
                        const std::uint32_t validOn = AttrTarget_Class | AttrTarget_Struct;
                        if ((validOn & targetFlag) == 0) {
                            result.diagnostics.report(
                                "RS4103",
                                "attribute '" + rawName + "' is not valid on '" +
                                    std::string(targetKindLabel(targetFlag)) + "' declarations",
                                attr.nameToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                srcName);
                            mod.invalid = true;
                        }
                        if (!seenSingleUseKeys.insert("$builtin::AttributeUsage").second) {
                            result.diagnostics.report(
                                "RS4104",
                                "duplicate attribute '" + rawName +
                                    "' is not allowed because allowMultiple is false",
                                attr.nameToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                srcName);
                            mod.invalid = true;
                        }
                        continue;
                    }
                    if (shortName == "Obsolete") {
                        const std::uint32_t validOn =
                            AttrTarget_Class | AttrTarget_Struct | AttrTarget_Interface |
                            AttrTarget_Enum | AttrTarget_Delegate | AttrTarget_Ctor |
                            AttrTarget_Method | AttrTarget_Function | AttrTarget_Field |
                            AttrTarget_Property | AttrTarget_Event;
                        if ((validOn & targetFlag) == 0) {
                            result.diagnostics.report(
                                "RS4103",
                                "attribute '" + rawName + "' is not valid on '" +
                                    std::string(targetKindLabel(targetFlag)) + "' declarations",
                                attr.nameToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                srcName);
                            mod.invalid = true;
                        }
                        if (!seenSingleUseKeys.insert("$builtin::Obsolete").second) {
                            result.diagnostics.report(
                                "RS4104",
                                "duplicate attribute '" + rawName +
                                    "' is not allowed because allowMultiple is false",
                                attr.nameToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                srcName);
                            mod.invalid = true;
                        }
                        continue;
                    }

                    const semantic::TypeSymbol* attrType = nullptr;
                    auto it = mod.visibleTypes.find(rawName);
                    if (it != mod.visibleTypes.end()) {
                        attrType = &it->second;
                    } else {
                        const auto withSuffix = shortName + "Attribute";
                        it = mod.visibleTypes.find(withSuffix);
                        if (it != mod.visibleTypes.end()) {
                            attrType = &it->second;
                        }
                    }
                    if (!attrType) continue;
                    const auto ruleIt = usageByTypeId.find(attrType->id);
                    if (ruleIt == usageByTypeId.end()) continue;
                    const auto& rule = ruleIt->second;
                    if ((rule.validTargets & targetFlag) == 0) {
                        result.diagnostics.report(
                            "RS4103",
                            "attribute '" + rawName + "' is not valid on '" +
                                std::string(targetKindLabel(targetFlag)) + "' declarations",
                            attr.nameToken.span,
                            diagnostics::DiagnosticSeverity::Error,
                            srcName);
                        mod.invalid = true;
                    }
                    if (!rule.allowMultiple) {
                        const std::string key = "id:" + std::to_string(attrType->id);
                        if (!seenSingleUseKeys.insert(key).second) {
                            result.diagnostics.report(
                                "RS4104",
                                "duplicate attribute '" + rawName +
                                    "' is not allowed because allowMultiple is false",
                                attr.nameToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                srcName);
                            mod.invalid = true;
                        }
                    }
                }
            }
        };

        for (auto& [modName, mod] : modules) {
            (void)modName;
            std::unordered_map<std::string, const text::SourceText*> unitSourcesByName;
            for (const auto* u : mod.units) {
                if (u && u->source) unitSourcesByName[u->source->name()] = u->source.get();
            }
            for (const auto* unit : mod.units) {
                if (!unit || !unit->syntaxTree) continue;
                const auto& srcName = unit->source->name();
                const bool validateThisMod = !mod.frontendCached;
                const auto memberSrcName = [&](const auto& member) -> const std::string& {
                    return member.originSourceName.empty() ? srcName : member.originSourceName;
                };
                const auto memberSourceText = [&](const auto& member) -> const text::SourceText& {
                    if (!member.originSourceName.empty()) {
                        const auto it = unitSourcesByName.find(member.originSourceName);
                        if (it != unitSourcesByName.end() && it->second) return *it->second;
                    }
                    return *unit->source;
                };

                if (validateThisMod && unit->syntaxTree->moduleDeclaration) {
                    validateAttrLists(
                        mod, srcName, unit->syntaxTree->moduleDeclaration->attributes, AttrTarget_Module);
                }

                const auto processRecordAttrs = [&](const auto& decls, std::uint32_t recordFlag) {
                    for (const auto& decl : decls) {
                        if (!decl.typeParameters.empty()) continue;
                        if (validateThisMod) {
                            validateAttrLists(mod, srcName, decl.attributes, recordFlag);
                        }
                        const auto* owner = findOwnType(mod, decl.identifierToken.text);
                        if (owner) {
                            if (const auto obs = extractObsolete(decl.attributes, *unit->source, mod.visibleTypes)) {
                                globalObsoleteSymbols[owner->id] = *obs;
                            }
                        }
                        for (const auto& fSyn : decl.fields) {
                            const auto& memberSrc = memberSrcName(fSyn);
                            if (validateThisMod) {
                                validateAttrLists(mod, memberSrc, fSyn.attributes, AttrTarget_Field);
                            }
                            if (owner) {
                                for (const auto& fSym : owner->fields) {
                                    if (fSym.declaringTypeId == owner->id &&
                                        fSym.name == fSyn.identifierToken.text) {
                                        if (const auto obs = extractObsolete(
                                                fSyn.attributes, memberSourceText(fSyn), mod.visibleTypes)) {
                                            globalObsoleteSymbols[fSym.id] = *obs;
                                        }
                                    }
                                }
                            }
                        }
                        for (const auto& pSyn : decl.properties) {
                            const auto& memberSrc = memberSrcName(pSyn);
                            if (validateThisMod) {
                                validateAttrLists(mod, memberSrc, pSyn.attributes, AttrTarget_Property);
                            }
                            if (owner) {
                                for (const auto& pSym : owner->properties) {
                                    if (pSym.declaringTypeId == owner->id &&
                                        pSym.name == pSyn.identifierToken.text) {
                                        if (const auto obs = extractObsolete(
                                                pSyn.attributes, memberSourceText(pSyn), mod.visibleTypes)) {
                                            globalObsoleteSymbols[pSym.id] = *obs;
                                            if (pSym.getter) globalObsoleteSymbols[pSym.getter->id] = *obs;
                                            if (pSym.setter) globalObsoleteSymbols[pSym.setter->id] = *obs;
                                        }
                                    }
                                }
                            }
                        }
                        for (const auto& evSyn : decl.events) {
                            if (validateThisMod) {
                                validateAttrLists(mod, memberSrcName(evSyn), evSyn.attributes, AttrTarget_Event);
                            }
                        }
                        for (const auto& cSyn : decl.constructors) {
                            const auto& memberSrc = memberSrcName(cSyn);
                            if (validateThisMod) {
                                validateAttrLists(mod, memberSrc, cSyn.attributes, AttrTarget_Ctor);
                                for (const auto& param : cSyn.parameters) {
                                    validateAttrLists(mod, memberSrc, param.attributes, AttrTarget_Parameter);
                                }
                            }
                            if (owner) {
                                if (const auto obs = extractObsolete(
                                        cSyn.attributes, memberSourceText(cSyn), mod.visibleTypes)) {
                                    diagnostics::DiagnosticBag ignored;
                                    const auto cSym = semantic::declareConstructorSymbol(
                                        mod.name, cSyn, *owner, mod.visibleTypes, ignored);
                                    globalObsoleteSymbols[cSym.id] = *obs;
                                }
                            }
                        }
                        for (const auto& mSyn : decl.methods) {
                            if (!mSyn.typeParameters.empty()) continue;
                            const auto& memberSrc = memberSrcName(mSyn);
                            if (validateThisMod) {
                                validateAttrLists(mod, memberSrc, mSyn.attributes, AttrTarget_Method);
                                for (const auto& param : mSyn.parameters) {
                                    validateAttrLists(mod, memberSrc, param.attributes, AttrTarget_Parameter);
                                }
                            }
                            if (owner) {
                                if (const auto obs = extractObsolete(
                                        mSyn.attributes, memberSourceText(mSyn), mod.visibleTypes)) {
                                    diagnostics::DiagnosticBag ignored;
                                    const auto mSym = semantic::declareFunctionSymbol(
                                        mod.name, mSyn, mod.visibleTypes, ignored, owner);
                                    globalObsoleteSymbols[mSym.id] = *obs;
                                }
                            }
                        }
                        for (const auto& sSyn : decl.sequences) {
                            if (validateThisMod) {
                                const auto& memberSrc = memberSrcName(sSyn);
                                validateAttrLists(mod, memberSrc, sSyn.attributes, AttrTarget_Method);
                                for (const auto& param : sSyn.parameters) {
                                    validateAttrLists(mod, memberSrc, param.attributes, AttrTarget_Parameter);
                                }
                            }
                        }
                    }
                };
                processRecordAttrs(unit->syntaxTree->classes, AttrTarget_Class);
                processRecordAttrs(unit->syntaxTree->structs, AttrTarget_Struct);

                for (const auto& en : unit->syntaxTree->enums) {
                    if (validateThisMod) {
                        validateAttrLists(mod, srcName, en.attributes, AttrTarget_Enum);
                    }
                    const auto* owner = findOwnType(mod, en.identifierToken.text);
                    if (owner) {
                        if (const auto obs = extractObsolete(en.attributes, *unit->source, mod.visibleTypes)) {
                            globalObsoleteSymbols[owner->id] = *obs;
                        }
                    }
                    for (const auto& em : en.members) {
                        if (validateThisMod) {
                            validateAttrLists(mod, srcName, em.attributes, AttrTarget_Field);
                        }
                        if (owner) {
                            if (const auto obs = extractObsolete(em.attributes, *unit->source, mod.visibleTypes)) {
                                for (const auto& emSym : owner->enumMembers) {
                                    if (emSym.name == em.identifierToken.text) {
                                        globalObsoleteSymbols[emSym.id] = *obs;
                                    }
                                }
                            }
                        }
                    }
                }

                for (const auto& iface : unit->syntaxTree->interfaces) {
                    if (!iface.typeParameters.empty()) continue;
                    if (validateThisMod) {
                        validateAttrLists(mod, srcName, iface.attributes, AttrTarget_Interface);
                    }
                    const auto* owner = findOwnType(mod, iface.identifierToken.text);
                    if (owner) {
                        if (const auto obs = extractObsolete(iface.attributes, *unit->source, mod.visibleTypes)) {
                            globalObsoleteSymbols[owner->id] = *obs;
                        }
                    }
                    for (const auto& im : iface.methods) {
                        if (validateThisMod) {
                            validateAttrLists(mod, srcName, im.attributes, AttrTarget_Method);
                            for (const auto& param : im.parameters) {
                                validateAttrLists(mod, srcName, param.attributes, AttrTarget_Parameter);
                            }
                        }
                        if (owner) {
                            if (const auto obs = extractObsolete(im.attributes, *unit->source, mod.visibleTypes)) {
                                diagnostics::DiagnosticBag ignored;
                                const auto imSym = semantic::declareFunctionSymbol(
                                    mod.name, im, mod.visibleTypes, ignored, owner);
                                globalObsoleteSymbols[imSym.id] = *obs;
                            }
                        }
                    }
                }

                for (const auto& del : unit->syntaxTree->delegates) {
                    if (!del.typeParameters.empty()) continue;
                    if (validateThisMod) {
                        validateAttrLists(mod, srcName, del.attributes, AttrTarget_Delegate);
                        for (const auto& param : del.parameters) {
                            validateAttrLists(mod, srcName, param.attributes, AttrTarget_Parameter);
                        }
                    }
                    const auto* owner = findOwnType(mod, del.identifierToken.text);
                    if (owner) {
                        if (const auto obs = extractObsolete(del.attributes, *unit->source, mod.visibleTypes)) {
                            globalObsoleteSymbols[owner->id] = *obs;
                        }
                    }
                }

                for (const auto& fn : unit->syntaxTree->functions) {
                    if (!fn.typeParameters.empty()) continue;
                    if (validateThisMod) {
                        validateAttrLists(mod, srcName, fn.attributes, AttrTarget_Function);
                        for (const auto& param : fn.parameters) {
                            validateAttrLists(mod, srcName, param.attributes, AttrTarget_Parameter);
                        }
                    }
                    if (const auto obs = extractObsolete(fn.attributes, *unit->source, mod.visibleTypes)) {
                        diagnostics::DiagnosticBag ignored;
                        const auto fnSym = semantic::declareFunctionSymbol(
                            mod.name, fn, mod.visibleTypes, ignored);
                        globalObsoleteSymbols[fnSym.id] = *obs;
                    }
                }
            }
        }

        // 3. Check declaration-signature TypeSyntax references for [Obsolete] types
        if (!globalObsoleteSymbols.empty()) {
            std::function<void(
                ModuleWork&,
                const std::string&,
                const syntax::TypeSyntax&,
                semantic::SymbolId,
                bool)> checkSignatureTypeObs;
            checkSignatureTypeObs = [&](ModuleWork& mod,
                                        const std::string& srcName,
                                        const syntax::TypeSyntax& typeSyn,
                                        semantic::SymbolId selfTypeId,
                                        bool contextIsObsolete) {
                if (contextIsObsolete) return;
                if (semantic::resolvePrimitiveType(typeSyn.name.text) == semantic::PrimitiveType::Error) {
                    const auto it = mod.visibleTypes.find(typeSyn.name.text);
                    if (it != mod.visibleTypes.end() && it->second.id != selfTypeId) {
                        const auto obsIt = globalObsoleteSymbols.find(it->second.id);
                        if (obsIt != globalObsoleteSymbols.end()) {
                            std::string msg = "type '" + semantic::canonicalTypeName(it->second) + "' is obsolete";
                            if (!obsIt->second.message.empty()) {
                                msg += ": " + obsIt->second.message;
                            }
                            result.diagnostics.report(
                                obsIt->second.isError ? "RS4102" : "RS4101",
                                std::move(msg),
                                typeSyn.span(),
                                obsIt->second.isError
                                    ? diagnostics::DiagnosticSeverity::Error
                                    : diagnostics::DiagnosticSeverity::Warning,
                                srcName);
                            if (obsIt->second.isError) mod.invalid = true;
                        }
                    }
                }
                for (const auto& arg : typeSyn.typeArguments) {
                    checkSignatureTypeObs(mod, srcName, arg, selfTypeId, contextIsObsolete);
                }
            };

            for (auto& [modName, mod] : modules) {
                (void)modName;
                if (mod.frontendCached) continue;
                std::unordered_map<std::string, const text::SourceText*> unitSourcesByName;
                for (const auto* u : mod.units) {
                    if (u && u->source) unitSourcesByName[u->source->name()] = u->source.get();
                }
                for (const auto* unit : mod.units) {
                    if (!unit || !unit->syntaxTree || unit->isGenericSpecialization) continue;
                    const auto& srcName = unit->source->name();
                    const auto memberSrcName = [&](const auto& member) -> const std::string& {
                        return member.originSourceName.empty() ? srcName : member.originSourceName;
                    };
                    const auto memberSourceText = [&](const auto& member) -> const text::SourceText& {
                        if (!member.originSourceName.empty()) {
                            const auto it = unitSourcesByName.find(member.originSourceName);
                            if (it != unitSourcesByName.end() && it->second) return *it->second;
                        }
                        return *unit->source;
                    };
                    const auto checkRecordDeclSigs = [&](const auto& decls) {
                        for (const auto& decl : decls) {
                            if (!decl.typeParameters.empty()) continue;
                            const auto* owner = findOwnType(mod, decl.identifierToken.text);
                            const semantic::SymbolId ownerId = owner ? owner->id : 0;
                            const bool ownerObs = ownerId != 0 &&
                                globalObsoleteSymbols.find(ownerId) != globalObsoleteSymbols.end();
                            for (const auto& ifaceSyn : decl.interfaces) {
                                checkSignatureTypeObs(mod, srcName, ifaceSyn, ownerId, ownerObs);
                            }
                            for (const auto& fSyn : decl.fields) {
                                const bool fObs = ownerObs ||
                                    extractObsolete(fSyn.attributes, memberSourceText(fSyn), mod.visibleTypes).has_value();
                                checkSignatureTypeObs(mod, memberSrcName(fSyn), fSyn.type, ownerId, fObs);
                            }
                            for (const auto& pSyn : decl.properties) {
                                const bool pObs = ownerObs ||
                                    extractObsolete(pSyn.attributes, memberSourceText(pSyn), mod.visibleTypes).has_value();
                                checkSignatureTypeObs(mod, memberSrcName(pSyn), pSyn.type, ownerId, pObs);
                            }
                            for (const auto& cSyn : decl.constructors) {
                                const auto& memberSrc = memberSrcName(cSyn);
                                const bool cObs = ownerObs ||
                                    extractObsolete(cSyn.attributes, memberSourceText(cSyn), mod.visibleTypes).has_value();
                                for (const auto& param : cSyn.parameters) {
                                    checkSignatureTypeObs(mod, memberSrc, param.type, ownerId, cObs);
                                }
                            }
                            for (const auto& mSyn : decl.methods) {
                                if (!mSyn.typeParameters.empty()) continue;
                                const auto& memberSrc = memberSrcName(mSyn);
                                const bool mObs = ownerObs ||
                                    extractObsolete(mSyn.attributes, memberSourceText(mSyn), mod.visibleTypes).has_value();
                                checkSignatureTypeObs(mod, memberSrc, mSyn.returnType, ownerId, mObs);
                                for (const auto& param : mSyn.parameters) {
                                    checkSignatureTypeObs(mod, memberSrc, param.type, ownerId, mObs);
                                }
                            }
                        }
                    };
                    checkRecordDeclSigs(unit->syntaxTree->classes);
                    checkRecordDeclSigs(unit->syntaxTree->structs);
                    for (const auto& fn : unit->syntaxTree->functions) {
                        if (!fn.typeParameters.empty()) continue;
                        const bool fnObs =
                            extractObsolete(fn.attributes, *unit->source, mod.visibleTypes).has_value();
                        checkSignatureTypeObs(mod, srcName, fn.returnType, 0, fnObs);
                        for (const auto& param : fn.parameters) {
                            checkSignatureTypeObs(mod, srcName, param.type, 0, fnObs);
                        }
                    }
                }
            }
        }
    }

    // Reject recursive value-type layouts before lowering. A struct must have a
    // finite inline representation; reference fields may be recursive, value
    // fields may not.
    {
        struct StructOwner {
            ModuleWork* module = nullptr;
            const semantic::TypeSymbol* type = nullptr;
        };
        std::unordered_map<std::string, StructOwner> structs;
        for (auto& [moduleName, module] : modules) {
            (void)moduleName;
            if (module.frontendCached && !module.neededByDirtyModule) continue;
            for (const auto& type : module.types) {
                if (type.kind == semantic::TypeKind::Struct) {
                    structs.emplace(
                        semantic::canonicalTypeName(type),
                        StructOwner{&module, &type});
                }
            }
        }
        std::unordered_map<std::string, int> colors;
        std::unordered_set<std::string> reported;
        std::vector<std::string> stack;
        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            const auto found = structs.find(name);
            if (found == structs.end()) return;
            colors[name] = 1;
            stack.push_back(name);
            for (const auto& field : found->second.type->fields) {
                if (field.staticField ||
                    field.constField ||
                    field.type != semantic::PrimitiveType::Struct ||
                    field.typeName.empty()) {
                    continue;
                }
                const auto color = colors[field.typeName];
                if (color == 0) {
                    visit(field.typeName);
                } else if (color == 1 && reported.insert(field.typeName).second) {
                    std::ostringstream cycle;
                    auto begin = std::find(stack.begin(), stack.end(), field.typeName);
                    for (auto current = begin; current != stack.end(); ++current) {
                        if (current != begin) cycle << " -> ";
                        cycle << *current;
                    }
                    if (begin != stack.end()) cycle << " -> " << field.typeName;
                    result.diagnostics.report(
                        "RS2487",
                        "recursive struct layout is not allowed: " + cycle.str(),
                        {});
                    found->second.module->invalid = true;
                    const auto target = structs.find(field.typeName);
                    if (target != structs.end()) target->second.module->invalid = true;
                }
            }
            stack.pop_back();
            colors[name] = 2;
        };
        for (const auto& [name, owner] : structs) {
            (void)owner;
            if (colors[name] == 0) visit(name);
        }
    }

    // Re-propagate transitive [StackOnly] across all modules and validate field storage.
    {
        bool stackOnlyChanged = true;
        while (stackOnlyChanged) {
            stackOnlyChanged = false;
            for (auto& [moduleName, module] : modules) {
                (void)moduleName;
                if (module.frontendCached) continue;
                if (semantic::propagateStackOnlyTypes(module.types, module.visibleTypes)) {
                    stackOnlyChanged = true;
                }
            }
            if (stackOnlyChanged) {
                for (auto& [moduleName, module] : modules) {
                    (void)moduleName;
                    if (module.frontendCached) continue;
                    refreshVisibleTypes(modules, module);
                }
            }
        }
        for (auto& [moduleName, module] : modules) {
            (void)moduleName;
            if (module.frontendCached) continue;
            for (const auto& type : module.types) {
                if (type.kind != semantic::TypeKind::Class &&
                    type.kind != semantic::TypeKind::Struct) {
                    continue;
                }
                if (!semantic::validateStackOnlyFields(
                        type, module.visibleTypes, result.diagnostics)) {
                    module.invalid = true;
                }
            }
        }
    }

    // Member tables are part of the visible type descriptors.
    for (auto& [name, module] : modules) {
        (void)name;
        if (module.frontendCached) continue;
        refreshVisibleTypes(modules, module);
    }

    for (auto& [moduleName, module] : modules) {
        if (module.frontendCached) continue;
        std::ostringstream dependencies;
        for (const auto& importedModule : module.imports) {
            const auto imported = modules.find(importedModule);
            if (imported == modules.end()) {
                result.diagnostics.report(
                    "RS4001", "module '" + moduleName + "' imports missing module '" +
                        importedModule + "'", {});
                module.invalid = true;
                continue;
            }
            dependencies << importedModule << ':'
                << imported->second.publicFingerprint << '\n';
        }
        for (const auto& hygModule : module.hygienicModules) {
            const auto hyg = modules.find(hygModule);
            if (hyg != modules.end()) {
                dependencies << "hyg:" << hygModule << ':'
                    << hyg->second.publicFingerprint << '\n';
            }
        }
        module.dependencyFingerprint = stableFingerprint(dependencies.str());
    }
}

} // namespace realscript::compiler::detail
