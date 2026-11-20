#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace realscript::syntax {

using TokenValue = std::variant<std::monostate, std::int64_t, double, bool, std::string>;

struct ExpressionSyntax;

} // namespace realscript::syntax
