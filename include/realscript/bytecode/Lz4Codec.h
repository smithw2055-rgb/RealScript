#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace realscript::bytecode {

[[nodiscard]] std::size_t lz4CompressBound(std::size_t inputSize) noexcept;

[[nodiscard]] bool lz4CompressBlock(
    const std::uint8_t* src,
    std::size_t srcSize,
    std::vector<std::uint8_t>& outCompressed);

[[nodiscard]] bool lz4DecompressBlock(
    const std::uint8_t* src,
    std::size_t srcSize,
    std::uint8_t* dst,
    std::size_t expectedDstSize) noexcept;

[[nodiscard]] bool lz4DecompressBlock(
    const std::uint8_t* src,
    std::size_t srcSize,
    std::size_t expectedDstSize,
    std::vector<std::uint8_t>& outDecompressed);

[[nodiscard]] std::uint64_t fastChecksum64(
    const std::uint8_t* data,
    std::size_t size,
    std::uint64_t seed = 0u) noexcept;

} // namespace realscript::bytecode
