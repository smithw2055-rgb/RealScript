#include "realscript/runtime/Runtime.h"

#include "realscript/diagnostics/Diagnostic.h"

#include <utility>

namespace realscript::runtime {

bool BindingRegistry::bind(semantic::SymbolId symbolId, ExternalFunction function) {
    if (symbolId == 0 || !function) return false;
    return bySymbol_.emplace(symbolId, std::move(function)).second;
}

bool BindingRegistry::bind(const std::string& canonicalName, ExternalFunction function) {
    if (canonicalName.empty() || !function) return false;
    return byName_.emplace(canonicalName, std::move(function)).second;
}

std::optional<Value> BindingRegistry::invoke(
    const bytecode::FunctionReference& reference,
    const std::vector<Value>& arguments,
    RuntimeError& error) const {
    const auto bySymbol = bySymbol_.find(reference.symbolId);
    if (bySymbol != bySymbol_.end()) return bySymbol->second(reference, arguments, error);
    const auto byName = byName_.find(reference.name);
    if (byName != byName_.end()) return byName->second(reference, arguments, error);
    error.code = ErrorCode::ExternalFunctionUnresolved;
    error.message = "external function '" + reference.name + "' is not registered";
    return std::nullopt;
}

std::size_t BindingRegistry::size() const noexcept { return bySymbol_.size() + byName_.size(); }

ProgramImage::ProgramImage(std::vector<bytecode::Module> modules)
    : modules_(std::move(modules)) {
    for (const auto& module : modules_) {
        for (const auto& function : module.functions) {
            const auto qualified = module.name + "::" + function.name;
            functions_.emplace(function.symbolId, qualified);
            names_.emplace(qualified, function.symbolId);
        }
    }
}

std::optional<ProgramImage> ProgramImage::link(
    std::vector<bytecode::Module> modules,
    RuntimeError& error) {
    std::unordered_map<semantic::SymbolId, std::string> symbols;
    std::unordered_map<std::string, semantic::SymbolId> names;
    for (const auto& module : modules) {
        diagnostics::DiagnosticBag diagnostics;
        if (!bytecode::verifyModule(module, diagnostics)) {
            error.code = ErrorCode::InvalidProgram;
            error.message = "bytecode verification failed while linking module '" + module.name + "'";
            return std::nullopt;
        }
        for (const auto& function : module.functions) {
            const auto qualified = module.name + "::" + function.name;
            if (!symbols.emplace(function.symbolId, qualified).second) {
                error.code = ErrorCode::DuplicateSymbol;
                error.message = "duplicate function SymbolId while linking '" + qualified + "'";
                return std::nullopt;
            }
            if (!names.emplace(qualified, function.symbolId).second) {
                error.code = ErrorCode::DuplicateSymbol;
                error.message = "duplicate qualified function name '" + qualified + "'";
                return std::nullopt;
            }
        }
    }
    return ProgramImage(std::move(modules));
}

const std::vector<bytecode::Module>& ProgramImage::modules() const noexcept { return modules_; }
std::size_t ProgramImage::functionCount() const noexcept { return functions_.size(); }
std::size_t ProgramImage::moduleCount() const noexcept { return modules_.size(); }
bool ProgramImage::contains(semantic::SymbolId symbolId) const noexcept {
    return functions_.find(symbolId) != functions_.end();
}
std::optional<semantic::SymbolId> ProgramImage::findFunction(const std::string& qualifiedName) const {
    const auto found = names_.find(qualifiedName);
    if (found == names_.end()) return std::nullopt;
    return found->second;
}

EngineRuntime::EngineRuntime(std::shared_ptr<const ProgramImage> program)
    : program_(std::move(program)),
      heap_(std::make_shared<ManagedHeap>()) {}

void EngineRuntime::setBindings(std::shared_ptr<const BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
}

void EngineRuntime::setHeap(std::shared_ptr<ManagedHeap> heap) {
    heap_ = heap ? std::move(heap) : std::make_shared<ManagedHeap>();
}

std::shared_ptr<ManagedHeap> EngineRuntime::heap() const noexcept { return heap_; }

ExecutionResult EngineRuntime::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    if (!program_) {
        ExecutionResult result;
        result.error.code = ErrorCode::InvalidProgram;
        result.error.message = "runtime has no linked program image";
        return result;
    }
    Interpreter interpreter(program_, heap_);
    interpreter.setBindingRegistry(bindings_);
    return interpreter.invoke(qualifiedName, arguments, std::move(options));
}

const ProgramImage& EngineRuntime::program() const noexcept { return *program_; }

const char* traceEventKindName(TraceEventKind kind) noexcept {
    switch (kind) {
    case TraceEventKind::FunctionEnter: return "function-enter";
    case TraceEventKind::FunctionExit: return "function-exit";
    case TraceEventKind::Instruction: return "instruction";
    case TraceEventKind::Branch: return "branch";
    case TraceEventKind::ExternalCall: return "external-call";
    case TraceEventKind::GcStep: return "gc-step";
    case TraceEventKind::RuntimeError: return "runtime-error";
    }
    return "unknown";
}

} // namespace realscript::runtime
