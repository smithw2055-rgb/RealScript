#pragma once

#include "realscript/debug/DebugInfo.h"
#include "realscript/runtime/Runtime.h"

#include <cstddef>
#include <string>
#include <vector>

namespace realscript::debug {

struct DebugValueWindow {
    std::vector<runtime::Value>* values = nullptr;
    std::size_t offset = 0;
    std::size_t count = 0;

    [[nodiscard]] std::size_t size() const noexcept { return count; }
    [[nodiscard]] runtime::Value& operator[](std::size_t index) {
        return (*values)[offset + index];
    }
    [[nodiscard]] const runtime::Value& operator[](std::size_t index) const {
        return (*values)[offset + index];
    }
};

struct DebugFrameView {
    const bytecode::Module* module = nullptr;
    const bytecode::Function* function = nullptr;
    const SequencePoint* point = nullptr;
    DebugValueWindow arguments;
    DebugValueWindow locals;
    DebugValueWindow registers;
    bytecode::BlockId activeBlockId = 0;
};

class RuntimeDebugHook {
public:
    virtual ~RuntimeDebugHook() = default;

    virtual void bindProgram(const runtime::ProgramImage& program) = 0;
    virtual void beginExecution() = 0;
    virtual void endExecution() noexcept = 0;
    [[nodiscard]] virtual bool hasPendingInterruption() const noexcept = 0;
    [[nodiscard]] virtual bool onSequencePoint(
        const std::vector<DebugFrameView>& frames,
        runtime::ManagedHeap* heap) = 0;
    [[nodiscard]] virtual bool onException(
        const std::vector<DebugFrameView>& frames,
        runtime::ManagedHeap* heap,
        runtime::ErrorCode code,
        std::string exceptionId,
        std::string description,
        bool isUncaught,
        bool isRuntimeError) = 0;
};

} // namespace realscript::debug
