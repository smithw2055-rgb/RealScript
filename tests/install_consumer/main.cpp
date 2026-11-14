#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript_aot_generated.h"

#include <cstdint>
#include <iostream>
#include <variant>

int main() {
    realscript::aot::Program program(realscript_generated::InstallConsumerProgram());
    const auto result = program.invoke("Install.Consumer::main");
    if (!result.succeeded || !std::holds_alternative<std::int64_t>(result.value) ||
        std::get<std::int64_t>(result.value) != 42) {
        std::cerr << "Install.Consumer::main failed: " << result.error.message << "\n";
        return 1;
    }
    return 0;
}
