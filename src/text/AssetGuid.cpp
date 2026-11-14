#include "realscript/text/AssetGuid.h"

#include <array>
#include <cstddef>
#include <vector>

namespace realscript::text {
namespace {

constexpr std::array<std::uint8_t, 16> kRealScriptUuidNamespace{{
    0x6bu, 0xa7u, 0xb8u, 0x10u,
    0x9du, 0xadu, 0x11u, 0xd1u,
    0x80u, 0xb4u, 0x00u, 0xc0u,
    0x4fu, 0xd4u, 0x30u, 0xc8u,
}};

constexpr std::uint32_t rotl32(std::uint32_t v, unsigned int n) noexcept {
    return (v << n) | (v >> (32u - n));
}

std::array<std::uint8_t, 20> computeSha1(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h0 = 0x67452301u;
    std::uint32_t h1 = 0xEFCDAB89u;
    std::uint32_t h2 = 0x98BADCFEu;
    std::uint32_t h3 = 0x10325476u;
    std::uint32_t h4 = 0xC3D2E1F0u;

    const std::uint64_t bitLen = static_cast<std::uint64_t>(len) * 8ULL;
    std::vector<std::uint8_t> msg;
    msg.reserve(((len + 9u + 63u) / 64u) * 64u);
    if (len > 0u && data != nullptr) {
        msg.insert(msg.end(), data, data + len);
    }
    msg.push_back(0x80u);
    while ((msg.size() % 64u) != 56u) {
        msg.push_back(0x00u);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bitLen >> (i * 8)) & 0xFFu));
    }

    std::uint32_t w[80]{};
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64u) {
        for (std::size_t i = 0; i < 16u; ++i) {
            const std::size_t off = chunk + i * 4u;
            w[i] = (static_cast<std::uint32_t>(msg[off]) << 24u) |
                   (static_cast<std::uint32_t>(msg[off + 1u]) << 16u) |
                   (static_cast<std::uint32_t>(msg[off + 2u]) << 8u) |
                   static_cast<std::uint32_t>(msg[off + 3u]);
        }
        for (std::size_t i = 16u; i < 80u; ++i) {
            w[i] = rotl32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
        }

        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;

        for (std::size_t i = 0; i < 80u; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20u) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40u) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60u) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t temp = rotl32(a, 5u) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30u);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest{};
    const std::uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < 5u; ++i) {
        digest[i * 4u + 0u] = static_cast<std::uint8_t>((words[i] >> 24u) & 0xFFu);
        digest[i * 4u + 1u] = static_cast<std::uint8_t>((words[i] >> 16u) & 0xFFu);
        digest[i * 4u + 2u] = static_cast<std::uint8_t>((words[i] >> 8u) & 0xFFu);
        digest[i * 4u + 3u] = static_cast<std::uint8_t>(words[i] & 0xFFu);
    }
    return digest;
}

int parseHexNibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string_view trimAscii(std::string_view sv) noexcept {
    while (!sv.empty() &&
           (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

std::string AssetGuid::normalizeAssetPath(std::string_view rawPath) {
    rawPath = trimAscii(rawPath);
    std::string normalized;
    normalized.reserve(rawPath.size());
    for (const char ch : rawPath) {
        const char mapped = (ch == '\\') ? '/' : ch;
        if (mapped == '/' && !normalized.empty() && normalized.back() == '/') {
            continue;
        }
        normalized.push_back(mapped);
    }
    while (normalized.size() >= 2u && normalized[0] == '.' && normalized[1] == '/') {
        normalized.erase(0, 2u);
    }
    while (normalized.size() > 1u && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

AssetGuid AssetGuid::fromCanonicalName(
    std::string_view domain,
    std::string_view canonicalName) {
    canonicalName = trimAscii(canonicalName);
    if (canonicalName.empty()) {
        return AssetGuid::nil();
    }
    std::vector<std::uint8_t> buf;
    buf.reserve(
        kRealScriptUuidNamespace.size() +
        (domain.empty() ? 0u : (domain.size() + 1u)) +
        canonicalName.size());
    buf.insert(buf.end(), kRealScriptUuidNamespace.begin(), kRealScriptUuidNamespace.end());
    if (!domain.empty()) {
        buf.insert(buf.end(), domain.begin(), domain.end());
        buf.push_back(static_cast<std::uint8_t>(':'));
    }
    buf.insert(buf.end(), canonicalName.begin(), canonicalName.end());

    auto digest = computeSha1(buf.data(), buf.size());
    // RFC-4122 Version 5 (SHA-1 name-based) & Variant 1 (10xx)
    digest[6] = static_cast<std::uint8_t>((digest[6] & 0x0Fu) | 0x50u);
    digest[8] = static_cast<std::uint8_t>((digest[8] & 0x3Fu) | 0x80u);

    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    for (std::size_t i = 0; i < 8u; ++i) {
        hi = (hi << 8u) | static_cast<std::uint64_t>(digest[i]);
        lo = (lo << 8u) | static_cast<std::uint64_t>(digest[8u + i]);
    }
    return AssetGuid{hi, lo};
}

AssetGuid AssetGuid::fromTypeName(std::string_view canonicalTypeName) {
    return fromCanonicalName("type", trimAscii(canonicalTypeName));
}

AssetGuid AssetGuid::fromAssetPath(std::string_view assetPath) {
    const std::string norm = normalizeAssetPath(assetPath);
    return fromCanonicalName("asset", norm);
}

std::optional<AssetGuid> AssetGuid::parse(std::string_view uuidText) noexcept {
    uuidText = trimAscii(uuidText);
    if (uuidText.size() >= 2u && uuidText.front() == '{' && uuidText.back() == '}') {
        uuidText.remove_prefix(1u);
        uuidText.remove_suffix(1u);
    }

    std::array<std::uint8_t, 16> bytes{};
    if (uuidText.size() == 36u) {
        if (uuidText[8] != '-' || uuidText[13] != '-' ||
            uuidText[18] != '-' || uuidText[23] != '-') {
            return std::nullopt;
        }
        std::size_t byteIdx = 0;
        for (std::size_t pos = 0; pos < 36u;) {
            if (pos == 8u || pos == 13u || pos == 18u || pos == 23u) {
                ++pos;
                continue;
            }
            const int hi = parseHexNibble(uuidText[pos]);
            const int lo = parseHexNibble(uuidText[pos + 1u]);
            if (hi < 0 || lo < 0) return std::nullopt;
            bytes[byteIdx++] = static_cast<std::uint8_t>((hi << 4) | lo);
            pos += 2u;
        }
    } else if (uuidText.size() == 32u) {
        for (std::size_t i = 0; i < 16u; ++i) {
            const int hi = parseHexNibble(uuidText[i * 2u]);
            const int lo = parseHexNibble(uuidText[i * 2u + 1u]);
            if (hi < 0 || lo < 0) return std::nullopt;
            bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }
    } else {
        return std::nullopt;
    }

    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    for (std::size_t i = 0; i < 8u; ++i) {
        hi = (hi << 8u) | static_cast<std::uint64_t>(bytes[i]);
        lo = (lo << 8u) | static_cast<std::uint64_t>(bytes[8u + i]);
    }
    return AssetGuid{hi, lo};
}

std::string AssetGuid::toString() const {
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < 8u; ++i) {
        bytes[i] = static_cast<std::uint8_t>((high >> ((7u - i) * 8u)) & 0xFFu);
        bytes[8u + i] = static_cast<std::uint8_t>((low >> ((7u - i) * 8u)) & 0xFFu);
    }
    static constexpr const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(36u);
    for (std::size_t i = 0; i < 16u; ++i) {
        if (i == 4u || i == 6u || i == 8u || i == 10u) {
            out.push_back('-');
        }
        out.push_back(kHex[(bytes[i] >> 4u) & 0xFu]);
        out.push_back(kHex[bytes[i] & 0xFu]);
    }
    return out;
}

} // namespace realscript::text
