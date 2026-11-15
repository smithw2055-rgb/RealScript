#pragma once

#include "realscript/compiler/Compilation.h"

#include <memory>

namespace realscript::compiler {

// Creates the built-in L0 standard reactive/MVVM attribute macro plugin
// supporting `[Signal]` (on `partial void` methods inside `partial class` /
// `partial struct`) and `[RelayCommand]` (with optional `CanExecute` target on
// methods inside `partial class` / `partial struct`).
[[nodiscard]] std::shared_ptr<const ICompilerMacroPlugin>
createStandardReactiveMacroPlugin();

} // namespace realscript::compiler
