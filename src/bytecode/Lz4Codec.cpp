#include "realscript/bytecode/Lz4Codec.h"

#include <cstring>
#include <limits>

namespace realscript::bytecode {
namespace {

constexpr std::size_t kHashLog = 12u; // 4096 entries -> 16 KB stack table
constexpr std::size_t kHashSize = std::size_t{1} << kHashLog;
constexpr std::size_t kMinMatch = 4u;
constexpr std::size_t kLastLiterals = 5u;
constexpr std::size_t kMfLimit = 12u; // minimum input size before matching stops
constexpr std::size_t kMaxDistance = 65535u;
constexpr std::uint32_t kInvalidHashPos = 0xffffffffu;

[[nodiscard]] inline std::uint32_t readU32Unaligned(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0u;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

[[nodiscard]] inline std::uint32_t hash4(std::uint32_t seq) noexcept {
    return static_cast<std::uint32_t>((seq * 2654435761u) >> (32u - kHashLog));
}

inline void writeLengthBytes(std::vector<std::uint8_t>& out, std::size_t lenMinus15) {
    while (lenMinus15 >= 255u) {
        out.push_back(255u);
        lenMinus15 -= 255u;
    }
    out.push_back(static_cast<std::uint8_t>(lenMinus15));
}

inline void writeLiteralsSequence(
    std::vector<std::uint8_t>& out,
    const std::uint8_t* litPtr,
    std::size_t litLen) {
    if (litLen >= 15u) {
        out.push_back(0xF0u);
        writeLengthBytes(out, litLen - 15u);
    } else {
        out.push_back(static_cast<std::uint8_t>(litLen << 4u));
    }
    if (litLen > 0u) {
        out.insert(out.end(), litPtr, litPtr + litLen);
    }
}

} // namespace

std::size_t lz4CompressBound(std::size_t inputSize) noexcept {
    if (inputSize == 0u) {
        return 0u;
    }
    const std::size_t overhead = (inputSize / 255u) + 16u;
    if (inputSize > (std::numeric_limits<std::size_t>::max)() - overhead) {
        return 0u;
    }
    return inputSize + overhead;
}

bool lz4CompressBlock(
    const std::uint8_t* src,
    std::size_t srcSize,
    std::vector<std::uint8_t>& outCompressed) {
    outCompressed.clear();
    if (srcSize == 0u) {
        return true;
    }
    if (!src) {
        return false;
    }
    if (srcSize > static_cast<std::size_t>(0x7E000000u)) {
        return false;
    }

    const std::size_t bound = lz4CompressBound(srcSize);
    if (bound == 0u) {
        return false;
    }
    outCompressed.reserve(bound);

    if (srcSize < kMfLimit) {
        writeLiteralsSequence(outCompressed, src, srcSize);
        return true;
    }

    std::uint32_t hashTable[kHashSize];
    for (std::size_t i = 0u; i < kHashSize; ++i) {
        hashTable[i] = kInvalidHashPos;
    }

    const std::size_t mfLimitIdx = srcSize - kMfLimit;
    const std::size_t matchLimitIdx = srcSize - kLastLiterals;

    std::size_t anchor = 0u;
    std::size_t ip = 1u;

    // Seed position 0
    hashTable[hash4(readU32Unaligned(src))] = 0u;

    while (ip <= mfLimitIdx) {
        std::size_t refIdx = 0u;
        bool foundMatch = false;

        // Acceleration search loop similar to LZ4 fast mode
        std::size_t forwardIp = ip;
        std::size_t step = 1u;
        std::size_t searchMatchNb = 64u;
        while (true) {
            ip = forwardIp;
            const std::uint32_t curSeq = readU32Unaligned(src + ip);
            const std::uint32_t h = hash4(curSeq);
            const std::uint32_t candidate = hashTable[h];
            hashTable[h] = static_cast<std::uint32_t>(ip);

            if (candidate != kInvalidHashPos &&
                ip > static_cast<std::size_t>(candidate) &&
                (ip - static_cast<std::size_t>(candidate)) <= kMaxDistance &&
                readU32Unaligned(src + candidate) == curSeq) {
                refIdx = static_cast<std::size_t>(candidate);
                foundMatch = true;
                break;
            }

            forwardIp = ip + step;
            step = searchMatchNb++ >> 6u;
            if (forwardIp > mfLimitIdx) {
                break;
            }
        }

        if (!foundMatch) {
            break;
        }

        // Catch up backward while both anchor & refIdx > 0
        while (ip > anchor && refIdx > 0u && src[ip - 1u] == src[refIdx - 1u]) {
            --ip;
            --refIdx;
        }

        const std::size_t litLen = ip - anchor;
        const std::uint16_t offset = static_cast<std::uint16_t>(ip - refIdx);

        // Extend match forward within matchLimitIdx
        std::size_t matchEnd = ip + kMinMatch;
        std::size_t refEnd = refIdx + kMinMatch;
        while (matchEnd + 4u <= matchLimitIdx &&
               readU32Unaligned(src + matchEnd) == readU32Unaligned(src + refEnd)) {
            matchEnd += 4u;
            refEnd += 4u;
        }
        while (matchEnd < matchLimitIdx && src[matchEnd] == src[refEnd]) {
            ++matchEnd;
            ++refEnd;
        }

        const std::size_t matchLenMinus4 = (matchEnd - ip) - kMinMatch;

        // Encode token byte
        const std::uint8_t litToken = litLen >= 15u
            ? 0xF0u
            : static_cast<std::uint8_t>(litLen << 4u);
        const std::uint8_t matchToken = matchLenMinus4 >= 15u
            ? 0x0Fu
            : static_cast<std::uint8_t>(matchLenMinus4);
        outCompressed.push_back(static_cast<std::uint8_t>(litToken | matchToken));

        if (litLen >= 15u) {
            writeLengthBytes(outCompressed, litLen - 15u);
        }
        if (litLen > 0u) {
            outCompressed.insert(outCompressed.end(), src + anchor, src + anchor + litLen);
        }

        // Little-endian 16-bit offset
        outCompressed.push_back(static_cast<std::uint8_t>(offset & 0xFFu));
        outCompressed.push_back(static_cast<std::uint8_t>((offset >> 8u) & 0xFFu));

        if (matchLenMinus4 >= 15u) {
            writeLengthBytes(outCompressed, matchLenMinus4 - 15u);
        }

        ip = matchEnd;
        anchor = ip;

        if (ip > mfLimitIdx) {
            break;
        }

        // Fill table at ip - 2 to improve match density around sequence boundary
        if (ip >= 2u) {
            hashTable[hash4(readU32Unaligned(src + ip - 2u))] =
                static_cast<std::uint32_t>(ip - 2u);
        }
        // Check immediate next match at current ip before continuing
        const std::uint32_t nextSeq = readU32Unaligned(src + ip);
        const std::uint32_t nextHash = hash4(nextSeq);
        hashTable[nextHash] = static_cast<std::uint32_t>(ip);
        ++ip;
    }

    // Encode trailing literals
    const std::size_t trailingLiterals = srcSize - anchor;
    writeLiteralsSequence(outCompressed, src + anchor, trailingLiterals);
    return true;
}

bool lz4DecompressBlock(
    const std::uint8_t* src,
    std::size_t srcSize,
    std::uint8_t* dst,
    std::size_t expectedDstSize) noexcept {
    if (expectedDstSize == 0u) {
        return srcSize == 0u;
    }
    if (!src || srcSize == 0u || !dst) {
        return false;
    }

    const std::uint8_t* ip = src;
    const std::uint8_t* const iend = src + srcSize;
    std::uint8_t* op = dst;
    std::uint8_t* const oend = dst + expectedDstSize;

    while (ip < iend) {
        const std::uint8_t token = *ip++;
        std::size_t litLen = static_cast<std::size_t>(token >> 4u);

        if (litLen == 15u) {
            std::uint8_t s = 0u;
            do {
                if (ip >= iend) {
                    return false;
                }
                s = *ip++;
                if (litLen > (std::numeric_limits<std::size_t>::max)() - static_cast<std::size_t>(s)) {
                    return false;
                }
                litLen += static_cast<std::size_t>(s);
            } while (s == 255u);
        }

        if (litLen > static_cast<std::size_t>(iend - ip) ||
            litLen > static_cast<std::size_t>(oend - op)) {
            return false;
        }
        if (litLen > 0u) {
            std::memcpy(op, ip, litLen);
            ip += litLen;
            op += litLen;
        }

        // In standard LZ4 blocks, the final sequence omits offset & match length
        if (ip == iend) {
            break;
        }

        // Remaining block must have at least a 2-byte offset
        if (static_cast<std::size_t>(iend - ip) < 2u) {
            return false;
        }
        const std::size_t offset =
            static_cast<std::size_t>(ip[0]) | (static_cast<std::size_t>(ip[1]) << 8u);
        ip += 2u;

        if (offset == 0u || offset > static_cast<std::size_t>(op - dst)) {
            return false;
        }
        const std::uint8_t* match = op - offset;

        std::size_t matchLen = static_cast<std::size_t>(token & 0x0Fu);
        if (matchLen == 15u) {
            std::uint8_t s = 0u;
            do {
                if (ip >= iend) {
                    return false;
                }
                s = *ip++;
                if (matchLen > (std::numeric_limits<std::size_t>::max)() - static_cast<std::size_t>(s)) {
                    return false;
                }
                matchLen += static_cast<std::size_t>(s);
            } while (s == 255u);
        }
        if (matchLen > (std::numeric_limits<std::size_t>::max)() - kMinMatch) {
            return false;
        }
        matchLen += kMinMatch;

        if (matchLen > static_cast<std::size_t>(oend - op)) {
            return false;
        }

        if (offset >= matchLen) {
            std::memcpy(op, match, matchLen);
            op += matchLen;
        } else {
            for (std::size_t i = 0u; i < matchLen; ++i) {
                op[i] = match[i];
            }
            op += matchLen;
        }
    }

    return ip == iend && op == oend;
}

bool lz4DecompressBlock(
    const std::uint8_t* src,
    std::size_t srcSize,
    std::size_t expectedDstSize,
    std::vector<std::uint8_t>& outDecompressed) {
    outDecompressed.clear();
    if (expectedDstSize == 0u) {
        return srcSize == 0u;
    }
    if (!src || srcSize == 0u) {
        return false;
    }
    outDecompressed.resize(expectedDstSize);
    if (!lz4DecompressBlock(src, srcSize, outDecompressed.data(), expectedDstSize)) {
        outDecompressed.clear();
        return false;
    }
    return true;
}

std::uint64_t fastChecksum64(
    const std::uint8_t* data,
    std::size_t size,
    std::uint64_t seed) noexcept {
    constexpr std::uint64_t kC0 = 0x9e3779b97f4a7c15ull;
    constexpr std::uint64_t kC1 = 0xc2b2ae3d27d4eb4full;
    constexpr std::uint64_t kC2 = 0x165667b19e3779f9ull;
    constexpr std::uint64_t kC3 = 0x85ebca77c2b2ae63ull;
    constexpr std::uint64_t kM1 = 0xbf58476d1ce4e5b9ull;
    constexpr std::uint64_t kM2 = 0x94d049bb133111ebull;

    std::uint64_t h0 = seed ^ (static_cast<std::uint64_t>(size) * kC0 + kC1);
    std::uint64_t h1 = seed + kC1;
    std::uint64_t h2 = seed ^ kC2;
    std::uint64_t h3 = seed - kC3;

    if (data && size > 0u) {
        std::size_t i = 0u;
        while (i + 32u <= size) {
            std::uint64_t w0 = 0u;
            std::uint64_t w1 = 0u;
            std::uint64_t w2 = 0u;
            std::uint64_t w3 = 0u;
            std::memcpy(&w0, data + i, 8u);
            std::memcpy(&w1, data + i + 8u, 8u);
            std::memcpy(&w2, data + i + 16u, 8u);
            std::memcpy(&w3, data + i + 24u, 8u);
            h0 = (h0 ^ (w0 * kC0)) * kC1;
            h0 = (h0 << 27) | (h0 >> 37);
            h1 = (h1 ^ (w1 * kC1)) * kC2;
            h1 = (h1 << 31) | (h1 >> 33);
            h2 = (h2 ^ (w2 * kC2)) * kC3;
            h2 = (h2 << 29) | (h2 >> 35);
            h3 = (h3 ^ (w3 * kC3)) * kC0;
            h3 = (h3 << 23) | (h3 >> 41);
            i += 32u;
        }
        while (i + 8u <= size) {
            std::uint64_t w = 0u;
            std::memcpy(&w, data + i, 8u);
            h0 = ((h0 ^ (w * kC2)) * kC1);
            h0 = (h0 << 27) | (h0 >> 37);
            i += 8u;
        }
        if (i < size) {
            std::uint64_t tail = 0u;
            std::memcpy(&tail, data + i, size - i);
            h1 = (h1 ^ (tail * kC3)) * kC0;
        }
    }

    std::uint64_t h = h0 ^ (h1 * kC0) ^ ((h2 << 17) | (h2 >> 47)) ^ (h3 * kC2);
    h ^= static_cast<std::uint64_t>(size);
    h ^= h >> 30;
    h *= kM1;
    h ^= h >> 27;
    h *= kM2;
    h ^= h >> 31;
    return h;
}

} // namespace realscript::bytecode
