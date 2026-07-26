#pragma once

#include "realscript/debug/Debugger.h"
#include "realscript/tooling/Json.h"

#include <atomic>
#include <istream>
#include <memory>
#include <ostream>
#include <unordered_map>

namespace realscript::debug {

class DapServer {
public:
    explicit DapServer(
        std::shared_ptr<const runtime::ProgramImage> program,
        std::shared_ptr<runtime::ManagedHeap> heap = {});

    int run(std::istream& input, std::ostream& output);
    [[nodiscard]] tooling::Json handle(
        const tooling::Json& request,
        std::ostream* events = nullptr);

private:
    [[nodiscard]] tooling::Json response(
        const tooling::Json& request,
        bool success,
        tooling::Json body = nullptr,
        std::string message = {});
    void emitStopped(std::ostream& output);
    void emitTerminated(std::ostream& output);
    void rebuildVariableReferences();

    std::shared_ptr<const runtime::ProgramImage> program_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::unique_ptr<DebugSession> session_;
    std::atomic<std::uint32_t> sequence_{1};
    std::unordered_map<std::int64_t, std::vector<DebugVariable>> variables_;
};

} // namespace realscript::debug
