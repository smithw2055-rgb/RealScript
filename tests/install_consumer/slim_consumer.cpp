#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bindgen/BindGen.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/jit/StencilJit.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <memory>

static_assert(sizeof(realscript::runtime::Value) == 16u,
    "Slim consumer expects 16-byte Value ABI");
static_assert(sizeof(realscript::runtime::ObjectRef) == 16u,
    "Slim consumer expects 16-byte ObjectRef ABI");

int main() {
    auto heap = std::make_shared<realscript::runtime::ManagedHeap>();
    auto allocated = heap->allocateString("slim-install-ok");
    if (!allocated.has_value() || !heap->isAlive(*allocated)) {
        return 1;
    }

    realscript::runtime::Interpreter interpreter(std::vector<realscript::bytecode::Module>{});
    interpreter.setHeap(heap);
    if (interpreter.heap() != heap) {
        return 2;
    }

    realscript::bindgen::HostApi hostApi;
    if (!hostApi.function("Install.Slim", "Add",
            [](std::int64_t left, std::int64_t right) {
                return left + right;
            })) {
        return 3;
    }

    const bool stencilAvailable = realscript::jit::StencilJit::nativeAvailable();
    (void)stencilAvailable;

    realscript::hot_reload::ReloadOptions reloadOpts;
    if (!reloadOpts.allowAddedFunctions) {
        return 4;
    }

    if (realscript::aot::RuntimeAbiMajor == 0u) {
        return 5;
    }

    return 0;
}
