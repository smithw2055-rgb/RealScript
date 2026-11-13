#pragma once

#include "realscript/game/GameBindings.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace realscript::game::rsbind_detail {

[[nodiscard]] GameApi::HeaderBindingGenerationResult generateFromParsedHeader(
    std::string_view headerContent,
    const std::string& defaultModuleName,
    const std::unordered_map<std::string, std::string>& predefinedMacros = {});

} // namespace realscript::game::rsbind_detail
