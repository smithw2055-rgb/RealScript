#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace realscript::compiler {

struct LanguageAttributeArgument {
    std::string name;
    std::string value;
};

struct LanguageAttributeRecord {
    std::string target;
    std::string name;
    std::vector<LanguageAttributeArgument> arguments;
    std::string sourceName;
    std::size_t offset = 0;
};

struct LanguageInterfaceImplementation {
    std::string typeName;
    std::vector<std::string> interfaces;
};

struct LanguageGenericInstantiation {
    std::string genericName;
    std::vector<std::string> arguments;
    std::string generatedName;
};

struct LanguageSequenceRecord {
    std::string typeName;
    std::string name;
    std::string resultTypeName = "void";
    std::vector<std::string> callbacks;
    std::string sourceName;
    std::size_t offset = 0;
};

struct LanguageModuleMetadata {
    std::vector<LanguageAttributeRecord> attributes;
    std::vector<LanguageInterfaceImplementation> interfaces;
    std::vector<LanguageGenericInstantiation> genericInstantiations;
    std::vector<LanguageSequenceRecord> sequences;

    [[nodiscard]] bool empty() const noexcept {
        return attributes.empty() && interfaces.empty() &&
            genericInstantiations.empty() && sequences.empty();
    }
};

} // namespace realscript::compiler
