#pragma once

#include "realscript/bindgen/BindGen.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace realscript::bindgen::rsbind_detail {

[[nodiscard]] HostApi::HeaderBindingGenerationResult generateFromParsedHeader(
    std::string_view headerContent,
    const std::string& defaultModuleName,
    const std::unordered_map<std::string, std::string>& predefinedMacros = {});

} // namespace realscript::bindgen::rsbind_detail
