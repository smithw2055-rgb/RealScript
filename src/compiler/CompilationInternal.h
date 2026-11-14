#pragma once

#include "realscript/compiler/Compilation.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realscript::compiler::detail {

struct InterfaceTypeRef {
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    std::string typeName;
};

struct InterfaceMethodContract {
    std::string name;
    InterfaceTypeRef returnType;
    std::vector<InterfaceTypeRef> parameters;
    std::vector<semantic::ParameterModifier> parameterModifiers;
    text::TextSpan declarationSpan;
    bool hasDefaultImplementation = false;
    const syntax::InterfaceMethodDeclarationSyntax* methodSyntax = nullptr;
    std::string sourceName;
    std::set<std::string> hygienicModules;
};

struct InterfaceContract {
    std::string moduleName;
    std::string name;
    std::string sourceName;
    text::TextSpan declarationSpan;
    std::vector<InterfaceMethodContract> methods;
};

struct InterfaceDeclarationInput {
    const syntax::InterfaceDeclarationSyntax* syntax = nullptr;
    std::string sourceName;
    std::set<std::string> unitHygienicModules;
};

using InterfaceMap = std::map<std::string, InterfaceContract>;

struct DelegateContract {
    std::string moduleName;
    std::string name;
    std::string sourceName;
    InterfaceTypeRef returnType;
    std::vector<InterfaceTypeRef> parameters;
    std::vector<semantic::ParameterModifier> parameterModifiers;
    text::TextSpan declarationSpan;
};

struct DelegateDeclarationInput {
    const syntax::DelegateDeclarationSyntax* syntax = nullptr;
    std::string sourceName;
};

using DelegateMap = std::map<std::string, DelegateContract>;

struct ParsedUnit {
    std::shared_ptr<text::SourceText> source;
    std::shared_ptr<syntax::CompilationUnitSyntax> syntaxTree;
    std::string moduleName;
    std::vector<std::string> imports;
    std::vector<diagnostics::Diagnostic> parseDiagnostics;
    std::uint64_t sourceFingerprint = 0;
    bool invalid = false;
    bool deterministic = false;
    bool hasGenericSyntax = false;
    bool isGenericSpecialization = false;
    std::uint32_t genericDepth = 0;
    std::set<std::string> unitHygienicModules;
};

struct ModuleWork {
    std::string name;
    std::vector<ParsedUnit*> units;
    std::set<std::string> imports;
    std::set<std::string> hygienicModules;
    std::vector<semantic::TypeSymbol> types;
    semantic::TypeSymbolMap visibleTypes;
    std::vector<InterfaceDeclarationInput> interfaceInputs;
    InterfaceMap interfaces;
    InterfaceMap visibleInterfaces;
    std::vector<DelegateDeclarationInput> delegateInputs;
    DelegateMap delegates;
    DelegateMap visibleDelegates;
    std::vector<semantic::FunctionSymbol> declarations;
    std::vector<semantic::FunctionBindingInput> functionBindings;
    std::vector<std::unique_ptr<syntax::FunctionDeclarationSyntax>> synthesizedMethods;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t transitiveSourceFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    bool invalid = false;
    bool deterministic = false;
    bool frontendCached = false;
    bool neededByDirtyModule = false;
};

struct CachedModuleFrontend {
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t transitiveSourceFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    std::vector<semantic::TypeSymbol> types;
    InterfaceMap interfaces;
    DelegateMap delegates;
    std::vector<semantic::FunctionSymbol> declarations;
    std::vector<LanguageAttributeRecord> nativeAttributes;
    std::vector<LanguageSequenceRecord> nativeSequences;
    std::vector<LanguageInterfaceImplementation> nativeInterfaces;
    std::vector<std::shared_ptr<syntax::CompilationUnitSyntax>> keptSyntaxTrees;
};

template <typename T>
const T& declarationReference(const T& value) noexcept {
    return value;
}

template <typename T>
const T& declarationReference(const T* value) noexcept {
    return *value;
}

[[nodiscard]] inline std::uint64_t combineFingerprint(
    std::uint64_t seed,
    std::uint64_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ull +
        (seed << 6) + (seed >> 2);
    return seed;
}

// Shared module visibility & metadata helpers (defined in TypeHierarchyAndContracts.cpp)
void addVisibleType(
    semantic::TypeSymbolMap& map,
    const semantic::TypeSymbol& type,
    bool includeSimpleName);
void refreshVisibleTypes(
    std::map<std::string, ModuleWork>& modules,
    ModuleWork& module);
[[nodiscard]] std::string attributeValueText(
    const syntax::AttributeArgumentSyntax& argument,
    const text::SourceText& source);
void appendNativeAttributes(
    std::vector<LanguageAttributeRecord>& output,
    const std::vector<syntax::AttributeListSyntax>& lists,
    const std::string& target,
    const text::SourceText& source);
[[nodiscard]] std::string memberAttributeTarget(
    const std::string& owner,
    const std::string& kind,
    const std::string& name,
    std::size_t arity = 0);
[[nodiscard]] std::string canonicalInterfaceName(
    const std::string& moduleName,
    const std::string& name);
[[nodiscard]] std::string canonicalDelegateName(
    const std::string& moduleName,
    const std::string& name);
void refreshVisibleDelegates(
    std::map<std::string, ModuleWork>& modules,
    ModuleWork& module);
void refreshVisibleInterfaces(
    std::map<std::string, ModuleWork>& modules,
    ModuleWork& module);
void appendStampedDiagnostics(
    diagnostics::DiagnosticBag& target,
    const diagnostics::DiagnosticBag& sourceBag,
    const std::string& defaultSourceName);
[[nodiscard]] InterfaceTypeRef resolveInterfaceType(
    const syntax::TypeSyntax& syntaxTree,
    const semantic::TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    const std::string& sourceName,
    bool allowVoid);
[[nodiscard]] bool sameInterfaceType(
    const InterfaceTypeRef& expected,
    semantic::PrimitiveType actual,
    const std::string& actualName);
[[nodiscard]] std::string interfaceMethodSignature(
    const InterfaceMethodContract& method);
[[nodiscard]] std::string fieldTypeSignature(const semantic::FieldSymbol& field);
[[nodiscard]] std::string typeSignature(const semantic::TypeSymbol& type);
[[nodiscard]] semantic::TypeSymbol* findOwnType(
    ModuleWork& module,
    const std::string& name);
[[nodiscard]] LanguageModuleMetadata languageMetadataForModule(
    const std::string& moduleName,
    const mir::Module& module,
    const BuildResult& result);
[[nodiscard]] debug::SourceFileInfo makeSourceFileInfo(
    const text::SourceText& source,
    debug::SourceFileId id);
void appendParameters(
    semantic::FunctionBindingInput& binding,
    const std::vector<syntax::ParameterSyntax>& parameters);

// Class inheritance & virtual table helpers (defined in TypeHierarchyAndContracts.cpp)
[[nodiscard]] semantic::TypeSymbol* findOwnClassOrType(
    ModuleWork& module,
    const std::string& name);
[[nodiscard]] const semantic::TypeSymbol* findGlobalType(
    const std::map<std::string, ModuleWork>& modules,
    const std::string& canonicalName);
void resolveClassInheritance(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result);
void applyInheritedFieldLayouts(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result);
[[nodiscard]] std::size_t visibleMethodParameterOffset(
    const semantic::FunctionSymbol& function) noexcept;
[[nodiscard]] bool sameInheritedMethodSignature(
    const semantic::FunctionSymbol& left,
    const semantic::FunctionSymbol& right);
[[nodiscard]] std::optional<std::size_t> prepareVirtualMethod(
    semantic::TypeSymbol& owner,
    semantic::FunctionSymbol& method,
    const syntax::FunctionDeclarationSyntax& syntaxTree,
    ModuleWork& module,
    BuildResult& result,
    const std::string& sourceName);
void validateConcreteClassType(
    const semantic::TypeSymbol& owner,
    ModuleWork& module,
    BuildResult& result);

// Builtin standard library modules (defined in BuiltinModules.cpp)
[[nodiscard]] const std::string& builtinDeterministicMathModuleSource();
[[nodiscard]] const std::string& builtinAsyncModuleSource();
[[nodiscard]] bool sourceMentionsAsyncPrimitives(std::string_view text);
[[nodiscard]] const std::string& builtinFiberModuleSource();
[[nodiscard]] const std::string& builtinResourceModuleSource();
[[nodiscard]] bool sourceMentionsResourcePrimitives(std::string_view text);
[[nodiscard]] const std::string& builtinWeakModuleSource();
[[nodiscard]] const std::string& builtinMemoryModuleSource();

// Partial record merging & partial method reconciliation (defined in Compilation.cpp)
struct PartialUnitEntry {
    syntax::CompilationUnitSyntax* syntaxTree = nullptr;
    std::string sourceName;
};

struct MergedGenericPartialPart {
    std::string sourceName;
    text::TextSpan span;
};

struct MergedPartialTypeOriginInfo {
    std::vector<MergedGenericPartialPart> parts;
};

void mergePartialRecordDeclarations(
    const std::vector<PartialUnitEntry>& units,
    diagnostics::DiagnosticBag& diagnostics,
    bool& moduleInvalid,
    std::unordered_map<std::string, MergedPartialTypeOriginInfo>* outMultiPartRecords = nullptr);

// Pipeline stage passes
void specializeNativeGenerics(
    std::vector<std::unique_ptr<ParsedUnit>>& units,
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result);

void discoverTypesContractsAndLayouts(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result);

void synthesizeDeclarationsSequencesAndMembers(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result);

void closeGlobalInterfacesValidateAttributesAndLayouts(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result,
    std::unordered_map<semantic::SymbolId, semantic::ObsoleteInfo>& globalObsoleteSymbols);

} // namespace realscript::compiler::detail
