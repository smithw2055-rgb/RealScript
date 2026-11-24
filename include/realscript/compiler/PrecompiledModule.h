#pragma once

#include "realscript/compiler/LanguageMetadata.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/text/Text.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace realscript::compiler {

namespace detail {
struct LazyFrontendPackStorage;
} // namespace detail

struct PrecompiledTypeRef {
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    std::string typeName;
};

struct PrecompiledInterfaceMethodContract {
    std::string name;
    PrecompiledTypeRef returnType;
    std::vector<PrecompiledTypeRef> parameters;
    std::vector<semantic::ParameterModifier> parameterModifiers;
    text::TextSpan declarationSpan;
    bool hasDefaultImplementation = false;
    std::string sourceName;
    std::set<std::string> hygienicModules;
};

struct PrecompiledInterfaceContract {
    std::string moduleName;
    std::string name;
    std::string sourceName;
    text::TextSpan declarationSpan;
    std::vector<std::string> baseInterfaces;
    std::vector<text::TextSpan> baseInterfaceSpans;
    std::vector<PrecompiledInterfaceMethodContract> methods;
};

struct PrecompiledDelegateContract {
    std::string moduleName;
    std::string name;
    std::string sourceName;
    PrecompiledTypeRef returnType;
    std::vector<PrecompiledTypeRef> parameters;
    std::vector<semantic::ParameterModifier> parameterModifiers;
    text::TextSpan declarationSpan;
};

struct PrecompiledGenericTemplateSource {
    std::string virtualPath;
    std::string sourceText;
};

struct PrecompiledModuleFrontend {
    std::string name;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t transitiveSourceFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    bool deterministic = false;
    std::vector<std::string> imports;
    std::vector<std::string> hygienicModules;
    std::vector<semantic::TypeSymbol> types;
    semantic::SharedTypeLayer typeLayer;
    std::vector<PrecompiledInterfaceContract> interfaces;
    std::vector<PrecompiledDelegateContract> delegates;
    std::vector<semantic::FunctionSymbol> declarations;
    std::vector<LanguageAttributeRecord> nativeAttributes;
    std::vector<LanguageSequenceRecord> nativeSequences;
    std::vector<LanguageInterfaceImplementation> nativeInterfaces;
    std::vector<LanguageGenericInstantiation> nativeGenericInstantiations;
    std::vector<PrecompiledGenericTemplateSource> genericTemplateSources;
};

struct PrecompiledFrontendPack {
    std::vector<PrecompiledModuleFrontend> modules;
    std::uint64_t contentHash = 0;
    std::shared_ptr<detail::LazyFrontendPackStorage> lazyStorage;

    [[nodiscard]] const PrecompiledModuleFrontend* findModule(
        std::string_view moduleName) const noexcept;
    [[nodiscard]] std::vector<std::string> moduleNames() const;

    [[nodiscard]] bool loadGenericTemplates(
        std::size_t moduleIndex,
        std::vector<PrecompiledGenericTemplateSource>& outTemplates) const;

    [[nodiscard]] std::size_t materializedTypeCount() const noexcept;
    [[nodiscard]] std::size_t totalIndexedTypeCount() const noexcept;
    void materializeAllTypes();
};

[[nodiscard]] std::vector<std::uint8_t> encodePrecompiledFrontendPack(
    const PrecompiledFrontendPack& pack);

[[nodiscard]] bool decodePrecompiledFrontendPack(
    const std::uint8_t* data,
    std::size_t size,
    PrecompiledFrontendPack& outPack,
    std::string* outError = nullptr,
    bool copyBlob = true,
    bool skipChecksum = false);

[[nodiscard]] inline bool decodePrecompiledFrontendPack(
    const std::vector<std::uint8_t>& bytes,
    PrecompiledFrontendPack& outPack,
    std::string* outError = nullptr,
    bool skipChecksum = false) {
    return decodePrecompiledFrontendPack(
        bytes.data(), bytes.size(), outPack, outError, /*copyBlob=*/true, skipChecksum);
}

} // namespace realscript::compiler
