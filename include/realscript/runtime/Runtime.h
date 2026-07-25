#pragma once

#include "realscript/bytecode/Bytecode.h"
#include "realscript/semantic/Semantic.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace realscript::runtime {

struct NullString {
    friend constexpr bool operator==(NullString, NullString) noexcept { return true; }
};

using Value = std::variant<std::monostate, NullString, bool, std::int64_t, std::string>;

enum class ErrorCode {
    None,
    FunctionNotFound,
    InvalidArguments,
    TypeMismatch,
    DivisionByZero,
    IntegerOverflow,
    InstructionBudgetExceeded,
    RecursionLimitExceeded,
    ExternalFunctionUnresolved,
    DuplicateSymbol,
    InvalidProgram,
};

struct RuntimeError {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::vector<std::string> stackTrace;
};

struct Limits {
    std::uint64_t instructionBudget = 1'000'000;
    std::size_t recursionLimit = 256;
};

struct RuntimeStatistics {
    std::uint64_t instructionsExecuted = 0;
    std::uint64_t functionCalls = 0;
    std::uint64_t externalCalls = 0;
    std::uint64_t branchesTaken = 0;
    std::size_t maximumCallDepth = 0;
};

enum class TraceEventKind {
    FunctionEnter,
    FunctionExit,
    Instruction,
    Branch,
    ExternalCall,
    RuntimeError,
};

struct TraceEvent {
    TraceEventKind kind = TraceEventKind::Instruction;
    std::string function;
    std::string operation;
    std::uint64_t instructionIndex = 0;
    std::size_t callDepth = 0;
};

using TraceSink = std::function<void(const TraceEvent&)>;

struct ExecutionOptions {
    Limits limits;
    TraceSink trace;
};

struct ExecutionResult {
    bool succeeded = false;
    Value value;
    RuntimeError error;
    std::uint64_t instructionsExecuted = 0;
    RuntimeStatistics statistics;
};

using ExternalFunction = std::function<std::optional<Value>(
    const bytecode::FunctionReference&,
    const std::vector<Value>&,
    RuntimeError&)>;

class BindingRegistry {
public:
    bool bind(semantic::SymbolId symbolId, ExternalFunction function);
    bool bind(const std::string& canonicalName, ExternalFunction function);
    [[nodiscard]] std::optional<Value> invoke(
        const bytecode::FunctionReference& reference,
        const std::vector<Value>& arguments,
        RuntimeError& error) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<semantic::SymbolId, ExternalFunction> bySymbol_;
    std::unordered_map<std::string, ExternalFunction> byName_;
};

class ProgramImage {
public:
    static std::optional<ProgramImage> link(
        std::vector<bytecode::Module> modules,
        RuntimeError& error);

    [[nodiscard]] const std::vector<bytecode::Module>& modules() const noexcept;
    [[nodiscard]] std::size_t functionCount() const noexcept;
    [[nodiscard]] std::size_t moduleCount() const noexcept;
    [[nodiscard]] bool contains(semantic::SymbolId symbolId) const noexcept;
    [[nodiscard]] std::optional<semantic::SymbolId> findFunction(
        const std::string& qualifiedName) const;

private:
    explicit ProgramImage(std::vector<bytecode::Module> modules);

    std::vector<bytecode::Module> modules_;
    std::unordered_map<semantic::SymbolId, std::string> functions_;
    std::unordered_map<std::string, semantic::SymbolId> names_;
};

class Interpreter {
public:
    explicit Interpreter(std::vector<bytecode::Module> modules);
    explicit Interpreter(std::shared_ptr<const ProgramImage> program);

    void setExternalResolver(ExternalFunction resolver);
    void setBindingRegistry(std::shared_ptr<const BindingRegistry> bindings);

    [[nodiscard]] ExecutionResult invoke(
        semantic::SymbolId symbolId,
        const std::vector<Value>& arguments = {},
        Limits limits = {}) const;
    [[nodiscard]] ExecutionResult invoke(
        semantic::SymbolId symbolId,
        const std::vector<Value>& arguments,
        ExecutionOptions options) const;
    [[nodiscard]] ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<Value>& arguments = {},
        Limits limits = {}) const;
    [[nodiscard]] ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<Value>& arguments,
        ExecutionOptions options) const;

private:
    std::vector<bytecode::Module> modules_;
    std::shared_ptr<const ProgramImage> program_;
    ExternalFunction externalResolver_;
    std::shared_ptr<const BindingRegistry> bindings_;
};

class EngineRuntime {
public:
    explicit EngineRuntime(std::shared_ptr<const ProgramImage> program);

    void setBindings(std::shared_ptr<const BindingRegistry> bindings);
    [[nodiscard]] ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<Value>& arguments = {},
        ExecutionOptions options = {}) const;
    [[nodiscard]] const ProgramImage& program() const noexcept;

private:
    std::shared_ptr<const ProgramImage> program_;
    std::shared_ptr<const BindingRegistry> bindings_;
};

[[nodiscard]] const char* errorCodeName(ErrorCode code) noexcept;
[[nodiscard]] const char* traceEventKindName(TraceEventKind kind) noexcept;
[[nodiscard]] semantic::PrimitiveType valueType(const Value& value) noexcept;
[[nodiscard]] std::string valueToString(const Value& value);

} // namespace realscript::runtime
