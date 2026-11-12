#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace realscript::tooling {

struct DifferentialFuzzOptions {
    std::uint64_t seed = 0x525350335332ULL;
    std::size_t programCount = 20;
    std::size_t inputsPerFunction = 6;
    std::uint64_t instructionBudget = 200000;
};

struct DifferentialFuzzReport {
    std::size_t programsGenerated = 0;
    std::size_t functionsTested = 0;
    std::size_t invocationsExecuted = 0;
    std::size_t nativeX64FunctionsCompiled = 0;
    std::size_t threadedFunctionsCompiled = 0;
    std::size_t runtimeErrorsMatched = 0;
    std::vector<std::string> failures;

    [[nodiscard]] bool succeeded() const noexcept {
        return failures.empty() && programsGenerated > 0 && invocationsExecuted > 0;
    }
};

struct BytecodeMutationFuzzOptions {
    std::uint64_t seed = 0x52534243465AULL;
    std::size_t mutationsPerSeedModule = 128;
};

struct BytecodeMutationFuzzReport {
    std::size_t totalMutations = 0;
    std::size_t bitFlipMutations = 0;
    std::size_t truncationMutations = 0;
    std::size_t headerVersionMutations = 0;
    std::size_t outOfBoundsRegisterMutations = 0;
    std::size_t outOfBoundsBlockMutations = 0;
    std::size_t rejectedSafelyCount = 0;
    std::size_t validAfterBenignMutationCount = 0;
    std::vector<std::string> failures;

    [[nodiscard]] bool succeeded() const noexcept {
        return failures.empty() && totalMutations > 0;
    }
};

[[nodiscard]] std::string generateRandomProgram(
    std::uint64_t seed,
    std::size_t programIndex = 0);

[[nodiscard]] DifferentialFuzzReport runDifferentialFuzz(
    const DifferentialFuzzOptions& options = {});

[[nodiscard]] BytecodeMutationFuzzReport runBytecodeMutationFuzz(
    const BytecodeMutationFuzzOptions& options = {});

} // namespace realscript::tooling
