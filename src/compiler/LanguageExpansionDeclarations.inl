#include "LanguageExpansionDeclarationsPart01.inl"
#include "LanguageExpansionDeclarationsPart02.inl"

// Retained for isolated expansion compatibility. Compilation-level expansion
// uses the interface-aware collector in LanguageExpansion.cpp.
[[maybe_unused]] auto* const legacyCollectGenericDeclarations =
    &collectGenericDeclarations;
